#include "mint/infrastructure/event_log.hpp"

#include "output_path.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mint {
namespace {

constexpr std::uintmax_t max_existing_event_bytes = 64 * 1024 * 1024;

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;

    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
           << milliseconds << 'Z';
    return output.str();
}

} // namespace

EventLog::EventLog(std::filesystem::path path, bool append)
    : path_(infrastructure_detail::validated_output_path(
          std::move(path), "事件日志", infrastructure_detail::HardLinkPolicy::reject)) {
    bool needs_separator = false;
    if (append && std::filesystem::exists(path_)) {
        std::error_code size_error;
        const auto size = std::filesystem::file_size(path_, size_error);
        if (size_error || size > max_existing_event_bytes) {
            throw std::runtime_error("已有事件日志无法读取或超过 64 MiB 上限");
        }
        std::ifstream input(path_, std::ios::binary);
        std::string line;
        while (std::getline(input, line)) {
            try {
                const auto event = Json::parse(line);
                if (event.contains("seq") && event.at("seq").is_number_unsigned()) {
                    sequence_ = std::max(sequence_, event.at("seq").get<std::size_t>());
                }
            } catch (const Json::exception&) {
                // A crash may leave one incomplete final line. New events remain valid JSONL.
            }
        }
        if (size > 0) {
            input.clear();
            input.seekg(-1, std::ios::end);
            char last = '\0';
            input.get(last);
            needs_separator = last != '\n';
        }
    }

    const auto mode = std::ios::binary | std::ios::out | (append ? std::ios::app : std::ios::trunc);
    output_.open(path_, mode);
    if (!output_) {
        throw std::runtime_error("无法打开事件日志: " + path_.string());
    }
    if (needs_separator) {
        output_ << '\n';
        output_.flush();
        if (!output_) {
            throw std::runtime_error("无法修复事件日志末尾: " + path_.string());
        }
    }
    std::error_code permission_error;
    std::filesystem::permissions(
        path_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, permission_error);
    if (permission_error) {
        throw std::runtime_error("无法把事件日志权限限制为当前用户: " + permission_error.message());
    }
}

const std::filesystem::path& EventLog::path() const noexcept {
    return path_;
}

void EventLog::emit(std::string type, Json data) {
    if (type.empty()) {
        throw std::invalid_argument("事件类型不能为空");
    }
    if (!data.is_object()) {
        throw std::invalid_argument("事件数据必须是 JSON 对象");
    }

    std::scoped_lock lock(mutex_);
    Json event = {{"schema_version", 1},
                  {"seq", ++sequence_},
                  {"timestamp", utc_timestamp()},
                  {"type", std::move(type)},
                  {"data", std::move(data)}};
    output_ << event.dump() << '\n';
    output_.flush();
    if (!output_) {
        throw std::runtime_error("写入事件日志失败: " + path_.string());
    }
}

} // namespace mint
