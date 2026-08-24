#include "aiagent/tools/tool_registry.hpp"

#include "aiagent/domain/change_journal.hpp"
#include "aiagent/infrastructure/command_runner.hpp"

#include "file_support.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aiagent {
namespace {

constexpr std::uintmax_t default_read_bytes = 16 * 1024;
constexpr std::uintmax_t max_read_bytes = 64 * 1024;
constexpr std::uintmax_t max_search_file_bytes = 1024 * 1024;
constexpr std::uintmax_t max_edit_file_bytes = tools::detail::max_edit_file_bytes;
constexpr std::size_t max_list_entries = 200;
constexpr std::size_t max_search_hits = 100;
constexpr std::size_t max_search_files = 2000;

using tools::detail::contains_nul;
using tools::detail::display_path;
using tools::detail::is_valid_utf8;
using tools::detail::replace_file_safely;

std::string dump_json(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Json error_result(std::string message) {
    return {{"ok", false}, {"error", std::move(message)}};
}

std::string require_string(const Json& arguments, std::string_view name) {
    const std::string key(name);
    if (!arguments.contains(key) || !arguments.at(key).is_string()) {
        throw std::invalid_argument("参数 " + key + " 必须是字符串");
    }
    return arguments.at(key).get<std::string>();
}

bool is_inside(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() || *root_part != *candidate_part) {
            return false;
        }
    }
    return true;
}

bool is_ignored_directory(const std::filesystem::path& path) {
    static const std::unordered_set<std::string> ignored = {
        ".aiagent", ".git", ".cache", "build", "dist", "node_modules", "target", "__pycache__"};
    const auto name = path.filename().string();
    return ignored.contains(name) || name.starts_with("cmake-build-");
}

bool contains_ignored_component(const std::filesystem::path& root,
                                const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(root);
    for (const auto& component : relative) {
        if (component == "." || component == "..") {
            continue;
        }
        if (is_ignored_directory(component)) {
            return true;
        }
    }
    return false;
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string shorten_line(std::string line) {
    constexpr std::size_t max_line_length = 400;
    if (line.size() > max_line_length) {
        line.resize(max_line_length);
        line += "...";
    }
    return line;
}

} // namespace

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

std::filesystem::path ToolRegistry::resolve_inside_root(const std::string& requested) const {
    const std::filesystem::path input = requested.empty() ? "." : requested;
    const auto candidate = input.is_absolute() ? input : root_ / input;

    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(candidate, error);
    if (error) {
        throw std::runtime_error("无法解析路径: " + requested);
    }
    if (!is_inside(root_, resolved)) {
        throw std::runtime_error("拒绝访问工作目录之外的路径: " + requested);
    }
    return resolved;
}

Json ToolRegistry::list_files(const Json& arguments) const {
    std::string requested = ".";
    if (arguments.contains("path")) {
        requested = require_string(arguments, "path");
    }

    int max_depth = 2;
    if (arguments.contains("max_depth")) {
        if (!arguments.at("max_depth").is_number_integer()) {
            throw std::invalid_argument("参数 max_depth 必须是整数");
        }
        max_depth = arguments.at("max_depth").get<int>();
    }
    if (max_depth < 1 || max_depth > 4) {
        throw std::invalid_argument("max_depth 必须在 1 到 4 之间");
    }

    const auto target = resolve_inside_root(requested);
    if (is_protected(target)) {
        return error_result("拒绝访问受保护的配置文件: " + requested);
    }
    if (contains_ignored_component(root_, target)) {
        return error_result("拒绝访问忽略目录中的路径: " + requested);
    }
    std::error_code error;
    if (!std::filesystem::exists(target, error)) {
        return error_result("路径不存在: " + requested);
    }

    Json entries = Json::array();
    if (std::filesystem::is_regular_file(target, error)) {
        entries.push_back({{"path", display_path(root_, target)},
                           {"type", "file"},
                           {"size", std::filesystem::file_size(target, error)}});
        return {{"ok", true}, {"entries", entries}, {"truncated", false}};
    }
    if (!std::filesystem::is_directory(target, error)) {
        return error_result("目标不是普通文件或目录: " + requested);
    }

    bool truncated = false;
    std::filesystem::recursive_directory_iterator iterator(
        target, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;

    while (iterator != end) {
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }

        const auto path = iterator->path();
        if (is_protected(path)) {
            iterator.increment(error);
            continue;
        }
        const auto status = iterator->symlink_status(error);
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }

        const bool is_directory = std::filesystem::is_directory(status);
        if (is_directory && is_ignored_directory(path)) {
            iterator.disable_recursion_pending();
            iterator.increment(error);
            continue;
        }
        if (is_directory && iterator.depth() + 1 >= max_depth) {
            iterator.disable_recursion_pending();
        }

        std::string type = "other";
        if (std::filesystem::is_symlink(status)) {
            type = "symlink";
            iterator.disable_recursion_pending();
        } else if (is_directory) {
            type = "directory";
        } else if (std::filesystem::is_regular_file(status)) {
            type = "file";
        }

        Json entry = {{"path", display_path(root_, path)}, {"type", type}};
        if (type == "file") {
            const auto size = std::filesystem::file_size(path, error);
            if (!error) {
                entry["size"] = size;
            }
            error.clear();
        }
        entries.push_back(std::move(entry));

        if (entries.size() >= max_list_entries) {
            truncated = true;
            break;
        }
        iterator.increment(error);
    }

    std::sort(entries.begin(), entries.end(), [](const Json& left, const Json& right) {
        return left.at("path").get<std::string>() < right.at("path").get<std::string>();
    });
    return {{"ok", true},
            {"root", display_path(root_, target)},
            {"entries", std::move(entries)},
            {"truncated", truncated}};
}

Json ToolRegistry::read_file(const Json& arguments) const {
    const auto requested = require_string(arguments, "path");
    const auto path = resolve_inside_root(requested);
    if (is_protected(path)) {
        return error_result("拒绝访问受保护的配置文件: " + requested);
    }
    if (contains_ignored_component(root_, path)) {
        return error_result("拒绝访问忽略目录中的路径: " + requested);
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return error_result("文件不存在或不是普通文件: " + requested);
    }

    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return error_result("无法获取文件大小: " + requested);
    }

    std::uintmax_t offset = 0;
    if (arguments.contains("offset")) {
        if (!arguments.at("offset").is_number_integer()) {
            throw std::invalid_argument("参数 offset 必须是非负整数");
        }
        const auto parsed = arguments.at("offset").get<long long>();
        if (parsed < 0) {
            throw std::invalid_argument("参数 offset 必须是非负整数");
        }
        offset = static_cast<std::uintmax_t>(parsed);
    }
    if (offset > size) {
        return error_result("offset 超过文件大小: " + requested);
    }

    std::uintmax_t requested_bytes = default_read_bytes;
    if (arguments.contains("max_bytes")) {
        if (!arguments.at("max_bytes").is_number_integer()) {
            throw std::invalid_argument("参数 max_bytes 必须是整数");
        }
        const auto parsed = arguments.at("max_bytes").get<long long>();
        if (parsed < 1024 || parsed > static_cast<long long>(max_read_bytes)) {
            throw std::invalid_argument("参数 max_bytes 必须在 1024 到 65536 之间");
        }
        requested_bytes = static_cast<std::uintmax_t>(parsed);
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return error_result("无法读取文件: " + requested);
    }
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        return error_result("无法定位文件读取位置: " + requested);
    }

    const auto bytes_to_read = static_cast<std::size_t>(std::min(size - offset, requested_bytes));
    std::string content(bytes_to_read, '\0');
    input.read(content.data(), static_cast<std::streamsize>(bytes_to_read));
    content.resize(static_cast<std::size_t>(input.gcount()));
    if (contains_nul(content)) {
        return error_result("拒绝读取疑似二进制文件: " + requested);
    }

    if (offset + content.size() < size && content.size() > 4096) {
        const auto last_newline = content.rfind('\n');
        if (last_newline != std::string::npos && last_newline + 1 >= content.size() / 2) {
            content.resize(last_newline + 1);
        }
    }
    const auto next_offset = offset + content.size();
    const auto truncated = next_offset < size;

    return {{"ok", true},
            {"path", display_path(root_, path)},
            {"size", size},
            {"offset", offset},
            {"next_offset", truncated ? Json(next_offset) : Json(nullptr)},
            {"truncated", truncated},
            {"content", std::move(content)}};
}

Json ToolRegistry::search_text(const Json& arguments) const {
    const auto query = require_string(arguments, "query");
    if (query.empty()) {
        throw std::invalid_argument("搜索内容不能为空");
    }
    if (query.size() > 256) {
        throw std::invalid_argument("搜索内容不能超过 256 字节");
    }

    std::string requested = ".";
    if (arguments.contains("path")) {
        requested = require_string(arguments, "path");
    }
    bool case_sensitive = false;
    if (arguments.contains("case_sensitive")) {
        if (!arguments.at("case_sensitive").is_boolean()) {
            throw std::invalid_argument("参数 case_sensitive 必须是布尔值");
        }
        case_sensitive = arguments.at("case_sensitive").get<bool>();
    }

    const auto target = resolve_inside_root(requested);
    if (is_protected(target)) {
        return error_result("拒绝访问受保护的配置文件: " + requested);
    }
    if (contains_ignored_component(root_, target)) {
        return error_result("拒绝访问忽略目录中的路径: " + requested);
    }
    std::error_code error;
    if (!std::filesystem::exists(target, error)) {
        return error_result("路径不存在: " + requested);
    }

    Json hits = Json::array();
    std::size_t scanned_files = 0;
    bool truncated = false;
    const auto comparable_query = case_sensitive ? query : lowercase_ascii(query);

    const auto scan_file = [&](const std::filesystem::path& path) {
        if (hits.size() >= max_search_hits || scanned_files >= max_search_files) {
            truncated = true;
            return;
        }
        if (is_protected(path)) {
            return;
        }

        std::error_code file_error;
        if (!std::filesystem::is_regular_file(path, file_error)) {
            return;
        }
        const auto size = std::filesystem::file_size(path, file_error);
        if (file_error || size > max_search_file_bytes) {
            return;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return;
        }
        std::string probe(4096, '\0');
        input.read(probe.data(), static_cast<std::streamsize>(probe.size()));
        probe.resize(static_cast<std::size_t>(input.gcount()));
        if (contains_nul(probe)) {
            return;
        }
        input.clear();
        input.seekg(0);
        ++scanned_files;

        std::string line;
        std::size_t line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            const auto comparable_line = case_sensitive ? line : lowercase_ascii(line);
            if (comparable_line.find(comparable_query) == std::string::npos) {
                continue;
            }
            hits.push_back({{"path", display_path(root_, path)},
                            {"line", line_number},
                            {"text", shorten_line(line)}});
            if (hits.size() >= max_search_hits) {
                truncated = true;
                return;
            }
        }
    };

    if (std::filesystem::is_regular_file(target, error)) {
        scan_file(target);
    } else if (std::filesystem::is_directory(target, error)) {
        std::filesystem::recursive_directory_iterator iterator(
            target, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        while (iterator != end && !truncated) {
            if (error) {
                error.clear();
                iterator.increment(error);
                continue;
            }

            const auto status = iterator->symlink_status(error);
            if (error) {
                error.clear();
                iterator.increment(error);
                continue;
            }
            if (std::filesystem::is_symlink(status)) {
                iterator.disable_recursion_pending();
            } else if (std::filesystem::is_directory(status)) {
                if (is_ignored_directory(iterator->path())) {
                    iterator.disable_recursion_pending();
                }
            } else if (std::filesystem::is_regular_file(status)) {
                scan_file(iterator->path());
            }
            iterator.increment(error);
        }
    } else {
        return error_result("目标不是普通文件或目录: " + requested);
    }

    return {{"ok", true},
            {"query", query},
            {"scanned_files", scanned_files},
            {"hits", std::move(hits)},
            {"truncated", truncated}};
}

Json ToolRegistry::apply_patch(const Json& arguments) const {
    const auto requested = require_string(arguments, "path");
    const auto operation = require_string(arguments, "operation");
    const auto new_text = require_string(arguments, "new_text");

    const std::filesystem::path input(requested);
    if (input.empty() || input == "." || input.is_absolute()) {
        throw std::invalid_argument("apply_patch 的 path 必须是工作目录内的相对文件路径");
    }
    if (new_text.size() > max_edit_file_bytes) {
        throw std::invalid_argument("new_text 不能超过 256 KiB");
    }
    if (contains_nul(new_text) || !is_valid_utf8(new_text)) {
        throw std::invalid_argument("apply_patch 只支持有效 UTF-8 文本内容");
    }

    const auto unresolved = root_ / input;
    std::error_code status_error;
    const auto unresolved_status = std::filesystem::symlink_status(unresolved, status_error);
    if (!status_error && std::filesystem::is_symlink(unresolved_status)) {
        throw std::runtime_error("apply_patch 拒绝直接修改符号链接: " + requested);
    }

    const auto target = resolve_inside_root(requested);
    if (!is_write_allowed(target)) {
        return error_result("写入路径未获用户 --allow-write-path 授权: " + requested);
    }
    if (is_protected(target)) {
        return error_result("拒绝修改受保护的配置文件: " + requested);
    }
    if (contains_ignored_component(root_, target)) {
        return error_result("拒绝修改忽略目录中的路径: " + requested);
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(target, error);
    if (error) {
        throw std::runtime_error("无法检查目标文件: " + requested);
    }

    if (operation == "create") {
        if (arguments.contains("old_text")) {
            if (!arguments.at("old_text").is_string()) {
                throw std::invalid_argument("参数 old_text 必须是字符串");
            }
            if (!arguments.at("old_text").get_ref<const std::string&>().empty()) {
                throw std::invalid_argument("create 操作不能提供非空 old_text");
            }
        }
        if (exists) {
            return error_result("create 拒绝覆盖已经存在的路径: " + requested);
        }
        if (!std::filesystem::is_directory(target.parent_path(), error) || error) {
            return error_result("create 的父目录不存在或不是目录: " + requested);
        }

        replace_file_safely(target, new_text, false);
        const auto relative_path = display_path(root_, target);
        change_journal_->record_created(relative_path, new_text);
        return {{"ok", true},
                {"operation", "create"},
                {"path", relative_path},
                {"bytes_before", 0},
                {"bytes_after", new_text.size()}};
    }

    if (operation != "replace") {
        throw std::invalid_argument("operation 只支持 replace 或 create");
    }

    const auto old_text = require_string(arguments, "old_text");
    if (old_text.empty()) {
        throw std::invalid_argument("replace 操作的 old_text 不能为空");
    }
    if (old_text.size() > max_edit_file_bytes) {
        throw std::invalid_argument("old_text 不能超过 256 KiB");
    }
    if (!exists || !std::filesystem::is_regular_file(target, error) || error) {
        return error_result("replace 的目标不存在或不是普通文件: " + requested);
    }

    const auto size = std::filesystem::file_size(target, error);
    if (error) {
        return error_result("无法获取目标文件大小: " + requested);
    }
    if (size > max_edit_file_bytes) {
        return error_result("replace 暂不修改超过 256 KiB 的文件: " + requested);
    }

    std::ifstream input_stream(target, std::ios::binary);
    if (!input_stream) {
        return error_result("无法读取待修改文件: " + requested);
    }
    std::string original((std::istreambuf_iterator<char>(input_stream)),
                         std::istreambuf_iterator<char>());
    if (!input_stream.eof() && input_stream.fail()) {
        return error_result("读取待修改文件失败: " + requested);
    }
    if (contains_nul(original) || !is_valid_utf8(original)) {
        return error_result("apply_patch 拒绝修改二进制或非 UTF-8 文件: " + requested);
    }

    const auto match = original.find(old_text);
    if (match == std::string::npos) {
        return error_result("old_text 在目标文件中不存在；请重新读取文件后再修改");
    }
    if (original.find(old_text, match + 1) != std::string::npos) {
        return error_result("old_text 在目标文件中出现多次；请提供更精确的上下文");
    }

    std::string updated = original;
    updated.replace(match, old_text.size(), new_text);
    if (updated == original) {
        return error_result("修改前后内容相同，没有需要写入的变化");
    }
    if (updated.size() > max_edit_file_bytes) {
        return error_result("修改后的文件不能超过 256 KiB");
    }

    replace_file_safely(target, updated, true);
    const auto relative_path = display_path(root_, target);
    change_journal_->record_modified(relative_path, original, updated);
    return {{"ok", true},
            {"operation", "replace"},
            {"path", relative_path},
            {"bytes_before", original.size()},
            {"bytes_after", updated.size()}};
}

} // namespace aiagent
