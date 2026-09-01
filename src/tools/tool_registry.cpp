#include "mint/tools/tool_registry.hpp"

#include "mint/domain/change_journal.hpp"
#include "mint/infrastructure/change_transaction_store.hpp"
#include "mint/infrastructure/command_runner.hpp"
#include "mint/infrastructure/diagnostic_log.hpp"
#include "mint/version.hpp"

#include "file_support.hpp"
#include "path_identity.hpp"
#include "tool_catalog.hpp"
#include "tool_contract.hpp"
#include "tool_registry_command.hpp"
#include "tool_support.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace mint {
namespace {

void upsert_workspace_risk(Json& changed_files, const std::string& path, std::string_view status) {
    for (auto entry = changed_files.begin(); entry != changed_files.end();) {
        if (entry->is_object() && entry->value("path", "") == path) {
            entry = changed_files.erase(entry);
        } else {
            ++entry;
        }
    }
    changed_files.push_back(
        {{"path", path}, {"status", status}, {"bytes_before", nullptr}, {"bytes_after", nullptr}});
}

std::optional<std::filesystem::path> lexical_absolute_path(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error || absolute.empty()) {
        return std::nullopt;
    }
    return absolute.lexically_normal();
}

void append_unique_path(std::vector<std::filesystem::path>& paths,
                        const std::filesystem::path& path) {
    const auto identity = tools::detail::path_entry_identity(path);
    if (std::find(paths.begin(), paths.end(), identity) == paths.end()) {
        paths.push_back(identity);
    }
}

} // namespace

using tools::detail::contains_ignored_component;
using tools::detail::contains_nul;
using tools::detail::display_path;
using tools::detail::dump_json;
using tools::detail::error_result;
using tools::detail::is_entry_within;
using tools::detail::is_valid_utf8;
using tools::detail::same_path_entry_identity;
using tools::detail::same_path_identity;

ToolRegistry::ToolRegistry(std::filesystem::path root, ToolRegistryOptions options)
    : allow_write_(options.allow_write), runtime_(options.runtime),
      task_control_(options.task_control),
      change_set_approval_(std::move(options.change_set_approval)),
      policy_fingerprint_(std::move(options.policy_fingerprint)) {
    validate_tool_runtime_settings(runtime_, "ToolRegistry runtime");
    std::error_code error;
    root_ = std::filesystem::weakly_canonical(std::move(root), error);
    if (error || root_.empty() || !std::filesystem::is_directory(root_)) {
        throw std::invalid_argument("工作目录不存在或不是目录");
    }
    root_ = tools::detail::path_entry_identity(root_);

    for (auto& path : options.protected_paths) {
        auto snapshot_path = lexical_absolute_path(path);
        if (snapshot_path.has_value()) {
            snapshot_path = tools::detail::path_entry_identity(*snapshot_path);
        }
        error.clear();
        auto resolved = std::filesystem::weakly_canonical(std::move(path), error);
        if (!error && !resolved.empty()) {
            protected_paths_.push_back(std::move(resolved));
            if (snapshot_path.has_value()) {
                append_unique_path(snapshot_protected_paths_, *snapshot_path);
            }
        }
    }

    if (!allow_write_ && !options.allowed_write_paths.empty()) {
        throw std::invalid_argument("写路径白名单需要先启用写入能力");
    }
    for (const auto& requested : options.allowed_write_paths) {
        if (requested.empty() || requested == "." || requested.is_absolute()) {
            throw std::invalid_argument("写路径白名单必须是工作区内的相对文件或目录");
        }
        const auto unresolved = root_ / requested;
        error.clear();
        const auto status = std::filesystem::symlink_status(unresolved, error);
        if (!error && std::filesystem::is_symlink(status)) {
            throw std::invalid_argument("写路径白名单不能指向符号链接");
        }
        error.clear();
        auto resolved = std::filesystem::weakly_canonical(unresolved, error);
        if (error || !is_entry_within(root_, resolved) ||
            contains_ignored_component(root_, resolved)) {
            throw std::invalid_argument("写路径白名单超出工作区或位于忽略目录");
        }
        const auto duplicate =
            std::find_if(write_scopes_.begin(), write_scopes_.end(), [&](const WriteScope& scope) {
                return same_path_entry_identity(scope.path, resolved);
            });
        if (duplicate != write_scopes_.end()) {
            continue;
        }
        error.clear();
        const bool recursive = std::filesystem::is_directory(resolved, error) && !error;
        write_scopes_.push_back({std::move(resolved), recursive});
        write_path_labels_.push_back(display_path(root_, write_scopes_.back().path));
    }

    const bool commands_enabled =
        !options.allowed_programs.empty() || !options.command_recipes.empty();
    if (commands_enabled) {
        command_workspace_state_ = std::make_unique<CommandWorkspaceState>();
    }
    if (allow_write_ || commands_enabled) {
        change_journal_ = std::make_unique<ChangeJournal>();
    }
    if (!options.change_transaction_path.empty()) {
        if (!allow_write_) {
            throw std::invalid_argument("changeset 事务日志需要先启用写入能力");
        }
        change_transaction_store_ =
            std::make_unique<ChangeTransactionStore>(std::move(options.change_transaction_path));
        protected_paths_.push_back(change_transaction_store_->path());
        protected_paths_.push_back(change_transaction_lock_path(change_transaction_store_->path()));
        if (const auto path = lexical_absolute_path(change_transaction_store_->path())) {
            append_unique_path(snapshot_protected_paths_, *path);
        }
        if (const auto path = lexical_absolute_path(
                change_transaction_lock_path(change_transaction_store_->path()))) {
            append_unique_path(snapshot_protected_paths_, *path);
        }
    }

    if (!options.allowed_programs.empty() && !options.command_recipes.empty()) {
        throw std::invalid_argument("原始命令授权和固定命令 recipe 不能同时启用");
    }
    if (!options.allowed_programs.empty() || !options.command_recipes.empty()) {
        auto command_denied_paths = protected_paths_;
        command_denied_paths.push_back(root_ / ".git");
        command_denied_paths.push_back(root_ / ".codex");
        command_denied_paths.push_back(root_ / ".agents");
        auto max_timeout_seconds = options.max_command_timeout_seconds;
        for (const auto& recipe : options.command_recipes) {
            max_timeout_seconds = std::max(max_timeout_seconds, recipe.timeout_seconds);
        }
        command_runner_ = std::make_unique<CommandRunner>(
            CommandRunnerOptions{.root = root_,
                                 .allowed_programs = std::move(options.allowed_programs),
                                 .recipes = std::move(options.command_recipes),
                                 .default_timeout_seconds = options.default_command_timeout_seconds,
                                 .max_timeout_seconds = max_timeout_seconds,
                                 .max_output_bytes = runtime_.command_output_bytes,
                                 .resource_limits = runtime_.command_resources,
                                 .task_control = task_control_,
                                 .approval = std::move(options.command_approval),
                                 .require_os_sandbox = options.require_command_sandbox,
                                 .read_only_paths = std::move(options.command_read_paths),
                                 .denied_read_paths = std::move(command_denied_paths)});
    }
}

ToolRegistry::~ToolRegistry() = default;

const std::filesystem::path& ToolRegistry::root() const noexcept {
    return root_;
}

bool ToolRegistry::can_write() const noexcept {
    return allow_write_;
}

const std::vector<std::string>& ToolRegistry::allowed_write_paths() const noexcept {
    return write_path_labels_;
}

bool ToolRegistry::can_run_commands() const noexcept {
    return command_runner_ != nullptr;
}

bool ToolRegistry::requires_command_approval() const noexcept {
    return command_runner_ != nullptr && command_runner_->requires_approval();
}

bool ToolRegistry::requires_change_set_approval() const noexcept {
    return static_cast<bool>(change_set_approval_);
}

bool ToolRegistry::commands_are_os_sandboxed() const noexcept {
    return command_runner_ != nullptr && command_runner_->is_os_sandboxed();
}

const std::string& ToolRegistry::command_sandbox_backend() const noexcept {
    static const std::string none = "none";
    return command_runner_ == nullptr ? none : command_runner_->sandbox_backend();
}

const std::vector<std::string>& ToolRegistry::allowed_programs() const noexcept {
    static const std::vector<std::string> empty;
    return command_runner_ == nullptr ? empty : command_runner_->allowed_programs();
}

const std::vector<std::string>& ToolRegistry::command_recipe_names() const noexcept {
    static const std::vector<std::string> empty;
    return command_runner_ == nullptr ? empty : command_runner_->recipe_names();
}

bool ToolRegistry::uses_command_recipes() const noexcept {
    return command_runner_ != nullptr && command_runner_->uses_recipes();
}

const std::string& ToolRegistry::policy_fingerprint() const noexcept {
    return policy_fingerprint_;
}

const ToolRuntimeSettings& ToolRegistry::runtime_settings() const noexcept {
    return runtime_;
}

ToolCapabilities ToolRegistry::capabilities() const {
    return {.workspace_root = root_,
            .write_enabled = can_write(),
            .writable_paths = allowed_write_paths(),
            .commands_enabled = can_run_commands(),
            .command_approval_required = requires_command_approval(),
            .change_set_approval_required = requires_change_set_approval(),
            .command_sandboxed = commands_are_os_sandboxed(),
            .command_sandbox_backend = command_sandbox_backend(),
            .allowed_programs = allowed_programs(),
            .command_recipes = command_recipe_names(),
            .policy_fingerprint = policy_fingerprint(),
            .runtime = runtime_settings(),
            .durable_change_transactions = has_durable_change_transactions(),
            .change_transaction_path = change_transaction_path()};
}

bool ToolRegistry::has_workspace_changes() const {
    return workspace_tracking_error_.has_value() || !unauditable_command_paths_.empty() ||
           !command_policy_violation_paths_.empty() ||
           (change_journal_ != nullptr && change_journal_->has_changes());
}

bool ToolRegistry::workspace_integrity_failed() const noexcept {
    return workspace_tracking_error_.has_value() || !unauditable_command_paths_.empty() ||
           !command_policy_violation_paths_.empty();
}

Json ToolRegistry::workspace_change_snapshot() const {
    if (change_journal_ == nullptr) {
        return {{"ok", true},
                {"changed_files", Json::array()},
                {"diff", ""},
                {"diff_truncated", false}};
    }
    auto result = change_journal_->snapshot();
    for (const auto& path : unauditable_command_paths_) {
        upsert_workspace_risk(result["changed_files"], path, "unauditable");
    }
    for (const auto& path : command_policy_violation_paths_) {
        upsert_workspace_risk(result["changed_files"], path, "policy_violation");
    }
    if (workspace_tracking_error_.has_value()) {
        upsert_workspace_risk(result["changed_files"], "<workspace>", "unauditable");
    }
    return result;
}

Json ToolRegistry::workspace_change_state() const {
    if (change_journal_ == nullptr) {
        return {{"schema_version", workspace_change_schema_version}, {"entries", Json::array()}};
    }
    auto result = change_journal_->state();
    result["schema_version"] = workspace_change_schema_version;
    if (!unauditable_command_paths_.empty()) {
        result["unauditable_command_paths"] = unauditable_command_paths_;
    }
    if (!command_policy_violation_paths_.empty()) {
        result["command_policy_violation_paths"] = command_policy_violation_paths_;
    }
    if (workspace_tracking_error_.has_value()) {
        result["workspace_tracking_error"] = *workspace_tracking_error_;
    }
    return result;
}

void ToolRegistry::restore_workspace_change_state(const Json& state) {
    const auto journal_schema = state.is_object() ? state.value("schema_version", 0) : 0;
    if (!state.is_object() ||
        (journal_schema != 1 && journal_schema != 2 &&
         journal_schema != workspace_change_schema_version) ||
        !state.contains("entries") || !state.at("entries").is_array()) {
        throw std::invalid_argument("会话中的变更日志格式无效");
    }
    const bool has_safety_state = state.contains("unauditable_command_paths") ||
                                  state.contains("command_policy_violation_paths") ||
                                  state.contains("workspace_tracking_error");
    if (journal_schema < workspace_change_schema_version && has_safety_state) {
        throw std::invalid_argument("命令污染状态需要使用新版变更日志 schema");
    }
    if (state.at("entries").size() > tools::contract::max_restored_changes) {
        throw std::invalid_argument("会话中的变更日志条目过多");
    }
    std::vector<std::string> unauditable_paths;
    std::vector<std::string> policy_violation_paths;
    std::optional<std::string> tracking_error;
    const auto restore_taint_paths = [&](const char* field, std::vector<std::string>& output) {
        if (!state.contains(field)) {
            return;
        }
        if (!state.at(field).is_array() ||
            state.at(field).size() > tools::contract::max_restored_changes) {
            throw std::invalid_argument("会话中的命令污染路径格式无效");
        }
        for (const auto& item : state.at(field)) {
            if (!item.is_string()) {
                throw std::invalid_argument("会话中的命令污染路径格式无效");
            }
            const auto requested = item.get<std::string>();
            const std::filesystem::path input(requested);
            if (input.empty() || input == "." || input.is_absolute()) {
                throw std::invalid_argument("会话中的命令污染路径无效: " + requested);
            }
            const auto target = resolve_inside_root(requested);
            if (is_protected(target) || contains_ignored_component(root_, target)) {
                throw std::invalid_argument("会话中的命令污染路径当前不可审计: " + requested);
            }
            if (std::find(output.begin(), output.end(), requested) == output.end()) {
                output.push_back(requested);
            }
        }
    };
    restore_taint_paths("unauditable_command_paths", unauditable_paths);
    restore_taint_paths("command_policy_violation_paths", policy_violation_paths);
    if (state.contains("workspace_tracking_error")) {
        if (!state.at("workspace_tracking_error").is_string() ||
            state.at("workspace_tracking_error").get_ref<const std::string&>() !=
                tools::detail::workspace_tracking_error_code) {
            throw std::invalid_argument("会话中的工作区跟踪错误格式无效");
        }
        tracking_error = state.at("workspace_tracking_error").get<std::string>();
    }
    if (change_journal_ == nullptr) {
        if (!state.at("entries").empty() || !unauditable_paths.empty() ||
            !policy_violation_paths.empty() || tracking_error.has_value()) {
            throw std::invalid_argument("恢复有文件变化的会话需要启用写入或命令能力");
        }
        return;
    }

    for (const auto& item : state.at("entries")) {
        if (!item.is_object() || !item.contains("path") || !item.at("path").is_string() ||
            !item.contains("before") || !item.at("before").is_string() || !item.contains("after") ||
            !item.at("after").is_string()) {
            throw std::invalid_argument("会话中的变更日志条目格式无效");
        }
        const auto requested = item.at("path").get<std::string>();
        const auto& before = item.at("before").get_ref<const std::string&>();
        const auto& expected = item.at("after").get_ref<const std::string&>();
        bool before_exists = true;
        bool after_exists = true;
        if (journal_schema == 1) {
            if (!item.contains("created") || !item.at("created").is_boolean()) {
                throw std::invalid_argument("会话中的 v1 变更日志条目格式无效");
            }
            before_exists = !item.at("created").get<bool>();
        } else {
            if (!item.contains("before_exists") || !item.at("before_exists").is_boolean() ||
                !item.contains("after_exists") || !item.at("after_exists").is_boolean()) {
                throw std::invalid_argument("会话中的 v2 变更日志条目格式无效");
            }
            before_exists = item.at("before_exists").get<bool>();
            after_exists = item.at("after_exists").get<bool>();
        }
        if (before.size() > runtime_bounds::max_edit_file_bytes ||
            expected.size() > runtime_bounds::max_edit_file_bytes || contains_nul(before) ||
            contains_nul(expected) || !is_valid_utf8(before) || !is_valid_utf8(expected) ||
            (!before_exists && !before.empty()) || (!after_exists && !expected.empty())) {
            throw std::invalid_argument("会话中的变更日志内容无效: " + requested);
        }

        const std::filesystem::path input(requested);
        if (input.empty() || input == "." || input.is_absolute()) {
            throw std::invalid_argument("会话中的变更路径无效: " + requested);
        }
        std::error_code error;
        const auto unresolved = root_ / input;
        const auto status = std::filesystem::symlink_status(unresolved, error);
        if (!error && std::filesystem::is_symlink(status)) {
            throw std::invalid_argument("会话变更路径是符号链接: " + requested);
        }
        error.clear();
        const auto target = resolve_inside_root(requested);
        if (is_protected(target) || contains_ignored_component(root_, target) ||
            !is_write_allowed(target)) {
            throw std::invalid_argument("会话变更路径当前不可恢复: " + requested);
        }
        const bool current_exists = std::filesystem::exists(target, error);
        if (error || current_exists != after_exists) {
            throw std::invalid_argument("会话检查点之后文件存在状态已被外部修改，拒绝恢复: " +
                                        requested);
        }
        if (!after_exists) {
            continue;
        }
        if (!std::filesystem::is_regular_file(target, error) || error) {
            throw std::invalid_argument("会话变更路径当前不是普通文件: " + requested);
        }
        const auto size = std::filesystem::file_size(target, error);
        if (error || size > runtime_bounds::max_edit_file_bytes) {
            throw std::invalid_argument("会话变更文件当前无法读取: " + requested);
        }
        std::ifstream input_stream(target, std::ios::binary);
        std::string current{std::istreambuf_iterator<char>(input_stream),
                            std::istreambuf_iterator<char>()};
        if ((!input_stream.eof() && input_stream.fail()) || current != expected) {
            throw std::invalid_argument("会话检查点之后文件已被外部修改，拒绝恢复: " + requested);
        }
    }
    const Json journal_state = {{"schema_version", journal_schema == 1 ? 1 : 2},
                                {"entries", state.at("entries")}};
    change_journal_->restore(journal_state);
    unauditable_command_paths_ = std::move(unauditable_paths);
    command_policy_violation_paths_ = std::move(policy_violation_paths);
    workspace_tracking_error_ = std::move(tracking_error);
}

bool ToolRegistry::is_protected(const std::filesystem::path& path) const {
    return std::any_of(
        protected_paths_.begin(), protected_paths_.end(),
        [&](const auto& protected_path) { return same_path_identity(protected_path, path); });
}

bool ToolRegistry::is_snapshot_entry_protected(const std::filesystem::path& path) const {
    const auto normalized = (path.is_absolute() ? path : root_ / path).lexically_normal();
    return std::find(snapshot_protected_paths_.begin(), snapshot_protected_paths_.end(),
                     normalized) != snapshot_protected_paths_.end();
}

bool ToolRegistry::is_write_allowed(const std::filesystem::path& path) const {
    if (contains_ignored_component(root_, path)) {
        return false;
    }
    if (write_scopes_.empty()) {
        return true;
    }
    for (const auto& scope : write_scopes_) {
        if (same_path_entry_identity(path, scope.path) ||
            (scope.recursive && is_entry_within(scope.path, path))) {
            return true;
        }
    }
    return false;
}

Json ToolRegistry::definitions() const {
    auto result = tools::detail::workspace_tool_definitions(allow_write_, runtime_);
    if (command_runner_ != nullptr) {
        result.push_back(command_runner_->definition());
    }
    return result;
}

std::string ToolRegistry::describe_call(const ToolCall& call) const {
    return tools::detail::summarize_tool_call(call);
}

Json ToolRegistry::execute_json(const ToolCall& call) const {
    if (!call.arguments.is_object()) {
        return error_result("工具参数必须是 JSON 对象");
    }
    if (call.name == "list_files") {
        return list_files(call.arguments);
    }
    if (call.name == "read_file") {
        return read_file(call.arguments);
    }
    if (call.name == "search_text") {
        return search_text(call.arguments);
    }
    if (call.name == "apply_patch") {
        if (command_workspace_state_ != nullptr) {
            command_workspace_state_->baseline.reset();
        }
        return allow_write_ ? apply_patch(call.arguments)
                            : error_result("写入能力未启用；请由用户使用 --allow-write 显式授权");
    }
    if (call.name == "apply_changeset") {
        if (command_workspace_state_ != nullptr) {
            command_workspace_state_->baseline.reset();
        }
        return allow_write_ ? apply_changeset(call.arguments)
                            : error_result("写入能力未启用；请由用户显式授权写路径");
    }
    if (call.name == "workspace_changes") {
        if (!allow_write_ || change_journal_ == nullptr) {
            return error_result("变更日志未启用；请由用户使用 --allow-write 显式授权");
        }
        tools::detail::require_only_fields(call.arguments, "workspace_changes", {});
        return workspace_change_snapshot();
    }
    if (call.name == "run_command" || call.name == "run_recipe") {
        return execute_command(call);
    }
    return error_result("未知工具: " + call.name);
}

std::string ToolRegistry::execute(const ToolCall& call) const {
    try {
        const auto result = execute_json(call);
        diagnostics::emit(diagnostics::Level::debug, "tool.completed",
                          {{"name", call.name}, {"ok", result.value("ok", false)}});
        return dump_json(result);
    } catch (const std::exception& error) {
        diagnostics::emit(diagnostics::Level::warning, "tool.failed", {{"name", call.name}});
        return dump_json(error_result(error.what()));
    }
}

} // namespace mint
