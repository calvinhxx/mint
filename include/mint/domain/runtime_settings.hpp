#pragma once

#include "mint/domain/model.hpp"

#include <cstddef>
#include <string_view>

namespace mint {

namespace runtime_defaults {

inline constexpr std::size_t max_turns = 12;
inline constexpr std::size_t max_context_bytes = 24 * 1024;
inline constexpr std::size_t max_total_tokens = 0;
inline constexpr long max_seconds = 0;
inline constexpr long command_timeout_seconds = 60;
inline constexpr long max_command_timeout_seconds = 120;

inline constexpr std::size_t managed_max_turns = 24;
inline constexpr std::size_t managed_max_context_bytes = 128 * 1024;
inline constexpr std::size_t managed_max_total_tokens = 100'000;
inline constexpr long managed_max_seconds = 900;
inline constexpr long managed_recipe_timeout_seconds = 300;
inline constexpr std::size_t managed_command_cpu_seconds = 300;
inline constexpr std::size_t managed_command_max_processes = 256;
inline constexpr std::size_t managed_command_workspace_disk_bytes =
    std::size_t{16} * 1024 * 1024 * 1024;
inline constexpr std::size_t read_file_bytes = 16 * 1024;
inline constexpr std::size_t list_max_entries = 200;
inline constexpr std::size_t search_file_bytes = 1024 * 1024;
inline constexpr std::size_t search_max_hits = 100;
inline constexpr std::size_t search_max_files = 2000;
inline constexpr std::size_t command_output_bytes = 128 * 1024;
inline constexpr std::size_t workspace_snapshot_entries = 20000;
inline constexpr std::size_t workspace_snapshot_bytes = std::size_t{256} * 1024 * 1024;
inline constexpr std::size_t workspace_snapshot_text_bytes = std::size_t{32} * 1024 * 1024;
inline constexpr std::size_t command_cpu_seconds = 0;
inline constexpr std::size_t command_memory_bytes = 0;
inline constexpr std::size_t command_max_processes = 0;
inline constexpr std::size_t command_file_size_bytes = 0;
inline constexpr std::size_t command_workspace_disk_bytes = 0;

} // namespace runtime_defaults

namespace runtime_bounds {

inline constexpr std::size_t min_turns = 1;
inline constexpr std::size_t max_turns = 50;
inline constexpr std::size_t min_context_bytes = 16 * 1024;
inline constexpr std::size_t max_context_bytes = 8 * 1024 * 1024;
inline constexpr std::size_t max_total_tokens = 100'000'000;
inline constexpr long max_seconds = 86400;
inline constexpr std::size_t max_command_arguments = 64;
inline constexpr std::size_t max_command_argument_bytes = 32 * 1024;
inline constexpr long max_recipe_timeout_seconds = 3600;
inline constexpr std::size_t min_read_file_bytes = 1024;
inline constexpr std::size_t max_read_file_bytes = 64 * 1024;
inline constexpr std::size_t max_edit_file_bytes = 256 * 1024;
inline constexpr std::size_t max_list_entries = 5000;
inline constexpr std::size_t max_search_file_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t max_search_hits = 1000;
inline constexpr std::size_t max_search_files = 20000;
inline constexpr std::size_t max_command_output_bytes = 1024 * 1024;
inline constexpr std::size_t max_command_cpu_seconds = 3600;
inline constexpr std::size_t min_command_memory_bytes = 64 * 1024 * 1024;
inline constexpr std::size_t max_command_memory_bytes = std::size_t{64} * 1024 * 1024 * 1024;
inline constexpr std::size_t max_command_processes = 1024;
inline constexpr std::size_t min_command_file_size_bytes = 1024;
inline constexpr std::size_t max_command_file_size_bytes = std::size_t{8} * 1024 * 1024 * 1024;
inline constexpr std::size_t min_command_workspace_disk_bytes = 1024;
inline constexpr std::size_t max_command_workspace_disk_bytes =
    std::size_t{1024} * 1024 * 1024 * 1024;
inline constexpr std::size_t max_workspace_snapshot_entries = 200000;
inline constexpr std::size_t max_workspace_snapshot_bytes = std::size_t{2} * 1024 * 1024 * 1024;
inline constexpr std::size_t max_workspace_snapshot_text_bytes = std::size_t{512} * 1024 * 1024;

} // namespace runtime_bounds

struct CommandResourceLimits {
    // Zero keeps the corresponding operating-system limit disabled.
    std::size_t cpu_seconds = runtime_defaults::command_cpu_seconds;
    std::size_t memory_bytes = runtime_defaults::command_memory_bytes;
    std::size_t max_processes = runtime_defaults::command_max_processes;
    std::size_t file_size_bytes = runtime_defaults::command_file_size_bytes;
    std::size_t workspace_disk_bytes = runtime_defaults::command_workspace_disk_bytes;

    bool operator==(const CommandResourceLimits&) const = default;
};

struct ToolRuntimeSettings {
    std::size_t read_file_bytes = runtime_defaults::read_file_bytes;
    std::size_t list_max_entries = runtime_defaults::list_max_entries;
    std::size_t search_file_bytes = runtime_defaults::search_file_bytes;
    std::size_t search_max_hits = runtime_defaults::search_max_hits;
    std::size_t search_max_files = runtime_defaults::search_max_files;
    std::size_t command_output_bytes = runtime_defaults::command_output_bytes;
    std::size_t workspace_snapshot_entries = runtime_defaults::workspace_snapshot_entries;
    std::size_t workspace_snapshot_bytes = runtime_defaults::workspace_snapshot_bytes;
    std::size_t workspace_snapshot_text_bytes = runtime_defaults::workspace_snapshot_text_bytes;
    CommandResourceLimits command_resources{};

    bool operator==(const ToolRuntimeSettings&) const = default;
};

void validate_command_resource_limits(const CommandResourceLimits& limits,
                                      std::string_view context = "command_resources");
void validate_tool_runtime_settings(const ToolRuntimeSettings& settings,
                                    std::string_view context = "tool_limits");
[[nodiscard]] ToolRuntimeSettings
parse_tool_runtime_settings(const Json& document, std::string_view context = "tool_limits");
[[nodiscard]] Json command_resource_limits_to_json(const CommandResourceLimits& limits);
[[nodiscard]] Json tool_runtime_settings_to_json(const ToolRuntimeSettings& settings);

} // namespace mint
