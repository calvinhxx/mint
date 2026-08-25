#include "aiagent/tools/tool_registry.hpp"

#include "aiagent/domain/change_journal.hpp"
#include "aiagent/infrastructure/command_runner.hpp"

#include "file_support.hpp"
#include "tool_support.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace aiagent {

using tools::detail::contains_ignored_component;
using tools::detail::contains_nul;
using tools::detail::display_path;
using tools::detail::dump_json;
using tools::detail::error_result;
using tools::detail::is_inside;
using tools::detail::is_valid_utf8;
using tools::detail::max_edit_file_bytes;
using tools::detail::max_read_bytes;

ToolRegistry::ToolRegistry(std::filesystem::path root, ToolRegistryOptions options)
    : allow_write_(options.allow_write),
      change_set_approval_(std::move(options.change_set_approval)),
      policy_fingerprint_(std::move(options.policy_fingerprint)) {
    std::error_code error;
    root_ = std::filesystem::weakly_canonical(std::move(root), error);
    if (error || root_.empty() || !std::filesystem::is_directory(root_)) {
        throw std::invalid_argument("工作目录不存在或不是目录");
    }

    for (auto& path : options.protected_paths) {
        error.clear();
        auto resolved = std::filesystem::weakly_canonical(std::move(path), error);
        if (!error && !resolved.empty()) {
            protected_paths_.push_back(std::move(resolved));
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
        const auto resolved = std::filesystem::weakly_canonical(unresolved, error);
        if (error || !is_inside(root_, resolved) || contains_ignored_component(root_, resolved)) {
            throw std::invalid_argument("写路径白名单超出工作区或位于忽略目录");
        }
        if (std::find(allowed_write_paths_.begin(), allowed_write_paths_.end(), resolved) !=
            allowed_write_paths_.end()) {
            continue;
        }
        error.clear();
        const bool recursive = std::filesystem::is_directory(resolved, error) && !error;
        allowed_write_paths_.push_back(resolved);
        recursive_write_paths_.push_back(recursive);
        write_path_labels_.push_back(display_path(root_, resolved));
    }

    if (allow_write_) {
        change_journal_ = std::make_unique<ChangeJournal>();
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
                                 .max_output_bytes = options.max_command_output_bytes,
                                 .task_control = std::move(options.task_control),
                                 .approval = std::move(options.command_approval),
                                 .require_os_sandbox = options.require_command_sandbox,
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

bool ToolRegistry::has_workspace_changes() const {
    return change_journal_ != nullptr && change_journal_->has_changes();
}

Json ToolRegistry::workspace_change_snapshot() const {
    if (change_journal_ == nullptr) {
        return {{"ok", true},
                {"changed_files", Json::array()},
                {"diff", ""},
                {"diff_truncated", false}};
    }
    return change_journal_->snapshot();
}

Json ToolRegistry::workspace_change_state() const {
    if (change_journal_ == nullptr) {
        return {{"schema_version", 2}, {"entries", Json::array()}};
    }
    return change_journal_->state();
}

void ToolRegistry::restore_workspace_change_state(const Json& state) {
    const auto journal_schema = state.is_object() ? state.value("schema_version", 0) : 0;
    if (!state.is_object() || (journal_schema != 1 && journal_schema != 2) ||
        !state.contains("entries") || !state.at("entries").is_array()) {
        throw std::invalid_argument("会话中的变更日志格式无效");
    }
    if (state.at("entries").size() > 1024) {
        throw std::invalid_argument("会话中的变更日志条目过多");
    }
    if (change_journal_ == nullptr) {
        if (!state.at("entries").empty()) {
            throw std::invalid_argument("恢复有文件变化的会话需要启用 --allow-write");
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
        if (before.size() > max_edit_file_bytes || expected.size() > max_edit_file_bytes ||
            contains_nul(before) || contains_nul(expected) || !is_valid_utf8(before) ||
            !is_valid_utf8(expected) || (!before_exists && !before.empty()) ||
            (!after_exists && !expected.empty())) {
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
        if (error || size > max_edit_file_bytes) {
            throw std::invalid_argument("会话变更文件当前无法读取: " + requested);
        }
        std::ifstream input_stream(target, std::ios::binary);
        std::string current{std::istreambuf_iterator<char>(input_stream),
                            std::istreambuf_iterator<char>()};
        if ((!input_stream.eof() && input_stream.fail()) || current != expected) {
            throw std::invalid_argument("会话检查点之后文件已被外部修改，拒绝恢复: " + requested);
        }
    }
    change_journal_->restore(state);
}

bool ToolRegistry::is_protected(const std::filesystem::path& path) const {
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return false;
    }
    return std::find(protected_paths_.begin(), protected_paths_.end(), resolved) !=
           protected_paths_.end();
}

bool ToolRegistry::is_write_allowed(const std::filesystem::path& path) const {
    if (contains_ignored_component(root_, path)) {
        return false;
    }
    if (allowed_write_paths_.empty()) {
        return true;
    }
    for (std::size_t index = 0; index < allowed_write_paths_.size(); ++index) {
        if (path == allowed_write_paths_.at(index) ||
            (recursive_write_paths_.at(index) && is_inside(allowed_write_paths_.at(index), path))) {
            return true;
        }
    }
    return false;
}

Json ToolRegistry::definitions() const {
    Json result = Json::array(
        {{{"type", "function"},
          {"function",
           {{"name", "list_files"},
            {"description",
             "List files and directories below the allowed workspace root. Use relative paths."},
            {"parameters",
             {{"type", "object"},
              {"properties",
               {{"path",
                 {{"type", "string"},
                  {"description", "Relative directory path. Defaults to the workspace root."}}},
                {"max_depth",
                 {{"type", "integer"},
                  {"minimum", 1},
                  {"maximum", 4},
                  {"description", "How many directory levels to include. Defaults to 2."}}}}},
              {"additionalProperties", false}}}}}},
         {{"type", "function"},
          {"function",
           {{"name", "read_file"},
            {"description",
             "Read a text file inside the allowed workspace root. Large files are truncated."},
            {"parameters",
             {{"type", "object"},
              {"properties",
               {{"path",
                 {{"type", "string"}, {"description", "Relative path of the text file to read."}}},
                {"offset",
                 {{"type", "integer"},
                  {"minimum", 0},
                  {"description", "Byte offset to start from. Defaults to 0."}}},
                {"max_bytes",
                 {{"type", "integer"},
                  {"minimum", 1024},
                  {"maximum", max_read_bytes},
                  {"description", "Maximum bytes to return. Defaults to 16384. Use next_offset for "
                                  "another chunk."}}}}},
              {"required", Json::array({"path"})},
              {"additionalProperties", false}}}}}},
         {{"type", "function"},
          {"function",
           {{"name", "search_text"},
            {"description",
             "Search text files inside the allowed workspace root and return matching lines."},
            {"parameters",
             {{"type", "object"},
              {"properties",
               {{"query",
                 {{"type", "string"},
                  {"description", "Literal text to search for, not a regular expression."}}},
                {"path",
                 {{"type", "string"},
                  {"description",
                   "Relative file or directory path. Defaults to the workspace root."}}},
                {"case_sensitive",
                 {{"type", "boolean"},
                  {"description",
                   "Whether ASCII letter matching is case-sensitive. Defaults to false."}}}}},
              {"required", Json::array({"query"})},
              {"additionalProperties", false}}}}}}});

    if (allow_write_) {
        result.push_back(
            {{"type", "function"},
             {"function",
              {{"name", "apply_patch"},
               {"description",
                "Create one text file or replace one exact, unique text block in an existing file. "
                "Read existing files first. This tool cannot delete files or run commands."},
               {"parameters",
                {{"type", "object"},
                 {"properties",
                  {{"path",
                    {{"type", "string"},
                     {"description", "Relative path inside the allowed workspace root."}}},
                   {"operation",
                    {{"type", "string"},
                     {"enum", Json::array({"replace", "create"})},
                     {"description",
                      "Use replace for an existing file or create for a new file."}}},
                   {"old_text",
                    {{"type", "string"},
                     {"description",
                      "For replace: the non-empty text block that must occur exactly once."}}},
                   {"new_text",
                    {{"type", "string"},
                     {"description",
                      "Complete replacement block, or complete contents for a new file."}}}}},
                 {"required", Json::array({"path", "operation", "new_text"})},
                 {"additionalProperties", false}}}}}});
        result.push_back(
            {{"type", "function"},
             {"function",
              {{"name", "apply_changeset"},
               {"description",
                "Apply 1-16 validated text-file changes as one rollback transaction. Supports "
                "create, exact replace, delete, and move. Delete/move require old_text to equal "
                "the complete current file. Use only these exact fields per item: "
                "create(operation,path,new_text), "
                "replace(operation,path,old_text,new_text), "
                "delete(operation,path,old_text), or "
                "move(operation,path,old_text,destination). "
                "All paths are validated before approval and commit."},
               {"parameters",
                {{"type", "object"},
                 {"properties",
                  {{"changes",
                    {{"type", "array"},
                     {"minItems", 1},
                     {"maxItems", 16},
                     {"items",
                      {{"type", "object"},
                       {"properties",
                        {{"operation",
                          {{"type", "string"},
                           {"enum", Json::array({"create", "replace", "delete", "move"})}}},
                         {"path", {{"type", "string"}}},
                         {"old_text", {{"type", "string"}}},
                         {"new_text", {{"type", "string"}}},
                         {"destination", {{"type", "string"}}}}},
                       {"required", Json::array({"operation", "path"})},
                       {"additionalProperties", false}}}}}}},
                 {"required", Json::array({"changes"})},
                 {"additionalProperties", false}}}}}});
        result.push_back({{"type", "function"},
                          {"function",
                           {{"name", "workspace_changes"},
                            {"description", "Return every file changed by file-edit tools in this "
                                            "session and a bounded unified diff "
                                            "from the original contents to the current contents."},
                            {"parameters",
                             {{"type", "object"},
                              {"properties", Json::object()},
                              {"additionalProperties", false}}}}}});
    }
    if (command_runner_ != nullptr) {
        result.push_back(command_runner_->definition());
    }
    return result;
}

std::string ToolRegistry::describe_call(const ToolCall& call) const {
    if (!call.arguments.is_object()) {
        return call.arguments.dump();
    }

    if (call.name == "run_recipe") {
        Json summary = Json::object();
        if (call.arguments.contains("recipe") && call.arguments.at("recipe").is_string()) {
            summary["recipe"] = call.arguments.at("recipe");
        }
        return summary.dump();
    }

    if (call.name == "run_command") {
        Json summary = Json::object();
        if (call.arguments.contains("program") && call.arguments.at("program").is_string()) {
            summary["program"] = call.arguments.at("program");
        }
        if (call.arguments.contains("cwd") && call.arguments.at("cwd").is_string()) {
            summary["cwd"] = call.arguments.at("cwd");
        }
        if (call.arguments.contains("timeout_seconds") &&
            call.arguments.at("timeout_seconds").is_number_integer()) {
            summary["timeout_seconds"] = call.arguments.at("timeout_seconds");
        }
        if (call.arguments.contains("args") && call.arguments.at("args").is_array()) {
            summary["arg_count"] = call.arguments.at("args").size();
        }
        return summary.dump();
    }

    if (call.name == "apply_changeset") {
        Json summary = Json::object();
        if (!call.arguments.contains("changes") || !call.arguments.at("changes").is_array()) {
            return summary.dump();
        }
        summary["operation_count"] = call.arguments.at("changes").size();
        Json paths = Json::array();
        for (const auto& item : call.arguments.at("changes")) {
            if (!item.is_object()) {
                continue;
            }
            Json change = Json::object();
            for (const auto* field : {"operation", "path", "destination"}) {
                if (item.contains(field) && item.at(field).is_string()) {
                    change[field] = item.at(field);
                }
            }
            if (item.contains("old_text") && item.at("old_text").is_string()) {
                change["old_bytes"] = item.at("old_text").get_ref<const std::string&>().size();
            }
            if (item.contains("new_text") && item.at("new_text").is_string()) {
                change["new_bytes"] = item.at("new_text").get_ref<const std::string&>().size();
            }
            paths.push_back(std::move(change));
        }
        summary["changes"] = std::move(paths);
        return summary.dump();
    }

    if (call.name != "apply_patch") {
        return call.arguments.dump();
    }

    Json summary = Json::object();
    if (call.arguments.contains("path") && call.arguments.at("path").is_string()) {
        summary["path"] = call.arguments.at("path");
    }
    if (call.arguments.contains("operation") && call.arguments.at("operation").is_string()) {
        summary["operation"] = call.arguments.at("operation");
    }
    if (call.arguments.contains("old_text") && call.arguments.at("old_text").is_string()) {
        summary["old_bytes"] = call.arguments.at("old_text").get_ref<const std::string&>().size();
    }
    if (call.arguments.contains("new_text") && call.arguments.at("new_text").is_string()) {
        summary["new_bytes"] = call.arguments.at("new_text").get_ref<const std::string&>().size();
    }
    return summary.dump();
}

std::string ToolRegistry::execute(const ToolCall& call) const {
    try {
        if (!call.arguments.is_object()) {
            return dump_json(error_result("工具参数必须是 JSON 对象"));
        }
        if (call.name == "list_files") {
            return dump_json(list_files(call.arguments));
        }
        if (call.name == "read_file") {
            return dump_json(read_file(call.arguments));
        }
        if (call.name == "search_text") {
            return dump_json(search_text(call.arguments));
        }
        if (call.name == "apply_patch") {
            if (!allow_write_) {
                return dump_json(
                    error_result("写入能力未启用；请由用户使用 --allow-write 显式授权"));
            }
            return dump_json(apply_patch(call.arguments));
        }
        if (call.name == "apply_changeset") {
            if (!allow_write_) {
                return dump_json(error_result("写入能力未启用；请由用户显式授权写路径"));
            }
            return dump_json(apply_changeset(call.arguments));
        }
        if (call.name == "workspace_changes") {
            if (change_journal_ == nullptr) {
                return dump_json(
                    error_result("变更日志未启用；请由用户使用 --allow-write 显式授权"));
            }
            if (!call.arguments.empty()) {
                return dump_json(error_result("workspace_changes 不接受参数"));
            }
            return dump_json(change_journal_->snapshot());
        }
        if (call.name == "run_command" || call.name == "run_recipe") {
            if (command_runner_ == nullptr) {
                return dump_json(
                    error_result("命令执行未启用；请由用户显式授权程序或 task policy recipe"));
            }
            if ((call.name == "run_recipe") != command_runner_->uses_recipes()) {
                return dump_json(error_result("命令工具与当前授权模式不匹配"));
            }
            return dump_json(command_runner_->run(call.arguments));
        }
        return dump_json(error_result("未知工具: " + call.name));
    } catch (const std::exception& error) {
        return dump_json(error_result(error.what()));
    }
}

} // namespace aiagent
