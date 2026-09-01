#include "mint/infrastructure/diagnostic_log.hpp"
#include "diagnostic_log_internal.hpp"
#include "mint/localization/localization.hpp"
#include "mint/runtime/terminal_text.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace mint::diagnostics {

using localization::Message;
using localization::Placeholder;
namespace {

struct Backend {
    std::mutex mutex;
    std::shared_ptr<spdlog::logger> console;
    std::shared_ptr<spdlog::logger> file;
    std::shared_ptr<std::atomic_bool> file_failed;
    LogStatus status;
    spdlog::level::level_enum console_level = spdlog::level::warn;
};

std::shared_ptr<spdlog::logger> make_console_logger(spdlog::level::level_enum level) {
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto result = std::make_shared<spdlog::logger>("mint", std::move(sink));
    result->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
    result->set_level(level);
    result->flush_on(spdlog::level::warn);
    result->set_error_handler([](const std::string&) {});
    return result;
}

Backend& backend() {
    static Backend instance;
    static const bool initialized = [] {
        instance.console = make_console_logger(detail::parse_level(default_level));
        return true;
    }();
    (void)initialized;
    return instance;
}

} // namespace

namespace detail {

spdlog::level::level_enum parse_level(std::string_view level) {
    const auto name = std::string(level.empty() ? default_level : level);
    const auto parsed = spdlog::level::from_str(name);
    if (parsed != spdlog::level::off || name == "off") {
        return parsed;
    }
    throw std::invalid_argument(localization::message(Message::logging_invalid_level));
}

spdlog::level::level_enum backend_level(Level level) noexcept {
    switch (level) {
    case Level::trace:
        return spdlog::level::trace;
    case Level::debug:
        return spdlog::level::debug;
    case Level::info:
        return spdlog::level::info;
    case Level::warning:
        return spdlog::level::warn;
    case Level::error:
        return spdlog::level::err;
    case Level::critical:
        return spdlog::level::critical;
    case Level::off:
        return spdlog::level::off;
    }
    return spdlog::level::off;
}

std::string_view level_name(spdlog::level::level_enum level) noexcept {
    switch (level) {
    case spdlog::level::trace:
        return "trace";
    case spdlog::level::debug:
        return "debug";
    case spdlog::level::info:
        return "info";
    case spdlog::level::warn:
        return "warn";
    case spdlog::level::err:
        return "error";
    case spdlog::level::critical:
        return "critical";
    case spdlog::level::off:
        return "off";
    case spdlog::level::n_levels:
        break;
    }
    return "unknown";
}

std::string console_message(std::string_view event, const Json& fields) {
    constexpr std::string_view event_prefix = "event=";
    constexpr std::string_view fields_prefix = " fields=";
    const auto safe_event = escape_terminal_field(event);
    const auto safe_fields = escape_terminal_field(fields.dump());
    std::string message;
    message.reserve(event_prefix.size() + safe_event.size() + fields_prefix.size() +
                    safe_fields.size());
    message.append(event_prefix).append(safe_event).append(fields_prefix).append(safe_fields);
    return message;
}

} // namespace detail

void configure(std::string_view level) {
    const auto parsed = detail::parse_level(level);
    auto& state = backend();
    std::scoped_lock lock(state.mutex);
    state.console = make_console_logger(parsed);
    state.console_level = parsed;
    state.file.reset();
    state.file_failed.reset();
    state.status = {};
}

LogStatus configure_local(LocalLogOptions options) {
    const auto console_level = detail::parse_level(options.console_level);
    const auto file_level = detail::parse_level(options.file_level);
    auto console = options.console_enabled ? make_console_logger(console_level) : nullptr;
    detail::FileLogger file;
    LogStatus status;

    if (file_level != spdlog::level::off) {
        try {
            file = detail::create_file_logger(options, file_level);
            status.file_enabled = true;
            status.file_path = file.path;
        } catch (const std::exception& error) {
            status.error = error.what();
        }
    }

    auto& state = backend();
    {
        std::scoped_lock lock(state.mutex);
        state.console = std::move(console);
        state.file = std::move(file.logger);
        state.file_failed = std::move(file.failed);
        state.status = status;
        state.console_level = console_level;
    }
    return status;
}

void validate_level(std::string_view level) {
    (void)detail::parse_level(level);
}

std::string_view current_level() {
    auto& state = backend();
    std::scoped_lock lock(state.mutex);
    return detail::level_name(state.console_level);
}

LogStatus current_status() {
    auto& state = backend();
    std::scoped_lock lock(state.mutex);
    return state.status;
}

bool enabled(Level level) noexcept {
    try {
        auto& state = backend();
        std::scoped_lock lock(state.mutex);
        const auto severity = detail::backend_level(level);
        return (state.console != nullptr && state.console->should_log(severity)) ||
               (state.file != nullptr && state.file->should_log(severity));
    } catch (...) {
        return false;
    }
}

void emit(Level level, std::string_view event) noexcept {
    emit(level, event, Json::object());
}

void emit(Level level, std::string_view event, const Json& fields) noexcept {
    try {
        if (!detail::known_event(event)) {
            return;
        }
        auto& state = backend();
        std::scoped_lock lock(state.mutex);
        const auto severity = detail::backend_level(level);
        const auto safe_fields = detail::sanitize_fields(event, fields);
        if (state.console != nullptr && state.console->should_log(severity)) {
            state.console->log(severity, "{}", detail::console_message(event, safe_fields));
        }
        if (state.file != nullptr && state.file->should_log(severity)) {
            state.file->log(severity, "{}", detail::file_record(level, event, safe_fields).dump());
            if (state.file_failed != nullptr && state.file_failed->exchange(false)) {
                state.file.reset();
                state.file_failed.reset();
                state.status.file_enabled = false;
                state.status.error = localization::message(Message::logging_file_disabled);
                if (state.console != nullptr) {
                    state.console->warn("event=diagnostics.file_disabled");
                }
            }
        }
    } catch (...) {
    }
}

void flush() noexcept {
    try {
        auto& state = backend();
        std::scoped_lock lock(state.mutex);
        if (state.console != nullptr) {
            state.console->flush();
        }
        if (state.file != nullptr) {
            state.file->flush();
        }
    } catch (...) {
    }
}

void shutdown() noexcept {
    try {
        auto& state = backend();
        std::scoped_lock lock(state.mutex);
        if (state.console != nullptr) {
            state.console->flush();
        }
        if (state.file != nullptr) {
            state.file->flush();
        }
        state.file.reset();
        state.file_failed.reset();
        state.console = make_console_logger(detail::parse_level(default_level));
        state.console_level = detail::parse_level(default_level);
        state.status = {};
    } catch (...) {
    }
}

} // namespace mint::diagnostics
