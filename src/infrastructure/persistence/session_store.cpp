#include "mint/infrastructure/session_store.hpp"

#include "filesystem/output_path.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace mint {
namespace {

constexpr std::uintmax_t max_snapshot_bytes = 16 * 1024 * 1024;
std::atomic_uint64_t snapshot_sequence{0};

std::filesystem::path temporary_path(const std::filesystem::path& target) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return target.parent_path() /
           ("." + target.filename().string() + ".tmp-" + std::to_string(stamp) + "-" +
            std::to_string(snapshot_sequence.fetch_add(1)));
}

} // namespace

SessionStore::SessionStore(std::filesystem::path path)
    : path_(infrastructure_detail::validated_output_path(
          std::move(path), "会话快照", infrastructure_detail::HardLinkPolicy::allow)) {}

const std::filesystem::path& SessionStore::path() const noexcept {
    return path_;
}

bool SessionStore::exists() const {
    std::error_code error;
    return std::filesystem::is_regular_file(path_, error) && !error;
}

Json SessionStore::load() const {
    std::error_code error;
    const auto size = std::filesystem::file_size(path_, error);
    if (error) {
        throw std::runtime_error("无法读取会话快照: " + path_.string());
    }
    if (size > max_snapshot_bytes) {
        throw std::runtime_error("会话快照超过 16 MiB 上限");
    }
    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        throw std::runtime_error("无法打开会话快照: " + path_.string());
    }
    try {
        Json snapshot;
        input >> snapshot;
        return snapshot;
    } catch (const Json::exception& error_message) {
        throw std::runtime_error("会话快照不是有效 JSON: " + std::string(error_message.what()));
    }
}

void SessionStore::save(const Json& snapshot) const {
    if (!snapshot.is_object()) {
        throw std::invalid_argument("会话快照必须是 JSON 对象");
    }
    const auto encoded = snapshot.dump(2);
    if (encoded.size() > max_snapshot_bytes) {
        throw std::runtime_error("会话快照超过 16 MiB 上限");
    }

    const auto temporary = temporary_path(path_);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("无法创建会话临时文件");
        }
        output << encoded << '\n';
        output.close();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw std::runtime_error("写入会话临时文件失败");
        }
    }

    std::error_code permission_error;
    std::filesystem::permissions(
        temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, permission_error);
    if (permission_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("无法把会话快照权限限制为当前用户: " + permission_error.message());
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary, path_, rename_error);
    if (rename_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("安装会话快照失败: " + rename_error.message());
    }
}

} // namespace mint
