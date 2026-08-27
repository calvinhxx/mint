#include "mint/infrastructure/command_runner.hpp"
#include "mint/runtime/task_control.hpp"

#include "command_process.hpp"
#include "command_sandbox.hpp"
#include "diagnostic_log.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mint {
namespace {

std::string require_string(const Json& arguments, std::string_view name) {
    const std::string key(name);
    if (!arguments.contains(key) || !arguments.at(key).is_string()) {
        throw std::invalid_argument("参数 " + key + " 必须是字符串");
    }
    return arguments.at(key).get<std::string>();
}

bool contains_nul(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
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

std::string display_path(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(root);
    return relative.empty() ? "." : relative.generic_string();
}

#if !defined(_WIN32)

struct CommandInvocation {
    std::string program;
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path cwd;
    std::string cwd_label;
    long timeout_seconds = 0;
};

std::vector<std::string> command_arguments(const Json& arguments) {
    std::vector<std::string> result;
    if (!arguments.contains("args")) {
        return result;
    }
    if (!arguments.at("args").is_array()) {
        throw std::invalid_argument("参数 args 必须是字符串数组");
    }
    if (arguments.at("args").size() > runtime_bounds::max_command_arguments) {
        throw std::invalid_argument("命令参数数量超过允许上限");
    }

    std::size_t total_bytes = 0;
    for (const auto& argument : arguments.at("args")) {
        if (!argument.is_string()) {
            throw std::invalid_argument("参数 args 的每一项都必须是字符串");
        }
        auto value = argument.get<std::string>();
        if (contains_nul(value)) {
            throw std::invalid_argument("命令参数不能包含 NUL");
        }
        total_bytes += value.size();
        if (total_bytes > runtime_bounds::max_command_argument_bytes) {
            throw std::invalid_argument("命令参数总长度超过允许上限");
        }
        result.push_back(std::move(value));
    }
    return result;
}

std::filesystem::path command_cwd(const Json& arguments, const std::filesystem::path& root) {
    const auto requested = arguments.contains("cwd") ? require_string(arguments, "cwd") : ".";
    const std::filesystem::path relative(requested.empty() ? "." : requested);
    if (relative.is_absolute()) {
        throw std::invalid_argument("run_command 的 cwd 必须是工作区内的相对路径");
    }

    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(root / relative, error);
    if (error || !is_inside(root, resolved) || !std::filesystem::is_directory(resolved)) {
        throw std::invalid_argument("命令 cwd 不存在、不是目录或超出工作区: " + requested);
    }
    return resolved;
}

long command_timeout(const Json& arguments, long default_timeout, long max_timeout) {
    if (!arguments.contains("timeout_seconds")) {
        return default_timeout;
    }
    if (!arguments.at("timeout_seconds").is_number_integer()) {
        throw std::invalid_argument("timeout_seconds 必须是整数");
    }
    const auto timeout = arguments.at("timeout_seconds").get<long>();
    if (timeout <= 0 || timeout > max_timeout) {
        throw std::invalid_argument("timeout_seconds 必须在 1 到 " + std::to_string(max_timeout) +
                                    " 之间");
    }
    return timeout;
}

CommandInvocation
parse_invocation(const Json& arguments, const std::filesystem::path& root,
                 const std::unordered_map<std::string, std::filesystem::path>& resolved_programs,
                 long default_timeout, long max_timeout) {
    if (!arguments.is_object()) {
        throw std::invalid_argument("run_command 参数必须是 JSON 对象");
    }

    CommandInvocation invocation;
    invocation.program = require_string(arguments, "program");
    const auto executable = resolved_programs.find(invocation.program);
    if (executable == resolved_programs.end()) {
        throw std::invalid_argument("程序未获用户授权: " + invocation.program);
    }
    invocation.executable = executable->second;
    invocation.arguments = command_arguments(arguments);
    invocation.cwd = command_cwd(arguments, root);
    invocation.cwd_label = display_path(root, invocation.cwd);
    invocation.timeout_seconds = command_timeout(arguments, default_timeout, max_timeout);
    return invocation;
}

Json command_result_base(const CommandInvocation& invocation, bool sandboxed,
                         const std::string& sandbox_backend) {
    return {{"program", invocation.program},
            {"cwd", invocation.cwd_label},
            {"sandboxed", sandboxed},
            {"sandbox_backend", sandbox_backend}};
}

enum class UnstartedOutcome { cancelled, task_timed_out, denied };

Json unstarted_result(const CommandInvocation& invocation, UnstartedOutcome outcome, bool sandboxed,
                      const std::string& sandbox_backend) {
    const bool cancelled = outcome == UnstartedOutcome::cancelled;
    const bool task_timed_out = outcome == UnstartedOutcome::task_timed_out;
    const bool denied = outcome == UnstartedOutcome::denied;
    auto result = command_result_base(invocation, sandboxed, sandbox_backend);
    result.update({{"ok", !denied},
                   {"duration_ms", 0},
                   {"status", denied ? "denied" : (cancelled ? "cancelled" : "task_timed_out")},
                   {"exit_code", nullptr},
                   {"signal", nullptr},
                   {"timed_out", task_timed_out},
                   {"task_timed_out", task_timed_out},
                   {"cancelled", cancelled},
                   {"output_truncated", false},
                   {"output", ""}});
    if (denied) {
        result["error"] = "命令被用户逐次审批拒绝";
    }
    return result;
}

std::optional<Json> stopped_result(const CommandInvocation& invocation,
                                   const std::shared_ptr<TaskControl>& task_control, bool sandboxed,
                                   const std::string& sandbox_backend) {
    const auto reason = task_control == nullptr ? std::string{} : task_control->stop_reason();
    if (reason.empty()) {
        return std::nullopt;
    }
    const auto outcome =
        reason == "cancelled" ? UnstartedOutcome::cancelled : UnstartedOutcome::task_timed_out;
    return unstarted_result(invocation, outcome, sandboxed, sandbox_backend);
}

Json denied_result(const CommandInvocation& invocation, bool sandboxed,
                   const std::string& sandbox_backend) {
    return unstarted_result(invocation, UnstartedOutcome::denied, sandboxed, sandbox_backend);
}

Json process_result_json(const CommandInvocation& invocation, command_detail::ProcessResult process,
                         bool sandboxed, const std::string& sandbox_backend) {
    auto result = command_result_base(invocation, sandboxed, sandbox_backend);
    result.update(
        {{"ok", true},
         {"duration_ms", process.duration_ms},
         {"status", std::move(process.status)},
         {"exit_code", process.exit_code.has_value() ? Json(*process.exit_code) : Json(nullptr)},
         {"signal", process.signal.has_value() ? Json(*process.signal) : Json(nullptr)},
         {"timed_out", process.timed_out || process.task_timed_out},
         {"task_timed_out", process.task_timed_out},
         {"cancelled", process.cancelled},
         {"output_truncated", process.output_truncated},
         {"output", std::move(process.output)}});
    return result;
}

command_detail::ProcessRequest process_request(const CommandInvocation& invocation, bool sandboxed,
                                               const std::filesystem::path& sandbox_executable,
                                               const std::string& sandbox_profile,
                                               std::size_t max_output_bytes,
                                               const std::shared_ptr<TaskControl>& task_control) {
    command_detail::ProcessRequest request;
    request.executable = sandboxed ? sandbox_executable : invocation.executable;
    request.argv.reserve(invocation.arguments.size() + (sandboxed ? 4 : 1));
    if (sandboxed) {
        request.argv = {"sandbox-exec", "-p", sandbox_profile,
                        invocation.executable.generic_string()};
    } else {
        request.argv.push_back(invocation.program);
    }
    request.argv.insert(request.argv.end(), invocation.arguments.begin(),
                        invocation.arguments.end());
    request.cwd = invocation.cwd;
    request.timeout_seconds = invocation.timeout_seconds;
    request.max_output_bytes = max_output_bytes;
    request.task_control = task_control;
    return request;
}

struct CommandCatalog {
    std::vector<std::string> allowed_programs;
    std::unordered_map<std::string, std::filesystem::path> resolved_programs;
    std::vector<CommandRecipe> recipes;
    std::unordered_map<std::string, std::size_t> recipe_indices;
    std::vector<std::string> recipe_names;
};

CommandCatalog build_command_catalog(CommandRunnerOptions& options, long max_timeout) {
    CommandCatalog catalog;
    std::vector<std::string> requested_programs = options.allowed_programs;
    for (auto& recipe : options.recipes) {
        if (recipe.name.empty() || recipe.program.empty() || recipe.timeout_seconds <= 0 ||
            recipe.timeout_seconds > max_timeout || recipe.cwd.is_absolute()) {
            throw std::invalid_argument("固定命令 recipe 配置无效: " + recipe.name);
        }
        if (catalog.recipe_indices.contains(recipe.name)) {
            throw std::invalid_argument("固定命令 recipe 名称重复: " + recipe.name);
        }
        catalog.recipe_indices.emplace(recipe.name, catalog.recipes.size());
        catalog.recipe_names.push_back(recipe.name);
        if (std::find(requested_programs.begin(), requested_programs.end(), recipe.program) ==
            requested_programs.end()) {
            requested_programs.push_back(recipe.program);
        }
        catalog.recipes.push_back(std::move(recipe));
    }

    for (const auto& requested : requested_programs) {
        if (catalog.resolved_programs.contains(requested)) {
            continue;
        }
        catalog.resolved_programs.emplace(requested, command_detail::resolve_program(requested));
        catalog.allowed_programs.push_back(requested);
    }
    return catalog;
}

#endif

} // namespace

CommandRunner::CommandRunner(CommandRunnerOptions options)
    : default_timeout_seconds_(options.default_timeout_seconds),
      max_timeout_seconds_(options.max_timeout_seconds),
      max_output_bytes_(options.max_output_bytes), task_control_(std::move(options.task_control)),
      approval_(std::move(options.approval)) {
    std::error_code error;
    root_ = std::filesystem::weakly_canonical(std::move(options.root), error);
    if (error || root_.empty() || !std::filesystem::is_directory(root_)) {
        throw std::invalid_argument("命令工作目录不存在或不是目录");
    }
    if (default_timeout_seconds_ <= 0 || max_timeout_seconds_ <= 0 ||
        default_timeout_seconds_ > max_timeout_seconds_) {
        throw std::invalid_argument("命令超时配置必须为正数，且默认值不能超过最大值");
    }
    if (max_output_bytes_ == 0 || max_output_bytes_ > runtime_bounds::max_command_output_bytes) {
        throw std::invalid_argument("命令输出上限必须在 1 字节到 1 MiB 之间");
    }
    if (!options.allowed_programs.empty() && !options.recipes.empty()) {
        throw std::invalid_argument("原始程序授权与固定命令 recipe 不能同时启用");
    }
    if (options.allowed_programs.empty() && options.recipes.empty()) {
        throw std::invalid_argument("至少需要显式授权一个命令程序");
    }

#if defined(_WIN32)
    throw std::invalid_argument("当前受控命令执行暂未支持 Windows");
#else
    auto catalog = build_command_catalog(options, max_timeout_seconds_);
    allowed_programs_ = std::move(catalog.allowed_programs);
    resolved_programs_ = std::move(catalog.resolved_programs);
    recipes_ = std::move(catalog.recipes);
    recipe_indices_ = std::move(catalog.recipe_indices);
    recipe_names_ = std::move(catalog.recipe_names);

    auto sandbox =
        command_detail::build_sandbox_config(options.require_os_sandbox, root_, resolved_programs_,
                                             std::move(options.denied_read_paths));
    sandbox_executable_ = std::move(sandbox.executable);
    sandbox_profile_ = std::move(sandbox.profile);
    sandbox_backend_ = std::move(sandbox.backend);
#endif
}

const std::vector<std::string>& CommandRunner::allowed_programs() const noexcept {
    return allowed_programs_;
}

const std::vector<std::string>& CommandRunner::recipe_names() const noexcept {
    return recipe_names_;
}

bool CommandRunner::uses_recipes() const noexcept {
    return !recipes_.empty();
}

bool CommandRunner::requires_approval() const noexcept {
    return static_cast<bool>(approval_);
}

bool CommandRunner::is_os_sandboxed() const noexcept {
    return sandbox_backend_ != "none";
}

const std::string& CommandRunner::sandbox_backend() const noexcept {
    return sandbox_backend_;
}

Json CommandRunner::definition() const {
    if (uses_recipes()) {
        std::string descriptions;
        for (const auto& recipe : recipes_) {
            if (!descriptions.empty()) {
                descriptions += "; ";
            }
            descriptions += recipe.name;
            if (!recipe.description.empty()) {
                descriptions += "=" + recipe.description;
            }
            if (recipe.verification) {
                descriptions += " [verification]";
            }
        }
        return {{"type", "function"},
                {"function",
                 {{"name", "run_recipe"},
                  {"description",
                   "Run one fixed command recipe authorized by the user policy. Arguments, cwd and "
                   "timeout are immutable. Available recipes: " +
                       descriptions},
                  {"parameters",
                   {{"type", "object"},
                    {"properties",
                     {{"recipe",
                       {{"type", "string"},
                        {"enum", recipe_names_},
                        {"description", "Exact recipe name from the user policy."}}}}},
                    {"required", Json::array({"recipe"})},
                    {"additionalProperties", false}}}}}};
    }
    return {{"type", "function"},
            {"function",
             {{"name", "run_command"},
              {"description",
               "Run one user-approved executable without a shell. Use it for focused build or "
               "test commands. The working directory must remain inside the workspace."},
              {"parameters",
               {{"type", "object"},
                {"properties",
                 {{"program",
                   {{"type", "string"},
                    {"enum", allowed_programs_},
                    {"description", "Exact executable label approved by the user."}}},
                  {"args",
                   {{"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"maxItems", runtime_bounds::max_command_arguments},
                    {"description",
                     "Argument vector. Shell syntax and string interpolation are not used."}}},
                  {"cwd",
                   {{"type", "string"},
                    {"description",
                     "Relative working directory inside the workspace. Defaults to '.'."}}},
                  {"timeout_seconds",
                   {{"type", "integer"},
                    {"minimum", 1},
                    {"maximum", max_timeout_seconds_},
                    {"description", "Wall-clock timeout. Defaults to the runner policy."}}}}},
                {"required", Json::array({"program"})},
                {"additionalProperties", false}}}}}};
}

Json CommandRunner::run(const Json& arguments) const {
    if (!uses_recipes()) {
        auto result = run_command(arguments);
        result["verification_eligible"] = true;
        return result;
    }
    if (!arguments.is_object() || arguments.size() != 1) {
        throw std::invalid_argument("run_recipe 只接受 recipe 字段");
    }
    const auto recipe_name = require_string(arguments, "recipe");
    const auto found = recipe_indices_.find(recipe_name);
    if (found == recipe_indices_.end()) {
        throw std::invalid_argument("recipe 未获用户 policy 授权: " + recipe_name);
    }
    const auto& recipe = recipes_.at(found->second);
    Json expanded = {{"program", recipe.program},
                     {"args", recipe.args},
                     {"cwd", recipe.cwd.generic_string()},
                     {"timeout_seconds", recipe.timeout_seconds}};
    auto result = run_command(expanded);
    result["recipe"] = recipe.name;
    result["verification_eligible"] = recipe.verification;
    return result;
}

Json CommandRunner::run_command(const Json& arguments) const {
#if defined(_WIN32)
    (void)arguments;
    throw std::runtime_error("当前受控命令执行暂未支持 Windows");
#else
    const auto invocation = parse_invocation(arguments, root_, resolved_programs_,
                                             default_timeout_seconds_, max_timeout_seconds_);
    diagnostics::emit(diagnostics::Level::debug, "command.prepared",
                      {{"program", invocation.program},
                       {"cwd", invocation.cwd_label},
                       {"argument_count", invocation.arguments.size()},
                       {"timeout_seconds", invocation.timeout_seconds},
                       {"sandbox", sandbox_backend_}});
    if (const auto stopped =
            stopped_result(invocation, task_control_, is_os_sandboxed(), sandbox_backend_)) {
        return *stopped;
    }

    const CommandApprovalRequest approval_request{.program = invocation.program,
                                                  .args = invocation.arguments,
                                                  .cwd = invocation.cwd_label,
                                                  .timeout_seconds = invocation.timeout_seconds};
    if (approval_ && !approval_(approval_request)) {
        return denied_result(invocation, is_os_sandboxed(), sandbox_backend_);
    }
    if (const auto stopped =
            stopped_result(invocation, task_control_, is_os_sandboxed(), sandbox_backend_)) {
        return *stopped;
    }

    auto request = process_request(invocation, is_os_sandboxed(), sandbox_executable_,
                                   sandbox_profile_, max_output_bytes_, task_control_);
    auto process = command_detail::execute_process(std::move(request));
    diagnostics::emit(diagnostics::Level::debug, "command.completed",
                      {{"program", invocation.program},
                       {"status", process.status},
                       {"exit_code", process.exit_code.value_or(-1)},
                       {"duration_ms", process.duration_ms},
                       {"output_truncated", process.output_truncated}});
    return process_result_json(invocation, std::move(process), is_os_sandboxed(), sandbox_backend_);
#endif
}

} // namespace mint
