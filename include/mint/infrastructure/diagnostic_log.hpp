#pragma once

#include "mint/domain/model.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace mint::diagnostics {

inline constexpr std::string_view default_level = "warn";
inline constexpr std::string_view default_file_level = "info";
inline constexpr std::uintmax_t default_max_file_bytes = 5 * 1024 * 1024;
inline constexpr std::size_t default_rotated_files = 2;
inline constexpr unsigned default_retention_days = 7;
inline constexpr std::uintmax_t default_max_directory_bytes = 100 * 1024 * 1024;

enum class Level { trace, debug, info, warning, error, critical, off };

struct LocalLogOptions {
    std::filesystem::path directory;
    std::filesystem::path managed_root;
    std::string initialization_error;
    std::string console_level{default_level};
    std::string file_level{default_file_level};
    bool console_enabled = true;
    std::uintmax_t max_file_bytes = default_max_file_bytes;
    std::size_t rotated_files = default_rotated_files;
    unsigned retention_days = default_retention_days;
    std::uintmax_t max_directory_bytes = default_max_directory_bytes;
};

struct LogStatus {
    bool file_enabled = false;
    std::filesystem::path file_path;
    std::string error;
};

void configure(std::string_view level);
[[nodiscard]] LogStatus configure_local(LocalLogOptions options);
void validate_level(std::string_view level);
[[nodiscard]] std::string_view current_level();
[[nodiscard]] LogStatus current_status();
[[nodiscard]] bool enabled(Level level) noexcept;
void emit(Level level, std::string_view event) noexcept;
void emit(Level level, std::string_view event, const Json& fields) noexcept;
void flush() noexcept;
void shutdown() noexcept;

} // namespace mint::diagnostics
