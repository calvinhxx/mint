#include "mint/domain/runtime_settings.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace mint {
namespace {

constexpr std::array<std::string_view, 7> setting_names = {
    "read_file_bytes",  "list_max_entries",     "search_file_bytes", "search_max_hits",
    "search_max_files", "command_output_bytes", "command_resources"};

constexpr std::array<std::string_view, 5> resource_names = {
    "cpu_seconds", "memory_bytes", "max_processes", "file_size_bytes", "workspace_disk_bytes"};

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

std::size_t optional_limit(const Json& document, std::string_view field, std::size_t fallback,
                           std::size_t minimum, std::size_t maximum, std::string_view context) {
    const auto value = bounded_size(document, field, fallback, 0, maximum, context);
    if (value != 0 && value < minimum) {
        throw std::invalid_argument(std::string(context) + " 字段 " + std::string(field) +
                                    " 必须为 0，或在 " + std::to_string(minimum) + " 到 " +
                                    std::to_string(maximum) + " 之间");
    }
    return value;
}

void require_range(std::size_t value, std::size_t minimum, std::size_t maximum,
                   std::string_view field, std::string_view context) {
    if (value < minimum || value > maximum) {
        throw std::invalid_argument(std::string(context) + " 字段 " + std::string(field) +
                                    " 必须在 " + std::to_string(minimum) + " 到 " +
                                    std::to_string(maximum) + " 之间");
    }
}

void require_optional_range(std::size_t value, std::size_t minimum, std::size_t maximum,
                            std::string_view field, std::string_view context) {
    if (value == 0) {
        return;
    }
    require_range(value, minimum, maximum, field, context);
}

CommandResourceLimits parse_command_resource_limits(const Json& document,
                                                    std::string_view context) {
    if (!document.is_object()) {
        throw std::invalid_argument(std::string(context) + " 必须是对象");
    }
    for (const auto& [key, value] : document.items()) {
        (void)value;
        if (std::find(resource_names.begin(), resource_names.end(), key) == resource_names.end()) {
            throw std::invalid_argument(std::string(context) + " 包含未知字段: " + key);
        }
    }

    CommandResourceLimits limits;
    limits.cpu_seconds = optional_limit(document, "cpu_seconds", limits.cpu_seconds, 1,
                                        runtime_bounds::max_command_cpu_seconds, context);
    limits.memory_bytes = optional_limit(document, "memory_bytes", limits.memory_bytes,
                                         runtime_bounds::min_command_memory_bytes,
                                         runtime_bounds::max_command_memory_bytes, context);
    limits.max_processes = optional_limit(document, "max_processes", limits.max_processes, 1,
                                          runtime_bounds::max_command_processes, context);
    limits.file_size_bytes = optional_limit(document, "file_size_bytes", limits.file_size_bytes,
                                            runtime_bounds::min_command_file_size_bytes,
                                            runtime_bounds::max_command_file_size_bytes, context);
    limits.workspace_disk_bytes =
        optional_limit(document, "workspace_disk_bytes", limits.workspace_disk_bytes,
                       runtime_bounds::min_command_workspace_disk_bytes,
                       runtime_bounds::max_command_workspace_disk_bytes, context);
    return limits;
}

} // namespace

void validate_command_resource_limits(const CommandResourceLimits& limits,
                                      std::string_view context) {
    require_optional_range(limits.cpu_seconds, 1, runtime_bounds::max_command_cpu_seconds,
                           "cpu_seconds", context);
    require_optional_range(limits.memory_bytes, runtime_bounds::min_command_memory_bytes,
                           runtime_bounds::max_command_memory_bytes, "memory_bytes", context);
    require_optional_range(limits.max_processes, 1, runtime_bounds::max_command_processes,
                           "max_processes", context);
    require_optional_range(limits.file_size_bytes, runtime_bounds::min_command_file_size_bytes,
                           runtime_bounds::max_command_file_size_bytes, "file_size_bytes", context);
    require_optional_range(
        limits.workspace_disk_bytes, runtime_bounds::min_command_workspace_disk_bytes,
        runtime_bounds::max_command_workspace_disk_bytes, "workspace_disk_bytes", context);
}

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
    validate_command_resource_limits(settings.command_resources,
                                     std::string(context) + " command_resources");
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
    if (document.contains("command_resources")) {
        settings.command_resources = parse_command_resource_limits(
            document.at("command_resources"), std::string(context) + " command_resources");
    }
    return settings;
}

Json command_resource_limits_to_json(const CommandResourceLimits& limits) {
    return {{"cpu_seconds", limits.cpu_seconds},
            {"memory_bytes", limits.memory_bytes},
            {"max_processes", limits.max_processes},
            {"file_size_bytes", limits.file_size_bytes},
            {"workspace_disk_bytes", limits.workspace_disk_bytes}};
}

Json tool_runtime_settings_to_json(const ToolRuntimeSettings& settings) {
    Json result = {{"read_file_bytes", settings.read_file_bytes},
                   {"list_max_entries", settings.list_max_entries},
                   {"search_file_bytes", settings.search_file_bytes},
                   {"search_max_hits", settings.search_max_hits},
                   {"search_max_files", settings.search_max_files},
                   {"command_output_bytes", settings.command_output_bytes}};
    if (settings.command_resources != CommandResourceLimits{}) {
        result["command_resources"] = command_resource_limits_to_json(settings.command_resources);
    }
    return result;
}

} // namespace mint
