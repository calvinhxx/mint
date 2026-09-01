#pragma once

#include "mint/infrastructure/diagnostic_log.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/common.h>
#include <spdlog/logger.h>

namespace mint::diagnostics::detail {

struct FileLogger {
    std::shared_ptr<spdlog::logger> logger;
    std::shared_ptr<std::atomic_bool> failed;
    std::filesystem::path path;
};

[[nodiscard]] spdlog::level::level_enum parse_level(std::string_view level);
[[nodiscard]] spdlog::level::level_enum backend_level(Level level) noexcept;
[[nodiscard]] std::string_view level_name(spdlog::level::level_enum level) noexcept;

[[nodiscard]] Json sanitize_fields(std::string_view event, const Json& fields);
[[nodiscard]] std::string console_message(std::string_view event, const Json& fields);
[[nodiscard]] Json file_record(Level level, std::string_view event, const Json& fields);
[[nodiscard]] bool known_event(std::string_view event) noexcept;
[[nodiscard]] long process_id() noexcept;
[[nodiscard]] std::string file_stamp();

[[nodiscard]] FileLogger create_file_logger(const LocalLogOptions& options,
                                            spdlog::level::level_enum level);

} // namespace mint::diagnostics::detail
