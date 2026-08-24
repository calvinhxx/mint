#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace aiagent {

struct CommandRecipe {
    std::string name;
    std::string description;
    std::string program;
    std::vector<std::string> args;
    std::filesystem::path cwd = ".";
    long timeout_seconds = 60;
    bool verification = false;
};

struct TaskPolicy {
    std::filesystem::path source_path;
    std::vector<std::filesystem::path> write_paths;
    std::vector<CommandRecipe> recipes;
    bool require_verification = false;
    std::size_t max_turns = 12;
    std::size_t max_context_bytes = 24 * 1024;
    long max_seconds = 0;
    std::string fingerprint;
};

[[nodiscard]] TaskPolicy load_task_policy(const std::filesystem::path& path);

} // namespace aiagent
