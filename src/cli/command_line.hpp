#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "aiagent/domain/task_policy.hpp"

namespace aiagent::cli {

enum class CommandMode {
    legacy,
    init,
    run,
    resume,
    status
};

struct CommandLine {
    CommandMode mode = CommandMode::legacy;
    bool demo = false;
    bool help = false;
    bool version = false;
    bool force = false;
    bool allow_write = false;
    bool require_verification = false;
    bool approve_each_command = false;
    bool approve_each_changeset = false;
    bool unsafe_no_command_sandbox = false;
    bool json_output = false;
    bool resume_session = false;
    bool retry_inflight = false;
    bool policy_conflict = false;
    bool config_specified = false;
    std::filesystem::path config = "config.json";
    std::filesystem::path policy;
    std::filesystem::path root = std::filesystem::current_path();
    std::filesystem::path state_dir;
    std::filesystem::path events_jsonl;
    std::filesystem::path session;
    std::string task_id;
    std::vector<std::filesystem::path> allowed_write_paths;
    std::vector<std::string> allowed_programs;
    std::vector<CommandRecipe> command_recipes;
    std::string policy_fingerprint;
    std::size_t max_turns = 12;
    std::size_t max_context_bytes = 24 * 1024;
    long max_seconds = 0;
    std::string question;
};

[[nodiscard]] CommandLine parse_arguments(int argc, char** argv);
[[nodiscard]] std::filesystem::path normalized_path(std::filesystem::path path);
[[nodiscard]] bool requested_json_output(int argc, char** argv);
[[nodiscard]] bool is_managed_mode(CommandMode mode) noexcept;
void print_help(const char* program);

} // namespace aiagent::cli
