#pragma once

#include "command_sandbox.hpp"

namespace mint::command_detail::native {

void validate_program_file(const std::filesystem::path& input, const std::string& requested);
[[nodiscard]] std::filesystem::path find_executable(const std::string& requested);
[[nodiscard]] SandboxConfig build_sandbox_config(
    const std::filesystem::path& root,
    const std::unordered_map<std::string, std::filesystem::path>& resolved_programs,
    std::vector<std::filesystem::path> read_only_paths,
    std::vector<std::filesystem::path> denied_read_paths);

} // namespace mint::command_detail::native
