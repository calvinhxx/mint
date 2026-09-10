#include "command_sandbox.hpp"

#include "command_sandbox_backend.hpp"
#include "mint/localization/localization.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

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

} // namespace

std::filesystem::path resolve_program(const std::string& requested) {
    if (requested.empty() || contains_nul(requested)) {
        throw std::invalid_argument(message(Message::command_program_authorization_empty));
    }
    const std::filesystem::path input(requested);
    native::validate_program_file(input, requested);
    if (is_blocked_launcher(input.filename().string())) {
        throw std::invalid_argument(message(Message::command_program_launcher_forbidden,
                                            {arg(Placeholder::program, requested)}));
    }
    return native::find_executable(requested);
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
    return native::build_sandbox_config(root, resolved_programs, std::move(read_only_paths),
                                        std::move(denied_read_paths));
}

} // namespace mint::command_detail
