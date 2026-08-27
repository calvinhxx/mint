#pragma once

#include "mint/domain/model.hpp"

#include <cstddef>
#include <string_view>

namespace mint {

namespace runtime_defaults {

inline constexpr std::size_t max_turns = 12;
inline constexpr std::size_t max_context_bytes = 24 * 1024;
inline constexpr long max_seconds = 0;
inline constexpr long command_timeout_seconds = 60;
inline constexpr long max_command_timeout_seconds = 120;

inline constexpr std::size_t managed_max_turns = 24;
inline constexpr std::size_t managed_max_context_bytes = 128 * 1024;
inline constexpr long managed_max_seconds = 900;
inline constexpr long managed_recipe_timeout_seconds = 300;
inline constexpr std::size_t read_file_bytes = 16 * 1024;
inline constexpr std::size_t list_max_entries = 200;
inline constexpr std::size_t search_file_bytes = 1024 * 1024;
inline constexpr std::size_t search_max_hits = 100;
inline constexpr std::size_t search_max_files = 2000;
inline constexpr std::size_t command_output_bytes = 128 * 1024;

} // namespace runtime_defaults

namespace runtime_bounds {

inline constexpr std::size_t min_turns = 1;
inline constexpr std::size_t max_turns = 50;
inline constexpr std::size_t min_context_bytes = 16 * 1024;
inline constexpr std::size_t max_context_bytes = 8 * 1024 * 1024;
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

} // namespace runtime_bounds

struct ToolRuntimeSettings {
    std::size_t read_file_bytes = runtime_defaults::read_file_bytes;
    std::size_t list_max_entries = runtime_defaults::list_max_entries;
    std::size_t search_file_bytes = runtime_defaults::search_file_bytes;
    std::size_t search_max_hits = runtime_defaults::search_max_hits;
    std::size_t search_max_files = runtime_defaults::search_max_files;
    std::size_t command_output_bytes = runtime_defaults::command_output_bytes;

    bool operator==(const ToolRuntimeSettings&) const = default;
};

void validate_tool_runtime_settings(const ToolRuntimeSettings& settings,
                                    std::string_view context = "tool_limits");
[[nodiscard]] ToolRuntimeSettings
parse_tool_runtime_settings(const Json& document, std::string_view context = "tool_limits");
[[nodiscard]] Json tool_runtime_settings_to_json(const ToolRuntimeSettings& settings);

} // namespace mint
