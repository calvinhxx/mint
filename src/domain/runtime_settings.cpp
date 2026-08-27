#include "mint/domain/runtime_settings.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace mint {
namespace {

constexpr std::array<std::string_view, 6> setting_names = {
    "read_file_bytes", "list_max_entries", "search_file_bytes",
    "search_max_hits", "search_max_files", "command_output_bytes"};

std::size_t bounded_size(const Json& document, std::string_view field, std::size_t fallback,
                         std::size_t minimum, std::size_t maximum, std::string_view context) {
    const std::string key(field);
    if (!document.contains(key)) {
        return fallback;
    }
    const auto& value = document.at(key);
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<long long>() < 0)) {
        throw std::invalid_argument(std::string(context) + " 字段 " + key + " 必须是非负整数");
    }
    const auto parsed = value.get<std::size_t>();
    if (parsed < minimum || parsed > maximum) {
        throw std::invalid_argument(std::string(context) + " 字段 " + key + " 必须在 " +
                                    std::to_string(minimum) + " 到 " + std::to_string(maximum) +
                                    " 之间");
    }
    return parsed;
}

void require_range(std::size_t value, std::size_t minimum, std::size_t maximum,
                   std::string_view field, std::string_view context) {
    if (value < minimum || value > maximum) {
        throw std::invalid_argument(std::string(context) + " 字段 " + std::string(field) +
                                    " 必须在 " + std::to_string(minimum) + " 到 " +
                                    std::to_string(maximum) + " 之间");
    }
}

} // namespace

void validate_tool_runtime_settings(const ToolRuntimeSettings& settings, std::string_view context) {
    require_range(settings.read_file_bytes, runtime_bounds::min_read_file_bytes,
                  runtime_bounds::max_read_file_bytes, "read_file_bytes", context);
    require_range(settings.list_max_entries, 1, runtime_bounds::max_list_entries,
                  "list_max_entries", context);
    require_range(settings.search_file_bytes, runtime_bounds::min_read_file_bytes,
                  runtime_bounds::max_search_file_bytes, "search_file_bytes", context);
    require_range(settings.search_max_hits, 1, runtime_bounds::max_search_hits, "search_max_hits",
                  context);
    require_range(settings.search_max_files, 1, runtime_bounds::max_search_files,
                  "search_max_files", context);
    require_range(settings.command_output_bytes, 1, runtime_bounds::max_command_output_bytes,
                  "command_output_bytes", context);
}

ToolRuntimeSettings parse_tool_runtime_settings(const Json& document, std::string_view context) {
    if (!document.is_object()) {
        throw std::invalid_argument(std::string(context) + " 必须是对象");
    }
    for (const auto& [key, value] : document.items()) {
        (void)value;
        if (std::find(setting_names.begin(), setting_names.end(), key) == setting_names.end()) {
            throw std::invalid_argument(std::string(context) + " 包含未知字段: " + key);
        }
    }

    ToolRuntimeSettings settings;
    settings.read_file_bytes = bounded_size(document, "read_file_bytes", settings.read_file_bytes,
                                            runtime_bounds::min_read_file_bytes,
                                            runtime_bounds::max_read_file_bytes, context);
    settings.list_max_entries =
        bounded_size(document, "list_max_entries", settings.list_max_entries, 1,
                     runtime_bounds::max_list_entries, context);
    settings.search_file_bytes = bounded_size(
        document, "search_file_bytes", settings.search_file_bytes,
        runtime_bounds::min_read_file_bytes, runtime_bounds::max_search_file_bytes, context);
    settings.search_max_hits = bounded_size(document, "search_max_hits", settings.search_max_hits,
                                            1, runtime_bounds::max_search_hits, context);
    settings.search_max_files =
        bounded_size(document, "search_max_files", settings.search_max_files, 1,
                     runtime_bounds::max_search_files, context);
    settings.command_output_bytes =
        bounded_size(document, "command_output_bytes", settings.command_output_bytes, 1,
                     runtime_bounds::max_command_output_bytes, context);
    return settings;
}

Json tool_runtime_settings_to_json(const ToolRuntimeSettings& settings) {
    return {{"read_file_bytes", settings.read_file_bytes},
            {"list_max_entries", settings.list_max_entries},
            {"search_file_bytes", settings.search_file_bytes},
            {"search_max_hits", settings.search_max_hits},
            {"search_max_files", settings.search_max_files},
            {"command_output_bytes", settings.command_output_bytes}};
}

} // namespace mint
