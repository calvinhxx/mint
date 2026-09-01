#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "mint/domain/model.hpp"
#include "mint/domain/runtime_settings.hpp"

namespace mint {

struct CommandRecipe {
    std::string name{};
    std::string description{};
    std::string program{};
    std::vector<std::string> args{};
    std::filesystem::path cwd = ".";
    long timeout_seconds = runtime_defaults::command_timeout_seconds;
    bool verification = false;
};

struct TaskPolicy {
    std::filesystem::path source_path{};
    std::vector<std::filesystem::path> write_paths{};
    std::vector<std::filesystem::path> command_read_paths{};
    std::vector<CommandRecipe> recipes{};
    bool require_verification = false;
    std::size_t max_turns = runtime_defaults::max_turns;
    std::size_t max_context_bytes = runtime_defaults::max_context_bytes;
    long max_seconds = runtime_defaults::max_seconds;
    std::string fingerprint{};
    ToolRuntimeSettings tool_limits{};
};

[[nodiscard]] TaskPolicy load_task_policy(const std::filesystem::path& path);
[[nodiscard]] TaskPolicy parse_task_policy(const Json& document,
                                           std::filesystem::path source_path = {});

} // namespace mint
