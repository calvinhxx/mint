#include "diagnostic_log_internal.hpp"
#include "filesystem/private_path.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>

namespace mint::diagnostics::detail {
namespace {

bool mint_log_filename(std::string_view filename) {
    constexpr std::string_view prefix = "mint-";
    constexpr std::string_view extension = ".jsonl";
    constexpr std::size_t stamp_size = 19;
    if (!filename.starts_with(prefix) || filename.size() <= prefix.size() + stamp_size + 4) {
        return false;
    }

    const auto stamp = filename.substr(prefix.size(), stamp_size);
    const auto digits = [](std::string_view text) {
        return !text.empty() && std::ranges::all_of(text, [](unsigned char value) {
            return std::isdigit(value) != 0;
        });
    };
    if (stamp[8] != 'T' || stamp[18] != 'Z' || !digits(stamp.substr(0, 8)) ||
        !digits(stamp.substr(9, 9))) {
        return false;
    }

    auto suffix = filename.substr(prefix.size() + stamp_size);
    if (!suffix.starts_with("-p")) {
        return false;
    }
    suffix.remove_prefix(2);
    const auto process_end = suffix.find('-');
    if (process_end == std::string_view::npos || !digits(suffix.substr(0, process_end))) {
        return false;
    }
    suffix.remove_prefix(process_end + 1);
    const auto sequence_end = suffix.find('.');
    if (sequence_end == std::string_view::npos || !digits(suffix.substr(0, sequence_end))) {
        return false;
    }
    suffix.remove_prefix(sequence_end);
    if (suffix == extension) {
        return true;
    }
    if (!suffix.starts_with('.')) {
        return false;
    }
    suffix.remove_prefix(1);
    const auto rotation_end = suffix.find(extension);
    return rotation_end != std::string_view::npos && digits(suffix.substr(0, rotation_end)) &&
           suffix.substr(rotation_end) == extension;
}

struct ExistingLog {
    std::filesystem::path path;
    std::filesystem::file_time_type modified;
    std::uintmax_t bytes = 0;
};

void cleanup_logs(const LocalLogOptions& options) noexcept {
    try {
        const auto now = std::filesystem::file_time_type::clock::now();
        const auto retention = std::chrono::hours(24 * options.retention_days);
        const auto recent_guard = std::chrono::hours(24);
        std::vector<ExistingLog> retained;
        std::uintmax_t total_bytes = 0;
        for (const auto& entry : std::filesystem::directory_iterator(options.directory)) {
            std::error_code error;
            const auto status = entry.symlink_status(error);
            if (error || std::filesystem::is_symlink(status) ||
                !std::filesystem::is_regular_file(status) ||
                !mint_log_filename(entry.path().filename().string())) {
                continue;
            }
            const auto modified = entry.last_write_time(error);
            if (error) {
                continue;
            }
            if (options.retention_days != 0 && now - modified > retention) {
                std::filesystem::remove(entry.path(), error);
                continue;
            }
            const auto bytes = entry.file_size(error);
            if (error) {
                continue;
            }
            retained.push_back({entry.path(), modified, bytes});
            total_bytes += bytes;
        }
        std::ranges::sort(retained, {}, &ExistingLog::modified);
        for (const auto& entry : retained) {
            if (total_bytes <= options.max_directory_bytes) {
                break;
            }
            if (now - entry.modified < recent_guard) {
                continue;
            }
            std::error_code error;
            if (std::filesystem::remove(entry.path, error) && !error) {
                total_bytes -= std::min(total_bytes, entry.bytes);
            }
        }
    } catch (...) {
    }
}

std::filesystem::path prepare_log_file(const LocalLogOptions& options) {
    if (!options.initialization_error.empty()) {
        throw std::runtime_error(options.initialization_error);
    }
    if (options.directory.empty()) {
        throw std::invalid_argument("本地日志目录不能为空");
    }
    if (options.max_file_bytes == 0 || options.rotated_files == 0 ||
        options.max_directory_bytes == 0) {
        throw std::invalid_argument("本地日志轮转参数必须大于零");
    }

    std::error_code error;
    if (!options.managed_root.empty()) {
        private_path::ensure_directory(options.managed_root, "mint 状态目录");
    }

    private_path::ensure_directory(options.directory, "本地日志目录");
    cleanup_logs(options);

    static std::atomic_uint64_t sequence{0};
    const auto filename = "mint-" + file_stamp() + "-p" + std::to_string(process_id()) + '-' +
                          std::to_string(++sequence) + ".jsonl";
    const auto path = options.directory / filename;
    const auto existing = std::filesystem::symlink_status(path, error);
    if (!error && existing.type() != std::filesystem::file_type::not_found) {
        throw std::runtime_error("本地日志文件名发生冲突");
    }
    return path;
}

} // namespace

FileLogger create_file_logger(const LocalLogOptions& options, spdlog::level::level_enum level) {
    FileLogger result;
    result.path = prepare_log_file(options);
    result.failed = std::make_shared<std::atomic_bool>(false);
    spdlog::file_event_handlers events;
    events.after_open = [](const spdlog::filename_t& filename, std::FILE* stream) {
        private_path::secure_open_file(std::filesystem::path(filename), stream);
    };
    auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        result.path.native(), options.max_file_bytes, options.rotated_files, false, events);
    result.logger = std::make_shared<spdlog::logger>("mint.file", std::move(sink));
    result.logger->set_pattern("%v");
    result.logger->set_level(level);
    result.logger->flush_on(spdlog::level::trace);
    result.logger->set_error_handler(
        [failure = result.failed](const std::string&) { failure->store(true); });
    return result;
}

} // namespace mint::diagnostics::detail
