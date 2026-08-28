#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "mint/domain/runtime_settings.hpp"
#include "mint/domain/task_policy.hpp"

namespace mint::cli {

class Console;

enum class CommandMode { legacy, init, run, resume, status, provider };

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
    bool root_specified = false;
    std::filesystem::path config = "config.json";
    std::filesystem::path policy;
    std::filesystem::path root = std::filesystem::current_path();
    std::filesystem::path state_dir;
    std::filesystem::path events_jsonl;
    std::filesystem::path session;
    std::string task_id;
    std::vector<std::filesystem::path> allowed_write_paths;
    std::vector<std::filesystem::path> command_read_paths;
    std::vector<std::string> allowed_programs;
    std::vector<CommandRecipe> command_recipes;
    std::string policy_fingerprint;
    std::string log_level;
    std::size_t max_turns = runtime_defaults::max_turns;
    std::size_t max_context_bytes = runtime_defaults::max_context_bytes;
    long max_seconds = runtime_defaults::max_seconds;
    ToolRuntimeSettings tool_limits;
    std::string question;
};

[[nodiscard]] CommandLine parse_arguments(int argc, char** argv);
[[nodiscard]] std::filesystem::path normalized_path(std::filesystem::path path);
[[nodiscard]] bool requested_json_output(int argc, char** argv);
[[nodiscard]] bool is_managed_mode(CommandMode mode) noexcept;
void print_help(Console& console, const char* program);

} // namespace mint::cli
