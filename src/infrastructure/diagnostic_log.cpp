#include "diagnostic_log.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace mint::diagnostics {
namespace {

spdlog::level::level_enum parse_level(std::string_view level) {
    const auto name = std::string(level.empty() ? default_level : level);
    const auto parsed = spdlog::level::from_str(name);
    if (parsed != spdlog::level::off || name == "off") {
        return parsed;
    }
    throw std::invalid_argument(
        "--log-level 只支持 trace、debug、info、warn、warning、error、critical 或 off");
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

std::shared_ptr<spdlog::logger> make_logger() {
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto result = std::make_shared<spdlog::logger>("mint", std::move(sink));
    result->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
    result->set_level(parse_level(default_level));
    result->flush_on(spdlog::level::warn);
    return result;
}

spdlog::logger& backend() {
    static const auto instance = make_logger();
    return *instance;
}

} // namespace

void configure(std::string_view level) {
    backend().set_level(parse_level(level));
}

std::string_view current_level() {
    return level_name(backend().level());
}

bool enabled(Level level) noexcept {
    try {
        return backend().should_log(backend_level(level));
    } catch (...) {
        return false;
    }
}

void emit(Level level, std::string_view event) noexcept {
    try {
        backend().log(backend_level(level), "event={}", event);
    } catch (...) {
    }
}

void emit(Level level, std::string_view event, const Json& fields) noexcept {
    try {
        auto& log = backend();
        const auto severity = backend_level(level);
        if (!log.should_log(severity)) {
            return;
        }
        log.log(severity, "event={} fields={}", event, fields.dump());
    } catch (...) {
    }
}

void flush() noexcept {
    try {
        backend().flush();
    } catch (...) {
    }
}

} // namespace mint::diagnostics
