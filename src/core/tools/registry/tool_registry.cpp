#include "mint/tools/tool_registry.hpp"

#include "mint/domain/change_journal.hpp"
#include "mint/infrastructure/change_transaction_store.hpp"
#include "mint/infrastructure/command_runner.hpp"
#include "mint/infrastructure/diagnostic_log.hpp"
#include "mint/localization/localization.hpp"
#include "mint/runtime/task_control.hpp"

#include "tool_arguments.hpp"
#include "tool_catalog.hpp"
#include "tool_contract.hpp"
#include "tool_names.hpp"
#include "tool_registry_command.hpp"
#include "workspace/file_support.hpp"
#include "workspace/path_identity.hpp"
#include "workspace/sensitive_path.hpp"
#include "workspace/workspace_support.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mint {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

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
using tools::detail::display_path;
using tools::detail::dump_json;
using tools::detail::error_result;
using tools::detail::is_entry_within;
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
        throw std::invalid_argument(message(Message::tools_registry_invalid_root));
    }
    root_ = tools::detail::path_entry_identity(root_);

    for (auto& path : options.protected_paths) {
        if (path.empty()) {
            continue;
        }
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
        throw std::invalid_argument(message(Message::tools_registry_write_scope_requires_write));
    }
    for (const auto& requested : options.allowed_write_paths) {
        if (requested.empty() || requested == "." || requested.is_absolute()) {
            throw std::invalid_argument(message(Message::tools_registry_write_scope_relative));
        }
        const auto unresolved = root_ / requested;
        error.clear();
        const auto status = std::filesystem::symlink_status(unresolved, error);
        if (!error && std::filesystem::is_symlink(status)) {
            throw std::invalid_argument(message(Message::tools_registry_write_scope_symlink));
        }
        error.clear();
        auto resolved = std::filesystem::weakly_canonical(unresolved, error);
        if (error || !is_entry_within(root_, resolved) ||
            contains_ignored_component(root_, resolved)) {
            throw std::invalid_argument(message(Message::tools_registry_write_scope_outside));
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
        auto sensitive =
            tools::detail::scan_sensitive_paths(root_, runtime_.workspace_snapshot_entries);
        if (sensitive.truncated) {
            throw std::invalid_argument(message(Message::tools_registry_sensitive_scan_limit));
        }
        for (const auto& path : sensitive.paths) {
            append_unique_path(protected_paths_, path);
            append_unique_path(snapshot_protected_paths_, path);
        }
    }
    if (commands_enabled) {
        command_workspace_state_ = std::make_unique<CommandWorkspaceState>();
    }
    if (allow_write_ || commands_enabled) {
        change_journal_ = std::make_unique<ChangeJournal>();
    }
    if (!options.change_transaction_path.empty()) {
        if (!allow_write_) {
            throw std::invalid_argument(
                message(Message::tools_registry_transaction_requires_write));
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
        throw std::invalid_argument(message(Message::tools_registry_command_mode_conflict));
    }
    if (!options.allowed_programs.empty() || !options.command_recipes.empty()) {
        auto command_denied_paths = protected_paths_;
        command_denied_paths.push_back(root_ / ".git");
        command_denied_paths.push_back(root_ / ".codex");
        command_denied_paths.push_back(root_ / ".agents");
        command_denied_paths.push_back(root_ / ".husky");
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

bool ToolRegistry::is_protected(const std::filesystem::path& path) const {
    if (tools::detail::has_multiple_hard_links(path)) {
        return true;
    }
    return std::any_of(protected_paths_.begin(), protected_paths_.end(),
                       [&](const auto& protected_path) {
                           return same_path_identity(protected_path, path) ||
                                  is_entry_within(protected_path, path);
                       });
}

bool ToolRegistry::is_snapshot_entry_protected(const std::filesystem::path& path) const {
    const auto normalized = (path.is_absolute() ? path : root_ / path).lexically_normal();
    return std::find(snapshot_protected_paths_.begin(), snapshot_protected_paths_.end(),
                     normalized) != snapshot_protected_paths_.end();
}

bool ToolRegistry::is_write_allowed(const std::filesystem::path& path) const {
    if (contains_ignored_component(root_, path) || tools::detail::is_sensitive_path(root_, path) ||
        tools::detail::is_reserved_write_path(root_, path)) {
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

void ToolRegistry::check_task_control() const {
    if (task_control_ != nullptr && task_control_->should_stop()) {
        throw std::runtime_error(message(task_control_->cancellation_requested()
                                             ? Message::tools_call_cancelled
                                             : Message::tools_call_timed_out));
    }
}

std::string ToolRegistry::describe_call(const ToolCall& call) const {
    return tools::detail::summarize_tool_call(call);
}

Json ToolRegistry::execute_json(const ToolCall& call) const {
    if (!call.arguments.is_object()) {
        return error_result(message(Message::tools_call_arguments_object));
    }
    if (call.name == tools::name::list_files) {
        return list_files(call.arguments);
    }
    if (call.name == tools::name::read_file) {
        return read_file(call.arguments);
    }
    if (call.name == tools::name::search_text) {
        return search_text(call.arguments);
    }
    if (call.name == tools::name::apply_patch) {
        if (command_workspace_state_ != nullptr) {
            command_workspace_state_->baseline.reset();
        }
        return allow_write_ ? apply_patch(call.arguments)
                            : error_result(message(Message::tools_call_write_disabled));
    }
    if (call.name == tools::name::apply_changeset) {
        if (command_workspace_state_ != nullptr) {
            command_workspace_state_->baseline.reset();
        }
        return allow_write_ ? apply_changeset(call.arguments)
                            : error_result(message(Message::tools_call_write_path_disabled));
    }
    if (call.name == tools::name::workspace_changes) {
        if (!allow_write_ || change_journal_ == nullptr) {
            return error_result(message(Message::tools_call_journal_disabled));
        }
        tools::detail::require_only_fields(call.arguments, tools::name::workspace_changes, {});
        return workspace_change_snapshot();
    }
    if (call.name == tools::name::run_command || call.name == tools::name::run_recipe) {
        return execute_command(call);
    }
    return error_result(message(Message::tools_call_unknown, {arg(Placeholder::name, call.name)}));
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
