#include "command_sandbox.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#if !defined(_WIN32)
#include <unistd.h>
#if defined(__APPLE__)
#include <strings.h>
#endif
#endif

namespace mint::command_detail {
namespace {

bool contains_nul(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

bool is_blocked_launcher(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    static const std::unordered_set<std::string> blocked = {
        "sh",           "bash",        "zsh",
        "fish",         "dash",        "cmd",
        "cmd.exe",      "powershell",  "powershell.exe",
        "pwsh",         "pwsh.exe",    "env",
        "xargs",        "find",        "git",
        "sudo",         "doas",        "ssh",
        "scp",          "curl",        "wget",
        "osascript",    "open",        "busybox",
        "deno",         "bun",         "bwrap",
        "sandbox-exec", "unshare",     "nsenter",
        "chroot",       "systemd-run", "docker",
        "podman",       "wscript",     "wscript.exe",
        "cscript",      "cscript.exe", "mshta",
        "mshta.exe",    "rundll32",    "rundll32.exe",
        "regsvr32",     "regsvr32.exe"};
    return blocked.contains(name) || name.starts_with("python") || name.starts_with("pypy") ||
           name.starts_with("node") || name.starts_with("perl") || name.starts_with("ruby") ||
           name.starts_with("lua") || name.starts_with("php");
}

#if !defined(_WIN32)

bool is_inside(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end()) {
            return false;
        }
#if defined(__APPLE__)
        if (::strcasecmp((*root_part).c_str(), (*candidate_part).c_str()) != 0) {
            return false;
        }
#else
        if (*root_part != *candidate_part) {
            return false;
        }
#endif
    }
    return true;
}

std::vector<std::filesystem::path>
normalize_read_only_paths(const std::filesystem::path& workspace,
                          std::vector<std::filesystem::path> paths,
                          const std::vector<std::filesystem::path>& denied_paths) {
    std::vector<std::filesystem::path> normalized;
    normalized.reserve(paths.size());
    for (auto& path : paths) {
        std::error_code error;
        path = std::filesystem::weakly_canonical(std::move(path), error);
        if (error || path.empty() || !path.is_absolute() || !std::filesystem::exists(path, error) ||
            error) {
            throw std::invalid_argument("命令外部只读路径不存在或不是绝对路径");
        }
        if (is_inside(workspace, path) || is_inside(path, workspace)) {
            throw std::invalid_argument("命令外部只读路径不能位于工作区内或包含工作区: " +
                                        path.generic_string());
        }
        for (const auto& denied_input : denied_paths) {
            error.clear();
            const auto denied = std::filesystem::weakly_canonical(denied_input, error);
            if (!error && !denied.empty() && (is_inside(path, denied) || is_inside(denied, path))) {
                throw std::invalid_argument("命令外部只读路径不能与保护路径重叠: " +
                                            path.generic_string());
            }
        }
        normalized.push_back(std::move(path));
    }
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    return normalized;
}

bool is_executable_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
           ::access(path.c_str(), X_OK) == 0;
}

std::filesystem::path find_executable(const std::string& requested) {
    if (requested.empty() || contains_nul(requested)) {
        throw std::invalid_argument("程序名称不能为空或包含 NUL");
    }

    const std::filesystem::path input(requested);
    if (input.has_parent_path()) {
        if (!input.is_absolute()) {
            throw std::invalid_argument("带路径的程序必须使用绝对路径: " + requested);
        }
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(input, error);
        if (error || !is_executable_file(resolved)) {
            throw std::invalid_argument("程序不存在或不可执行: " + requested);
        }
        return resolved;
    }

    for (const auto character : requested) {
        const auto value = static_cast<unsigned char>(character);
        if (!std::isalnum(value) && character != '_' && character != '-' && character != '+' &&
            character != '.') {
            throw std::invalid_argument("程序名称包含不支持的字符: " + requested);
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
    throw std::invalid_argument("在 PATH 中找不到程序: " + requested);
}

#if defined(__APPLE__)
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
#endif

#endif

#if defined(_WIN32)

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool is_windows_executable_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::filesystem::path canonical_executable(const std::filesystem::path& candidate,
                                           const std::string& requested) {
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(candidate, error);
    if (error || !is_windows_executable_file(resolved)) {
        throw std::invalid_argument("程序不存在或不可执行: " + requested);
    }
    return resolved;
}

std::filesystem::path find_executable(const std::string& requested) {
    if (requested.empty() || contains_nul(requested)) {
        throw std::invalid_argument("程序名称不能为空或包含 NUL");
    }

    const std::filesystem::path input(requested);
    if (input.has_parent_path()) {
        if (!input.is_absolute()) {
            throw std::invalid_argument("带路径的程序必须使用绝对路径: " + requested);
        }
        return canonical_executable(input, requested);
    }

    for (const auto character : requested) {
        const auto value = static_cast<unsigned char>(character);
        if (!std::isalnum(value) && character != '_' && character != '-' && character != '+' &&
            character != '.') {
            throw std::invalid_argument("程序名称包含不支持的字符: " + requested);
        }
    }

    const auto extension = lowercase_ascii(input.extension().string());
    const std::vector<std::string> suffixes = extension.empty()
                                                  ? std::vector<std::string>{"", ".exe", ".com"}
                                                  : std::vector<std::string>{""};
    const char* path_value = std::getenv("PATH");
    const std::string search_path = path_value == nullptr ? std::string{} : path_value;
    std::size_t begin = 0;
    while (begin <= search_path.size()) {
        const auto end = search_path.find(';', begin);
        const auto length = end == std::string::npos ? search_path.size() - begin : end - begin;
        auto directory = search_path.substr(begin, length);
        if (directory.size() >= 2 && directory.front() == '"' && directory.back() == '"') {
            directory = directory.substr(1, directory.size() - 2);
        }
        if (!directory.empty()) {
            for (const auto& suffix : suffixes) {
                const auto candidate = std::filesystem::path(directory) / (requested + suffix);
                if (is_windows_executable_file(candidate)) {
                    return canonical_executable(candidate, requested);
                }
            }
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    throw std::invalid_argument("在 PATH 中找不到程序: " + requested);
}

#endif

#if defined(__linux__)

void append_option(std::vector<std::string>& arguments, std::string option,
                   const std::filesystem::path& value) {
    arguments.push_back(std::move(option));
    arguments.push_back(value.generic_string());
}

void append_bind(std::vector<std::string>& arguments, std::string option,
                 const std::filesystem::path& source, const std::filesystem::path& destination) {
    arguments.push_back(std::move(option));
    arguments.push_back(source.generic_string());
    arguments.push_back(destination.generic_string());
}

std::optional<std::filesystem::path> normalized_existing_path(std::string_view value) {
    if (value.empty() || contains_nul(value)) {
        return std::nullopt;
    }
    std::filesystem::path path(value);
    if (!path.is_absolute()) {
        return std::nullopt;
    }
    std::error_code error;
    path = std::filesystem::weakly_canonical(std::move(path), error);
    if (error || path.empty() || !std::filesystem::exists(path, error) || error) {
        return std::nullopt;
    }
    return path;
}

void append_path_list(std::vector<std::filesystem::path>& paths, const char* name) {
    const char* raw_value = std::getenv(name);
    if (raw_value == nullptr) {
        return;
    }
    const std::string value(raw_value);
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find_first_of(":;", begin);
        const auto length = end == std::string::npos ? value.size() - begin : end - begin;
        if (const auto path =
                normalized_existing_path(std::string_view(value).substr(begin, length))) {
            paths.push_back(*path);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
}

void append_single_path(std::vector<std::filesystem::path>& paths, const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return;
    }
    if (const auto path = normalized_existing_path(value)) {
        paths.push_back(*path);
    }
}

bool is_covered_by(const std::vector<std::filesystem::path>& roots,
                   const std::filesystem::path& candidate) {
    return std::any_of(roots.begin(), roots.end(),
                       [&](const auto& root) { return is_inside(root, candidate); });
}

std::filesystem::path bubblewrap_executable() {
    if (const char* override_path = std::getenv("MINT_BWRAP_PATH");
        override_path != nullptr && *override_path != '\0') {
        return find_executable(override_path);
    }
    try {
        return find_executable("bwrap");
    } catch (const std::invalid_argument&) {
        throw std::invalid_argument(
            "Linux 安全命令执行需要 bubblewrap；请安装 bwrap，或显式设置 MINT_BWRAP_PATH");
    }
}

std::vector<std::filesystem::path> sandbox_tool_paths(
    const std::unordered_map<std::string, std::filesystem::path>& resolved_programs) {
    std::vector<std::filesystem::path> paths;
    paths.reserve(resolved_programs.size() + 16);
    for (const auto& [label, executable] : resolved_programs) {
        (void)label;
        paths.push_back(executable);
    }

    append_path_list(paths, "PATH");
    append_path_list(paths, "CMAKE_PREFIX_PATH");
    append_path_list(paths, "PKG_CONFIG_PATH");
    append_single_path(paths, "VCPKG_ROOT");
    append_single_path(paths, "CMAKE_TOOLCHAIN_FILE");
    append_single_path(paths, "SDKROOT");
    append_single_path(paths, "DEVELOPER_DIR");
    append_single_path(paths, "CC");
    append_single_path(paths, "CXX");

    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

SandboxConfig linux_sandbox_config(
    const std::filesystem::path& root,
    const std::unordered_map<std::string, std::filesystem::path>& resolved_programs,
    std::vector<std::filesystem::path> read_only_paths,
    std::vector<std::filesystem::path> denied_read_paths) {
    read_only_paths =
        normalize_read_only_paths(root, std::move(read_only_paths), denied_read_paths);
    SandboxConfig config{.executable = bubblewrap_executable(),
                         .arguments = {"bwrap", "--die-with-parent", "--new-session",
                                       "--unshare-user", "--unshare-ipc", "--unshare-pid",
                                       "--unshare-net", "--unshare-uts", "--unshare-cgroup-try",
                                       "--cap-drop", "ALL", "--ro-bind", "/", "/"},
                         .backend = "linux-bubblewrap",
                         .sets_working_directory = true};

    append_option(config.arguments, "--dev", "/dev");
    append_option(config.arguments, "--proc", "/proc");

    std::vector<std::filesystem::path> masked_roots;
    for (const auto& path : {std::filesystem::path("/tmp"), std::filesystem::path("/run")}) {
        std::error_code error;
        if (std::filesystem::is_directory(path, error) && !error) {
            append_option(config.arguments, "--tmpfs", path);
            masked_roots.push_back(path);
        }
    }

    if (const char* home_value = std::getenv("HOME"); home_value != nullptr) {
        if (const auto home = normalized_existing_path(home_value);
            home.has_value() && std::filesystem::is_directory(*home) && !is_inside(root, *home) &&
            !is_covered_by(masked_roots, *home)) {
            append_option(config.arguments, "--tmpfs", *home);
            masked_roots.push_back(*home);
        }
    }

    for (const auto& path : sandbox_tool_paths(resolved_programs)) {
        if (is_inside(root, path) || !is_covered_by(masked_roots, path)) {
            continue;
        }
        append_bind(config.arguments, "--ro-bind", path, path);
    }

    for (const auto& path : read_only_paths) {
        if (is_covered_by(masked_roots, path)) {
            append_bind(config.arguments, "--ro-bind", path, path);
        }
    }

    append_bind(config.arguments, "--bind", root, root);

    for (auto& denied : denied_read_paths) {
        std::error_code error;
        denied = std::filesystem::weakly_canonical(std::move(denied), error);
        if (error || denied.empty() || !std::filesystem::exists(denied, error) || error) {
            continue;
        }
        if (is_inside(denied, root)) {
            throw std::invalid_argument("命令保护路径不能包含整个工作区: " +
                                        denied.generic_string());
        }
        if (std::filesystem::is_directory(denied, error) && !error) {
            append_option(config.arguments, "--tmpfs", denied);
        } else {
            append_bind(config.arguments, "--ro-bind", "/dev/null", denied);
        }
    }

    config.arguments.insert(config.arguments.end(),
                            {"--setenv", "TMPDIR", "/tmp", "--setenv", "TMP", "/tmp", "--setenv",
                             "TEMP", "/tmp", "--hostname", "mint"});
    return config;
}

#endif

} // namespace

std::filesystem::path resolve_program(const std::string& requested) {
#if defined(_WIN32)
    if (requested.empty() || contains_nul(requested)) {
        throw std::invalid_argument("授权程序名称不能为空或包含 NUL");
    }
    const std::filesystem::path input(requested);
    const auto extension = lowercase_ascii(input.extension().string());
    if (extension == ".bat" || extension == ".cmd" || extension == ".ps1" || extension == ".vbs" ||
        extension == ".js" || extension == ".wsf" || extension == ".hta" || extension == ".lnk") {
        throw std::invalid_argument("当前版本不允许授权脚本或快捷方式启动器: " + requested);
    }
    if (is_blocked_launcher(input.filename().string())) {
        throw std::invalid_argument("当前版本不允许授权 shell、解释器、git 或通用命令启动器: " +
                                    requested);
    }
    return find_executable(requested);
#else
    if (requested.empty() || contains_nul(requested)) {
        throw std::invalid_argument("授权程序名称不能为空或包含 NUL");
    }

    const std::filesystem::path input(requested);
    if (is_blocked_launcher(input.filename().string())) {
        throw std::invalid_argument("当前版本不允许授权 shell、解释器、git 或通用命令启动器: " +
                                    requested);
    }
    return find_executable(requested);
#endif
}

SandboxConfig build_sandbox_config(
    bool required, const std::filesystem::path& root,
    const std::unordered_map<std::string, std::filesystem::path>& resolved_programs,
    std::vector<std::filesystem::path> read_only_paths,
    std::vector<std::filesystem::path> denied_read_paths) {
    if (!required) {
        if (!read_only_paths.empty()) {
            throw std::invalid_argument("命令外部只读路径需要启用操作系统沙箱");
        }
        return {};
    }

#if defined(__APPLE__)
    read_only_paths =
        normalize_read_only_paths(root, std::move(read_only_paths), denied_read_paths);
    SandboxConfig config{.executable = "/usr/bin/sandbox-exec", .backend = "macos-seatbelt"};
    if (!is_executable_file(config.executable)) {
        throw std::invalid_argument("当前主机缺少 /usr/bin/sandbox-exec，拒绝无 OS 沙箱执行命令");
    }

    const auto escaped_root = sandbox_string(root.generic_string());
    std::string profile = "(version 1) "
                          "(allow default) "
                          "(deny network*) "
                          "(deny file-write* (require-not (subpath \"" +
                          escaped_root + "\")))";

    if (const char* home_value = std::getenv("HOME"); home_value != nullptr) {
        std::error_code error;
        const auto home = std::filesystem::weakly_canonical(home_value, error);
        if (!error && home.is_absolute() && home != root) {
            profile += " (deny file-read* (require-all (subpath \"" +
                       sandbox_string(home.generic_string()) + "\") (require-not (subpath \"" +
                       escaped_root + "\"))";
            for (const auto& [label, executable] : resolved_programs) {
                (void)label;
                if (is_inside(home, executable) && !is_inside(root, executable)) {
                    profile += " (require-not (literal \"" +
                               sandbox_string(executable.generic_string()) + "\"))";
                }
            }
            for (const auto& path : read_only_paths) {
                if (is_inside(home, path)) {
                    profile +=
                        " (require-not (" +
                        std::string(std::filesystem::is_directory(path) ? "subpath" : "literal") +
                        " \"" + sandbox_string(path.generic_string()) + "\"))";
                }
            }
            profile += "))";
        }
    }

    for (auto& denied : denied_read_paths) {
        std::error_code error;
        denied = std::filesystem::weakly_canonical(std::move(denied), error);
        if (error || denied.empty()) {
            continue;
        }
        const bool directory = std::filesystem::is_directory(denied, error) && !error;
        profile += " (deny file-read* file-write* (" +
                   std::string(directory ? "subpath" : "literal") + " \"" +
                   sandbox_string(denied.generic_string()) + "\"))";
    }
    config.arguments = {"sandbox-exec", "-p", std::move(profile)};
    return config;
#elif defined(__linux__)
    return linux_sandbox_config(root, resolved_programs, std::move(read_only_paths),
                                std::move(denied_read_paths));
#elif defined(_WIN32)
    (void)root;
    SandboxConfig config{.backend = "windows-appcontainer",
                         .uses_native_process_sandbox = true,
                         .read_only_paths = std::move(read_only_paths),
                         .denied_paths = std::move(denied_read_paths)};
    config.allowed_executables.reserve(resolved_programs.size());
    for (const auto& [label, executable] : resolved_programs) {
        (void)label;
        config.allowed_executables.push_back(executable);
    }
    return config;
#else
    (void)root;
    (void)resolved_programs;
    (void)read_only_paths;
    (void)denied_read_paths;
    throw std::invalid_argument(
        "当前主机没有已实现的命令 OS 沙箱后端；若确实接受风险，显式关闭该策略");
#endif
}

} // namespace mint::command_detail
