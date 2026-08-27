#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace mint::command_detail {

struct SandboxConfig {
    std::filesystem::path executable;
    std::string profile;
    std::string backend = "none";
};

[[nodiscard]] std::filesystem::path resolve_program(const std::string& requested);

[[nodiscard]] SandboxConfig build_sandbox_config(
    bool required, const std::filesystem::path& root,
    const std::unordered_map<std::string, std::filesystem::path>& resolved_programs,
    std::vector<std::filesystem::path> denied_read_paths);

} // namespace mint::command_detail
