#include "command_sandbox.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace mint::command_detail {
namespace {

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

#endif

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

} // namespace

std::filesystem::path resolve_program(const std::string& requested) {
#if defined(_WIN32)
    (void)requested;
    throw std::invalid_argument("当前受控命令执行暂未支持 Windows");
#else
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
#endif
}

SandboxConfig build_sandbox_config(
    bool required, const std::filesystem::path& root,
    const std::unordered_map<std::string, std::filesystem::path>& resolved_programs,
    std::vector<std::filesystem::path> denied_read_paths) {
    if (!required) {
        return {};
    }

#if defined(__APPLE__)
    SandboxConfig config{.executable = "/usr/bin/sandbox-exec", .backend = "macos-seatbelt"};
    if (!is_executable_file(config.executable)) {
        throw std::invalid_argument("当前主机缺少 /usr/bin/sandbox-exec，拒绝无 OS 沙箱执行命令");
    }

    const auto escaped_root = sandbox_string(root.generic_string());
    config.profile = "(version 1) "
                     "(allow default) "
                     "(deny network*) "
                     "(deny file-write* (require-not (subpath \"" +
                     escaped_root + "\")))";

    if (const char* home_value = std::getenv("HOME"); home_value != nullptr) {
        std::error_code error;
        const auto home = std::filesystem::weakly_canonical(home_value, error);
        if (!error && home.is_absolute() && home != root) {
            config.profile += " (deny file-read* (require-all (subpath \"" +
                              sandbox_string(home.generic_string()) +
                              "\") (require-not (subpath \"" + escaped_root + "\"))";
            for (const auto& [label, executable] : resolved_programs) {
                (void)label;
                if (is_inside(home, executable) && !is_inside(root, executable)) {
                    config.profile += " (require-not (literal \"" +
                                      sandbox_string(executable.generic_string()) + "\"))";
                }
            }
            config.profile += "))";
        }
    }

    for (auto& denied : denied_read_paths) {
        std::error_code error;
        denied = std::filesystem::weakly_canonical(std::move(denied), error);
        if (error || denied.empty()) {
            continue;
        }
        const bool directory = std::filesystem::is_directory(denied, error) && !error;
        config.profile += " (deny file-read* file-write* (" +
                          std::string(directory ? "subpath" : "literal") + " \"" +
                          sandbox_string(denied.generic_string()) + "\"))";
    }
    return config;
#else
    (void)root;
    (void)resolved_programs;
    (void)denied_read_paths;
    throw std::invalid_argument(
        "当前主机没有已实现的命令 OS 沙箱后端；若确实接受风险，显式关闭该策略");
#endif
}

} // namespace mint::command_detail
