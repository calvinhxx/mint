#include "aiagent/infrastructure/command_runner.hpp"
#include "aiagent/runtime/task_control.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace aiagent {
namespace {

constexpr std::size_t max_argument_count = 64;
constexpr std::size_t max_argument_bytes = 32 * 1024;

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

bool is_blocked_launcher(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    static const std::unordered_set<std::string> blocked = {
        "sh",         "bash",           "zsh",     "fish",     "dash", "cmd",   "cmd.exe",
        "powershell", "powershell.exe", "pwsh",    "pwsh.exe", "env",  "xargs", "find",
        "git",        "sudo",           "doas",    "ssh",      "scp",  "curl",  "wget",
        "osascript",  "open",           "busybox", "deno",     "bun"};
    return blocked.contains(name) || name.starts_with("python") || name.starts_with("pypy") ||
           name.starts_with("node") || name.starts_with("perl") || name.starts_with("ruby") ||
           name.starts_with("lua") || name.starts_with("php");
}

#if !defined(_WIN32)

bool is_executable_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
           ::access(path.c_str(), X_OK) == 0;
}

std::filesystem::path resolve_program(const std::string& requested) {
    if (requested.empty() || contains_nul(requested)) {
        throw std::invalid_argument("授权程序名称不能为空或包含 NUL");
    }

    const std::filesystem::path input(requested);
    if (is_blocked_launcher(input.filename().string())) {
        throw std::invalid_argument("当前版本不允许授权 shell、解释器、git 或通用命令启动器: " +
                                    requested);
    }

    if (input.has_parent_path()) {
        if (!input.is_absolute()) {
            throw std::invalid_argument("带路径的授权程序必须使用绝对路径: " + requested);
        }
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(input, error);
        if (error || !is_executable_file(resolved)) {
            throw std::invalid_argument("授权程序不存在或不可执行: " + requested);
        }
        return resolved;
    }

    for (const auto character : requested) {
        const auto value = static_cast<unsigned char>(character);
        if (!std::isalnum(value) && character != '_' && character != '-' && character != '+' &&
            character != '.') {
            throw std::invalid_argument("授权程序名称包含不支持的字符: " + requested);
        }
    }

    const char* path_value = std::getenv("PATH");
    const std::string search_path =
        path_value == nullptr ? "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin" : path_value;

    std::size_t begin = 0;
    while (begin <= search_path.size()) {
        const auto end = search_path.find(':', begin);
        const auto length = end == std::string::npos ? search_path.size() - begin : end - begin;
        auto directory = search_path.substr(begin, length);
        if (directory.empty()) {
            directory = ".";
        }
        const auto candidate = std::filesystem::path(directory) / requested;
        if (is_executable_file(candidate)) {
            std::error_code error;
            const auto resolved = std::filesystem::weakly_canonical(candidate, error);
            if (!error) {
                return resolved;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    throw std::invalid_argument("在 PATH 中找不到授权程序: " + requested);
}

std::vector<std::string> filtered_environment() {
    static const std::unordered_set<std::string> allowed_names = {"PATH",
                                                                  "HOME",
                                                                  "TMPDIR",
                                                                  "TMP",
                                                                  "TEMP",
                                                                  "LANG",
                                                                  "LC_ALL",
                                                                  "LC_CTYPE",
                                                                  "TERM",
                                                                  "USER",
                                                                  "LOGNAME",
                                                                  "SHELL",
                                                                  "SDKROOT",
                                                                  "DEVELOPER_DIR",
                                                                  "MACOSX_DEPLOYMENT_TARGET",
                                                                  "CC",
                                                                  "CXX",
                                                                  "CFLAGS",
                                                                  "CXXFLAGS",
                                                                  "CPPFLAGS",
                                                                  "LDFLAGS",
                                                                  "CMAKE_PREFIX_PATH",
                                                                  "CMAKE_TOOLCHAIN_FILE",
                                                                  "VCPKG_ROOT",
                                                                  "PKG_CONFIG_PATH",
                                                                  "NINJA_STATUS",
                                                                  "MAKEFLAGS"};

    std::vector<std::string> result;
    bool has_path = false;
    for (char** current = environ; current != nullptr && *current != nullptr; ++current) {
        const std::string entry(*current);
        const auto separator = entry.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const auto name = entry.substr(0, separator);
        if (!allowed_names.contains(name)) {
            continue;
        }
        has_path = has_path || name == "PATH";
        result.push_back(entry);
    }
    if (!has_path) {
        result.emplace_back("PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin");
    }
    return result;
}

std::vector<char*> mutable_pointers(std::vector<std::string>& values) {
    std::vector<char*> result;
    result.reserve(values.size() + 1);
    for (auto& value : values) {
        result.push_back(value.data());
    }
    result.push_back(nullptr);
    return result;
}

std::string sandbox_string(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result += character;
            break;
        }
    }
    return result;
}

void append_output(std::string& output, const char* data, std::size_t size, std::size_t limit,
                   bool& truncated) {
    const auto remaining = output.size() < limit ? limit - output.size() : 0;
    const auto accepted = std::min(size, remaining);
    output.append(data, accepted);
    truncated = truncated || accepted < size;
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
    if (max_output_bytes_ == 0 || max_output_bytes_ > 1024 * 1024) {
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
    std::vector<std::string> requested_programs = options.allowed_programs;
    for (auto& recipe : options.recipes) {
        if (recipe.name.empty() || recipe.program.empty() || recipe.timeout_seconds <= 0 ||
            recipe.timeout_seconds > max_timeout_seconds_ || recipe.cwd.is_absolute()) {
            throw std::invalid_argument("固定命令 recipe 配置无效: " + recipe.name);
        }
        if (recipe_indices_.contains(recipe.name)) {
            throw std::invalid_argument("固定命令 recipe 名称重复: " + recipe.name);
        }
        recipe_indices_.emplace(recipe.name, recipes_.size());
        recipe_names_.push_back(recipe.name);
        if (std::find(requested_programs.begin(), requested_programs.end(), recipe.program) ==
            requested_programs.end()) {
            requested_programs.push_back(recipe.program);
        }
        recipes_.push_back(std::move(recipe));
    }

    for (const auto& requested : requested_programs) {
        if (resolved_programs_.contains(requested)) {
            continue;
        }
        resolved_programs_.emplace(requested, resolve_program(requested));
        allowed_programs_.push_back(requested);
    }

    if (options.require_os_sandbox) {
#if defined(__APPLE__)
        sandbox_executable_ = "/usr/bin/sandbox-exec";
        if (!is_executable_file(sandbox_executable_)) {
            throw std::invalid_argument(
                "当前主机缺少 /usr/bin/sandbox-exec，拒绝无 OS 沙箱执行命令");
        }
        sandbox_backend_ = "macos-seatbelt";
        const auto escaped_root = sandbox_string(root_.generic_string());
        sandbox_profile_ = "(version 1) "
                           "(allow default) "
                           "(deny network*) "
                           "(deny file-write* (require-not (subpath \"" +
                           escaped_root + "\")))";

        if (const char* home_value = std::getenv("HOME"); home_value != nullptr) {
            std::error_code home_error;
            const auto home = std::filesystem::weakly_canonical(home_value, home_error);
            if (!home_error && home.is_absolute() && home != root_) {
                sandbox_profile_ += " (deny file-read* (require-all (subpath \"" +
                                    sandbox_string(home.generic_string()) +
                                    "\") (require-not (subpath \"" + escaped_root + "\"))";
                for (const auto& [label, executable] : resolved_programs_) {
                    (void)label;
                    if (is_inside(home, executable) && !is_inside(root_, executable)) {
                        sandbox_profile_ += " (require-not (literal \"" +
                                            sandbox_string(executable.generic_string()) + "\"))";
                    }
                }
                sandbox_profile_ += "))";
            }
        }

        for (auto& denied : options.denied_read_paths) {
            std::error_code denied_error;
            denied = std::filesystem::weakly_canonical(std::move(denied), denied_error);
            if (denied_error || denied.empty()) {
                continue;
            }
            const bool directory =
                std::filesystem::is_directory(denied, denied_error) && !denied_error;
            sandbox_profile_ += " (deny file-read* file-write* (" +
                                std::string(directory ? "subpath" : "literal") + " \"" +
                                sandbox_string(denied.generic_string()) + "\"))";
        }
#else
        throw std::invalid_argument(
            "当前主机没有已实现的命令 OS 沙箱后端；若确实接受风险，显式关闭该策略");
#endif
    }
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
                    {"maxItems", max_argument_count},
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
    if (!arguments.is_object()) {
        throw std::invalid_argument("run_command 参数必须是 JSON 对象");
    }

    const auto program = require_string(arguments, "program");
    const auto executable = resolved_programs_.find(program);
    if (executable == resolved_programs_.end()) {
        throw std::invalid_argument("程序未获用户授权: " + program);
    }

    std::vector<std::string> command_arguments;
    std::size_t argument_bytes = 0;
    if (arguments.contains("args")) {
        if (!arguments.at("args").is_array()) {
            throw std::invalid_argument("参数 args 必须是字符串数组");
        }
        if (arguments.at("args").size() > max_argument_count) {
            throw std::invalid_argument("命令参数不能超过 64 个");
        }
        for (const auto& argument : arguments.at("args")) {
            if (!argument.is_string()) {
                throw std::invalid_argument("参数 args 的每一项都必须是字符串");
            }
            auto value = argument.get<std::string>();
            if (contains_nul(value)) {
                throw std::invalid_argument("命令参数不能包含 NUL");
            }
            argument_bytes += value.size();
            if (argument_bytes > max_argument_bytes) {
                throw std::invalid_argument("命令参数总长度不能超过 32 KiB");
            }
            command_arguments.push_back(std::move(value));
        }
    }

    std::string requested_cwd = ".";
    if (arguments.contains("cwd")) {
        requested_cwd = require_string(arguments, "cwd");
    }
    const std::filesystem::path cwd_input(requested_cwd.empty() ? "." : requested_cwd);
    if (cwd_input.is_absolute()) {
        throw std::invalid_argument("run_command 的 cwd 必须是工作区内的相对路径");
    }
    std::error_code path_error;
    const auto cwd = std::filesystem::weakly_canonical(root_ / cwd_input, path_error);
    if (path_error || !is_inside(root_, cwd) || !std::filesystem::is_directory(cwd)) {
        throw std::invalid_argument("命令 cwd 不存在、不是目录或超出工作区: " + requested_cwd);
    }

    long timeout_seconds = default_timeout_seconds_;
    if (arguments.contains("timeout_seconds")) {
        if (!arguments.at("timeout_seconds").is_number_integer()) {
            throw std::invalid_argument("timeout_seconds 必须是整数");
        }
        timeout_seconds = arguments.at("timeout_seconds").get<long>();
        if (timeout_seconds <= 0 || timeout_seconds > max_timeout_seconds_) {
            throw std::invalid_argument("timeout_seconds 必须在 1 到 " +
                                        std::to_string(max_timeout_seconds_) + " 之间");
        }
    }

    const auto stopped_result = [&]() -> Json {
        const auto reason = task_control_ == nullptr ? std::string{} : task_control_->stop_reason();
        if (reason.empty()) {
            return Json();
        }
        const bool cancelled = reason == "cancelled";
        return {{"ok", true},
                {"program", program},
                {"cwd", display_path(root_, cwd)},
                {"duration_ms", 0},
                {"status", cancelled ? "cancelled" : "task_timed_out"},
                {"exit_code", nullptr},
                {"signal", nullptr},
                {"timed_out", !cancelled},
                {"task_timed_out", !cancelled},
                {"cancelled", cancelled},
                {"output_truncated", false},
                {"sandboxed", is_os_sandboxed()},
                {"sandbox_backend", sandbox_backend_},
                {"output", ""}};
    };
    if (const auto stopped = stopped_result(); !stopped.is_null()) {
        return stopped;
    }

    if (approval_ && !approval_(CommandApprovalRequest{.program = program,
                                                       .args = command_arguments,
                                                       .cwd = display_path(root_, cwd),
                                                       .timeout_seconds = timeout_seconds})) {
        return {{"ok", false},
                {"error", "命令被用户逐次审批拒绝"},
                {"program", program},
                {"cwd", display_path(root_, cwd)},
                {"duration_ms", 0},
                {"status", "denied"},
                {"exit_code", nullptr},
                {"signal", nullptr},
                {"timed_out", false},
                {"task_timed_out", false},
                {"cancelled", false},
                {"output_truncated", false},
                {"sandboxed", is_os_sandboxed()},
                {"sandbox_backend", sandbox_backend_},
                {"output", ""}};
    }
    if (const auto stopped = stopped_result(); !stopped.is_null()) {
        return stopped;
    }

    std::vector<std::string> argv_storage;
    argv_storage.reserve(command_arguments.size() + (is_os_sandboxed() ? 4 : 1));
    if (is_os_sandboxed()) {
        argv_storage.emplace_back("sandbox-exec");
        argv_storage.emplace_back("-p");
        argv_storage.push_back(sandbox_profile_);
        argv_storage.push_back(executable->second.generic_string());
    } else {
        argv_storage.push_back(program);
    }
    for (auto& argument : command_arguments) {
        argv_storage.push_back(std::move(argument));
    }
    auto argv = mutable_pointers(argv_storage);
    auto environment_storage = filtered_environment();
    auto environment = mutable_pointers(environment_storage);

    std::array<int, 2> output_pipe{};
    if (::pipe(output_pipe.data()) != 0) {
        throw std::runtime_error("无法创建命令输出管道: " + std::string(std::strerror(errno)));
    }

    const auto read_flags = ::fcntl(output_pipe[0], F_GETFL, 0);
    if (read_flags < 0 || ::fcntl(output_pipe[0], F_SETFL, read_flags | O_NONBLOCK) != 0) {
        const auto message = std::string(std::strerror(errno));
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        throw std::runtime_error("无法配置命令输出管道: " + message);
    }

    const auto started_at = std::chrono::steady_clock::now();
    const auto process = ::fork();
    if (process < 0) {
        const auto message = std::string(std::strerror(errno));
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        throw std::runtime_error("无法启动命令进程: " + message);
    }

    if (process == 0) {
        (void)::setpgid(0, 0);
        if (::chdir(cwd.c_str()) != 0 || ::dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
            ::dup2(output_pipe[1], STDERR_FILENO) < 0) {
            constexpr char message[] = "aiagent: failed to prepare approved command\n";
            (void)::write(output_pipe[1], message, sizeof(message) - 1);
            ::_exit(126);
        }
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        const auto& process_executable =
            is_os_sandboxed() ? sandbox_executable_ : executable->second;
        ::execve(process_executable.c_str(), argv.data(), environment.data());
        constexpr char message[] = "aiagent: failed to execute approved command\n";
        (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
        ::_exit(127);
    }

    ::close(output_pipe[1]);
    (void)::setpgid(process, process);

    std::string output;
    output.reserve(std::min<std::size_t>(max_output_bytes_, 16 * 1024));
    bool output_truncated = false;
    bool timed_out = false;
    bool task_timed_out = false;
    bool cancelled = false;
    bool exited = false;
    int wait_status = 0;

    const auto drain_output = [&]() {
        std::array<char, 4096> buffer{};
        while (true) {
            const auto bytes = ::read(output_pipe[0], buffer.data(), buffer.size());
            if (bytes > 0) {
                append_output(output, buffer.data(), static_cast<std::size_t>(bytes),
                              max_output_bytes_, output_truncated);
                continue;
            }
            if (bytes < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    };

    const auto deadline = started_at + std::chrono::seconds(timeout_seconds);
    const auto terminate_process = [&]() {
        (void)::kill(-process, SIGTERM);
        const auto grace_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < grace_deadline) {
            drain_output();
            const auto grace_wait = ::waitpid(process, &wait_status, WNOHANG);
            if (grace_wait == process) {
                exited = true;
                break;
            }
            pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
            (void)::poll(&descriptor, 1, 10);
        }
        if (!exited) {
            (void)::kill(-process, SIGKILL);
            while (::waitpid(process, &wait_status, 0) < 0 && errno == EINTR) {
            }
            exited = true;
        }
    };

    while (!exited) {
        drain_output();
        const auto waited = ::waitpid(process, &wait_status, WNOHANG);
        if (waited == process) {
            exited = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            const auto message = std::string(std::strerror(errno));
            (void)::kill(-process, SIGKILL);
            (void)::waitpid(process, &wait_status, 0);
            ::close(output_pipe[0]);
            throw std::runtime_error("等待命令进程失败: " + message);
        }

        if (task_control_ != nullptr && task_control_->cancellation_requested()) {
            cancelled = true;
            terminate_process();
            break;
        }
        if (task_control_ != nullptr && task_control_->budget_exhausted()) {
            task_timed_out = true;
            terminate_process();
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            terminate_process();
            break;
        }

        pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
        (void)::poll(&descriptor, 1, 20);
    }

    drain_output();
    ::close(output_pipe[0]);
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started_at)
                              .count();

    Json result = {{"ok", true},
                   {"program", program},
                   {"cwd", display_path(root_, cwd)},
                   {"duration_ms", duration},
                   {"timed_out", timed_out || task_timed_out},
                   {"task_timed_out", task_timed_out},
                   {"cancelled", cancelled},
                   {"output_truncated", output_truncated},
                   {"sandboxed", is_os_sandboxed()},
                   {"sandbox_backend", sandbox_backend_},
                   {"output", std::move(output)}};

    if (cancelled) {
        result["status"] = "cancelled";
        result["exit_code"] = nullptr;
        result["signal"] = WIFSIGNALED(wait_status) ? Json(WTERMSIG(wait_status)) : Json(nullptr);
    } else if (task_timed_out) {
        result["status"] = "task_timed_out";
        result["exit_code"] = nullptr;
        result["signal"] = WIFSIGNALED(wait_status) ? Json(WTERMSIG(wait_status)) : Json(nullptr);
    } else if (timed_out) {
        result["status"] = "timed_out";
        result["exit_code"] = nullptr;
        result["signal"] = WIFSIGNALED(wait_status) ? Json(WTERMSIG(wait_status)) : Json(nullptr);
    } else if (WIFEXITED(wait_status)) {
        result["status"] = "exited";
        result["exit_code"] = WEXITSTATUS(wait_status);
        result["signal"] = nullptr;
    } else if (WIFSIGNALED(wait_status)) {
        result["status"] = "signaled";
        result["exit_code"] = nullptr;
        result["signal"] = WTERMSIG(wait_status);
    } else {
        result["status"] = "unknown";
        result["exit_code"] = nullptr;
        result["signal"] = nullptr;
    }
    return result;
#endif
}

} // namespace aiagent
