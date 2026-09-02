#include "diagnostic_log_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace mint::diagnostics::detail {
namespace {

constexpr std::size_t maximum_record_bytes = 16 * 1024;

enum class FieldKind { text, integer, number, boolean, resource_limits };

struct FieldRule {
    std::string_view key;
    FieldKind kind;
};

struct EventRule {
    std::string_view name;
    std::span<const FieldRule> fields;
};

constexpr auto model_start_fields = std::to_array<FieldRule>({
    {"provider", FieldKind::text},
    {"adapter", FieldKind::text},
    {"model", FieldKind::text},
    {"stream", FieldKind::boolean},
    {"message_count", FieldKind::integer},
    {"tool_count", FieldKind::integer},
    {"request_bytes", FieldKind::integer},
});
constexpr auto model_attempt_fields = std::to_array<FieldRule>({
    {"attempt", FieldKind::integer},
    {"max_attempts", FieldKind::integer},
});
constexpr auto model_completed_fields = std::to_array<FieldRule>({
    {"attempt", FieldKind::integer},
    {"http_status", FieldKind::integer},
    {"duration_ms", FieldKind::integer},
    {"streamed", FieldKind::boolean},
    {"request_token_limit", FieldKind::integer},
    {"usage_available", FieldKind::boolean},
    {"input_tokens", FieldKind::integer},
    {"cached_tokens", FieldKind::integer},
    {"cache_hit_rate", FieldKind::number},
    {"output_tokens", FieldKind::integer},
    {"total_tokens", FieldKind::integer},
});
constexpr auto model_failure_fields = std::to_array<FieldRule>({
    {"attempt", FieldKind::integer},
    {"curl_code", FieldKind::integer},
    {"http_status", FieldKind::integer},
    {"delay_ms", FieldKind::integer},
    {"outcome", FieldKind::text},
});
constexpr auto task_start_fields = std::to_array<FieldRule>({
    {"resumed", FieldKind::boolean},
    {"transaction_recovery", FieldKind::text},
    {"recovered_in_flight", FieldKind::boolean},
    {"previous_turns", FieldKind::integer},
    {"max_turns", FieldKind::integer},
    {"max_context_bytes", FieldKind::integer},
    {"max_total_tokens", FieldKind::integer},
    {"verification_required", FieldKind::boolean},
});
constexpr auto task_finish_fields = std::to_array<FieldRule>({
    {"status", FieldKind::text},
    {"stop_reason", FieldKind::text},
    {"turns", FieldKind::integer},
    {"duration_ms", FieldKind::integer},
    {"verification_status", FieldKind::text},
    {"tool_calls", FieldKind::integer},
    {"max_total_tokens", FieldKind::integer},
    {"reported_total_tokens", FieldKind::integer},
    {"token_usage_coverage", FieldKind::text},
    {"changed_file_count", FieldKind::integer},
});
constexpr auto tool_fields = std::to_array<FieldRule>({
    {"name", FieldKind::text},
    {"ok", FieldKind::boolean},
});
constexpr auto command_start_fields = std::to_array<FieldRule>({
    {"program", FieldKind::text},
    {"cwd", FieldKind::text},
    {"argument_count", FieldKind::integer},
    {"timeout_seconds", FieldKind::integer},
    {"resource_limits", FieldKind::resource_limits},
    {"sandbox", FieldKind::text},
});
constexpr auto command_finish_fields = std::to_array<FieldRule>({
    {"program", FieldKind::text},
    {"status", FieldKind::text},
    {"exit_code", FieldKind::integer},
    {"duration_ms", FieldKind::integer},
    {"output_truncated", FieldKind::boolean},
});
constexpr auto process_start_fields = std::to_array<FieldRule>({
    {"mode", FieldKind::text},
    {"json_output", FieldKind::boolean},
    {"interaction_jsonl", FieldKind::boolean},
    {"file_enabled", FieldKind::boolean},
});
constexpr auto process_finish_fields = std::to_array<FieldRule>({
    {"exit_code", FieldKind::integer},
    {"has_task", FieldKind::boolean},
    {"task_id", FieldKind::text},
});

constexpr auto event_rules = std::to_array<EventRule>({
    {"model.request.started", model_start_fields},
    {"model.request.attempt", model_attempt_fields},
    {"model.request.completed", model_completed_fields},
    {"model.request.retry_scheduled", model_failure_fields},
    {"model.request.failed", model_failure_fields},
    {"task.started", task_start_fields},
    {"task.finished", task_finish_fields},
    {"tool.completed", tool_fields},
    {"tool.failed", tool_fields},
    {"command.prepared", command_start_fields},
    {"command.completed", command_finish_fields},
    {"process.started", process_start_fields},
    {"process.finished", process_finish_fields},
    {"process.failed", process_finish_fields},
});

const EventRule* find_event(std::string_view name) noexcept {
    const auto found = std::ranges::find(event_rules, name, &EventRule::name);
    return found == event_rules.end() ? nullptr : &*found;
}

std::tm utc_time(std::time_t value) noexcept {
    std::tm result{};
#if defined(_WIN32)
    gmtime_s(&result, &value);
#else
    gmtime_r(&value, &result);
#endif
    return result;
}

std::string timestamp(std::string_view format, bool milliseconds_before_utc) {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto utc = utc_time(seconds);
    std::ostringstream output;
    output << std::put_time(&utc, format.data());
    if (milliseconds_before_utc) {
        output << '.';
    }
    output << std::setw(3) << std::setfill('0') << milliseconds << 'Z';
    return output.str();
}

std::optional<FieldKind> field_kind(std::string_view event, std::string_view key) {
    const auto* schema = find_event(event);
    if (schema == nullptr) {
        return std::nullopt;
    }
    const auto found = std::ranges::find(schema->fields, key, &FieldRule::key);
    return found == schema->fields.end() ? std::nullopt : std::optional(found->kind);
}

bool allowed_resource_limit(std::string_view key) {
    return key == "cpu_seconds" || key == "memory_bytes" || key == "max_processes" ||
           key == "file_size_bytes" || key == "workspace_disk_bytes";
}

std::string bounded_string(std::string_view value) {
    constexpr std::size_t maximum_characters = 256;
    std::string result;
    result.reserve(std::min(value.size(), maximum_characters));
    std::size_t index = 0;
    while (index < value.size() && result.size() < maximum_characters) {
        const auto lead = static_cast<unsigned char>(value[index]);
        std::size_t width = 0;
        if (lead <= 0x7f) {
            width = 1;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            width = 2;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            width = 3;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            width = 4;
        }
        bool valid = width != 0 && index + width <= value.size();
        for (std::size_t offset = 1; valid && offset < width; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            valid = (continuation & 0xc0) == 0x80;
        }
        if (valid && width == 3) {
            const auto second = static_cast<unsigned char>(value[index + 1]);
            valid = (lead != 0xe0 || second >= 0xa0) && (lead != 0xed || second < 0xa0);
        }
        if (valid && width == 4) {
            const auto second = static_cast<unsigned char>(value[index + 1]);
            valid = (lead != 0xf0 || second >= 0x90) && (lead != 0xf4 || second <= 0x8f);
        }
        if (!valid) {
            result.push_back('?');
            ++index;
            continue;
        }
        if (result.size() + width > maximum_characters) {
            break;
        }
        if (width == 1 && std::iscntrl(lead) != 0) {
            result.push_back('?');
        } else {
            result.append(value.substr(index, width));
        }
        index += width;
    }
    if (index < value.size()) {
        result += "...[truncated]";
    }
    return result;
}

std::string program_name(std::string_view value) {
    const auto separator = value.find_last_of("/\\");
    if (separator != std::string::npos) {
        value.remove_prefix(separator + 1);
    }
    return bounded_string(value);
}

Json sanitize_value(FieldKind kind, std::string_view key, const Json& value) {
    if (kind == FieldKind::resource_limits && value.is_object()) {
        Json result = Json::object();
        for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
            if (allowed_resource_limit(iterator.key()) && iterator.value().is_number_integer()) {
                result[iterator.key()] = iterator.value();
            }
        }
        return result;
    }
    if (kind == FieldKind::text && value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        return key == "program" ? program_name(text) : bounded_string(text);
    }
    if (kind == FieldKind::boolean && value.is_boolean()) {
        return value;
    }
    if (kind == FieldKind::integer && (value.is_number_integer() || value.is_number_unsigned())) {
        return value;
    }
    if (kind == FieldKind::number && value.is_number()) {
        return value;
    }
    return nullptr;
}

} // namespace

long process_id() noexcept {
#if defined(_WIN32)
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

std::string file_stamp() {
    return timestamp("%Y%m%dT%H%M%S", false);
}

Json sanitize_fields(std::string_view event, const Json& fields) {
    Json result = Json::object();
    std::size_t omitted = 0;
    if (!fields.is_object()) {
        result["omitted_field_count"] = 1;
        return result;
    }
    for (auto iterator = fields.begin(); iterator != fields.end(); ++iterator) {
        const auto kind = field_kind(event, iterator.key());
        if (!kind.has_value()) {
            ++omitted;
            continue;
        }
        auto value = sanitize_value(*kind, iterator.key(), iterator.value());
        if (value.is_null()) {
            ++omitted;
            continue;
        }
        result[iterator.key()] = std::move(value);
    }
    if (omitted != 0) {
        result["omitted_field_count"] = omitted;
    }
    return result;
}

bool known_event(std::string_view event) noexcept {
    return find_event(event) != nullptr;
}

Json file_record(Level level, std::string_view event, const Json& fields) {
    std::ostringstream thread;
    thread << std::this_thread::get_id();
    Json record = {{"schema_version", 1},
                   {"timestamp", timestamp("%Y-%m-%dT%H:%M:%S", true)},
                   {"level", level_name(backend_level(level))},
                   {"event", bounded_string(std::string(event))},
                   {"process_id", process_id()},
                   {"thread_id", thread.str()},
                   {"fields", fields}};
    const auto bytes = record.dump().size();
    if (bytes > maximum_record_bytes) {
        record["fields"] = {{"fields_truncated", true}, {"record_bytes", bytes}};
    }
    return record;
}

} // namespace mint::diagnostics::detail
