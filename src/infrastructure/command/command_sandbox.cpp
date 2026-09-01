#include "command_sandbox.hpp"

#include "mint/localization/localization.hpp"
#include "mint/runtime/path.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#if !defined(_WIN32)
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/stat.h>
#endif
#endif

namespace mint::command_detail {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

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
            throw std::invalid_argument(message(Message::command_sandbox_read_path_invalid));
        }
        if (is_path_within(workspace, path) || is_path_within(path, workspace)) {
            throw std::invalid_argument(
                message(Message::command_sandbox_read_path_workspace_overlap,
                        {arg(Placeholder::path, path.generic_string())}));
        }
        for (const auto& denied_input : denied_paths) {
            error.clear();
            const auto denied = std::filesystem::weakly_canonical(denied_input, error);
            if (!error && !denied.empty() &&
                (is_path_within(path, denied) || is_path_within(denied, path))) {
                throw std::invalid_argument(
                    message(Message::command_sandbox_read_path_protected_overlap,
                            {arg(Placeholder::path, path.generic_string())}));
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
        throw std::invalid_argument(message(Message::command_program_empty));
    }

    const std::filesystem::path input(requested);
    if (input.has_parent_path()) {
        if (!input.is_absolute()) {
            throw std::invalid_argument(
                message(Message::command_program_absolute, {arg(Placeholder::program, requested)}));
        }
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(input, error);
        if (error || !is_executable_file(resolved)) {
            throw std::invalid_argument(message(Message::command_program_not_executable,
                                                {arg(Placeholder::program, requested)}));
        }
        return resolved;
    }

    for (const auto character : requested) {
        const auto value = static_cast<unsigned char>(character);
        if (!std::isalnum(value) && character != '_' && character != '-' && character != '+' &&
            character != '.') {
            throw std::invalid_argument(message(Message::command_program_invalid_characters,
                                                {arg(Placeholder::program, requested)}));
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
    throw std::invalid_argument(
        message(Message::command_program_not_found, {arg(Placeholder::program, requested)}));
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
        throw std::invalid_argument(message(Message::command_program_not_executable,
                                            {arg(Placeholder::program, requested)}));
    }
    return resolved;
}

std::filesystem::path find_executable(const std::string& requested) {
    if (requested.empty() || contains_nul(requested)) {
        throw std::invalid_argument(message(Message::command_program_empty));
    }

    const std::filesystem::path input(requested);
    if (input.has_parent_path()) {
        if (!input.is_absolute()) {
            throw std::invalid_argument(
                message(Message::command_program_absolute, {arg(Placeholder::program, requested)}));
        }
        return canonical_executable(input, requested);
    }

    for (const auto character : requested) {
        const auto value = static_cast<unsigned char>(character);
        if (!std::isalnum(value) && character != '_' && character != '-' && character != '+' &&
            character != '.') {
            throw std::invalid_argument(message(Message::command_program_invalid_characters,
                                                {arg(Placeholder::program, requested)}));
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
    throw std::invalid_argument(
        message(Message::command_program_not_found, {arg(Placeholder::program, requested)}));
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
                       [&](const auto& root) { return is_path_within(root, candidate); });
}

bool is_reserved_workspace_directory(const std::filesystem::path& root,
                                     const std::filesystem::path& path) {
    if (path.parent_path() != root) {
        return false;
    }
    static const std::unordered_set<std::string> names = {".agents", ".codex", ".git", ".husky"};
    return names.contains(path.filename().string());
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
            if (is_reserved_workspace_directory(root, denied)) {
                append_option(config.arguments, "--tmpfs", denied);
            }
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

#endif

} // namespace

#if defined(__APPLE__)

namespace {

bool same_scratch_identity(const struct stat& status, dev_t device, ino_t inode) noexcept {
    return S_ISDIR(status.st_mode) && status.st_dev == device && status.st_ino == inode;
}

[[noreturn]] void throw_scratch_identity_changed() {
    throw std::runtime_error(message(Message::command_scratch_identity_changed));
}

} // namespace

struct CommandScratchDirectory::State {
    std::filesystem::path path;
    dev_t device{};
    ino_t inode{};
};

CommandScratchDirectory::CommandScratchDirectory(const std::filesystem::path& workspace) {
    auto state = std::make_unique<State>();
    auto pattern = (workspace / ".mint-command-tmp-XXXXXX").string();
    const auto* created = ::mkdtemp(pattern.data());
    if (created == nullptr) {
        throw std::runtime_error(message(Message::command_scratch_create_failed,
                                         {arg(Placeholder::error, std::strerror(errno))}));
    }

    state->path = std::move(pattern);
    struct stat created_status = {};
    if (::lstat(state->path.c_str(), &created_status) != 0 || !S_ISDIR(created_status.st_mode)) {
        throw std::runtime_error(message(Message::command_scratch_identity_failed));
    }
    state->device = created_status.st_dev;
    state->inode = created_status.st_ino;

    std::string setup_error;
    struct stat status = {};
    if (::chmod(state->path.c_str(), S_IRWXU) != 0) {
        setup_error = std::strerror(errno);
    } else if (::lstat(state->path.c_str(), &status) != 0) {
        setup_error = std::strerror(errno);
    } else if (!same_scratch_identity(status, state->device, state->inode) ||
               status.st_uid != ::geteuid() || (status.st_mode & 07777) != S_IRWXU) {
        setup_error = message(Message::command_scratch_permission_check_failed);
    } else {
        state_ = std::move(state);
        return;
    }

    if (::lstat(state->path.c_str(), &status) != 0 ||
        !same_scratch_identity(status, state->device, state->inode)) {
        throw std::runtime_error(message(Message::command_scratch_init_identity_changed,
                                         {arg(Placeholder::error, setup_error)}));
    }
    if (::rmdir(state->path.c_str()) != 0) {
        throw std::runtime_error(message(Message::command_scratch_init_cleanup_failed,
                                         {arg(Placeholder::setup_error, setup_error),
                                          arg(Placeholder::cleanup_error, std::strerror(errno))}));
    }
    throw std::runtime_error(
        message(Message::command_scratch_init_failed, {arg(Placeholder::error, setup_error)}));
}

CommandScratchDirectory::~CommandScratchDirectory() = default;

const std::filesystem::path& CommandScratchDirectory::path() const noexcept {
    return state_->path;
}

void CommandScratchDirectory::verify_cleanup_target() const {
    struct stat status = {};
    if (state_ == nullptr || ::lstat(state_->path.c_str(), &status) != 0 ||
        !same_scratch_identity(status, state_->device, state_->inode)) {
        throw_scratch_identity_changed();
    }
}

void CommandScratchDirectory::confirm_cleanup() {
    if (state_ == nullptr) {
        return;
    }
    struct stat status = {};
    if (::lstat(state_->path.c_str(), &status) == 0) {
        throw std::runtime_error(message(Message::command_scratch_still_exists));
    }
    if (errno != ENOENT) {
        throw std::runtime_error(message(Message::command_scratch_cleanup_confirmation_failed,
                                         {arg(Placeholder::error, std::strerror(errno))}));
    }
    state_.reset();
}

#endif

std::filesystem::path resolve_program(const std::string& requested) {
#if defined(_WIN32)
    if (requested.empty() || contains_nul(requested)) {
        throw std::invalid_argument(message(Message::command_program_authorization_empty));
    }
    const std::filesystem::path input(requested);
    const auto extension = lowercase_ascii(input.extension().string());
    if (extension == ".bat" || extension == ".cmd" || extension == ".ps1" || extension == ".vbs" ||
        extension == ".js" || extension == ".wsf" || extension == ".hta" || extension == ".lnk") {
        throw std::invalid_argument(message(Message::command_program_script_forbidden,
                                            {arg(Placeholder::program, requested)}));
    }
    if (is_blocked_launcher(input.filename().string())) {
        throw std::invalid_argument(message(Message::command_program_launcher_forbidden,
                                            {arg(Placeholder::program, requested)}));
    }
    return find_executable(requested);
#else
    if (requested.empty() || contains_nul(requested)) {
        throw std::invalid_argument(message(Message::command_program_authorization_empty));
    }

    const std::filesystem::path input(requested);
    if (is_blocked_launcher(input.filename().string())) {
        throw std::invalid_argument(message(Message::command_program_launcher_forbidden,
                                            {arg(Placeholder::program, requested)}));
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
            throw std::invalid_argument(
                message(Message::command_sandbox_read_path_requires_sandbox));
        }
        return {};
    }

#if defined(__APPLE__)
    read_only_paths =
        normalize_read_only_paths(root, std::move(read_only_paths), denied_read_paths);
    SandboxConfig config{.executable = "/usr/bin/sandbox-exec", .backend = "macos-seatbelt"};
    if (!is_executable_file(config.executable)) {
        throw std::invalid_argument(message(Message::command_sandbox_seatbelt_missing));
    }

    const auto escaped_root = sandbox_string(root.generic_string());
    std::string profile = "(version 1) "
                          "(allow default) "
                          "(deny network*) "
                          "(deny file-write* (require-all (require-not (subpath \"" +
                          escaped_root + "\")) (require-not (literal \"/dev/null\"))))";

    if (const char* home_value = std::getenv("HOME"); home_value != nullptr) {
        std::error_code error;
        const auto home = std::filesystem::weakly_canonical(home_value, error);
        if (!error && home.is_absolute() && home != root) {
            profile += " (deny file-read* (require-all (subpath \"" +
                       sandbox_string(home.generic_string()) + "\") (require-not (subpath \"" +
                       escaped_root + "\"))";
            for (const auto& [label, executable] : resolved_programs) {
                (void)label;
                if (is_path_within(home, executable) && !is_path_within(root, executable)) {
                    profile += " (require-not (literal \"" +
                               sandbox_string(executable.generic_string()) + "\"))";
                }
            }
            for (const auto& path : read_only_paths) {
                if (is_path_within(home, path)) {
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
    throw std::invalid_argument(message(Message::command_sandbox_backend_unavailable));
#endif
}

} // namespace mint::command_detail
