#pragma once

#include "command_process.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mint::command_detail {

struct SandboxConfig {
    std::filesystem::path executable{};
    std::vector<std::string> arguments{};
    std::string backend = "none";
    bool sets_working_directory = false;
    std::shared_ptr<const NativeProcessSandbox> native_sandbox{};
};

[[nodiscard]] std::filesystem::path resolve_program(const std::string& requested);

[[nodiscard]] SandboxConfig build_sandbox_config(
    bool required, const std::filesystem::path& root,
    const std::unordered_map<std::string, std::filesystem::path>& resolved_programs,
    std::vector<std::filesystem::path> read_only_paths,
    std::vector<std::filesystem::path> denied_read_paths);

[[nodiscard]] ProcessResult execute_sandboxed_process(ProcessRequest request,
                                                      const SandboxConfig& sandbox);

} // namespace mint::command_detail
