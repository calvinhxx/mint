#include "../command_sandbox_backend.hpp"
#include "command_sandbox_posix.hpp"

#include "mint/localization/localization.hpp"
#include "mint/runtime/path.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mint::command_detail::native {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

bool contains_nul(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

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
                       [&](const auto& root) { return is_path_within(root, candidate); });
}

std::filesystem::path bubblewrap_executable() {
    if (const char* override_path = std::getenv("MINT_BWRAP_PATH");
        override_path != nullptr && *override_path != '\0') {
        return find_executable(override_path);
    }
    try {
        return find_executable("bwrap");
    } catch (const std::invalid_argument&) {
        throw std::invalid_argument(message(Message::command_sandbox_bubblewrap_required));
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

} // namespace

SandboxConfig build_sandbox_config(
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
            home.has_value() && std::filesystem::is_directory(*home) &&
            !is_path_within(root, *home) && !is_covered_by(masked_roots, *home)) {
            append_option(config.arguments, "--tmpfs", *home);
            masked_roots.push_back(*home);
        }
    }

    for (const auto& path : sandbox_tool_paths(resolved_programs)) {
        if (is_path_within(root, path) || !is_covered_by(masked_roots, path)) {
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
        if (error || denied.empty()) {
            continue;
        }
        if (is_path_within(denied, root)) {
            throw std::invalid_argument(
                message(Message::command_sandbox_protected_contains_workspace,
                        {arg(Placeholder::path, denied.generic_string())}));
        }
        const bool exists = std::filesystem::exists(denied, error);
        if (error) {
            continue;
        }
        if (!exists) {
            continue;
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

} // namespace mint::command_detail::native

namespace mint::command_detail {

ProcessResult execute_sandboxed_process(ProcessRequest request, const SandboxConfig&) {
    return execute_process(std::move(request));
}

} // namespace mint::command_detail
