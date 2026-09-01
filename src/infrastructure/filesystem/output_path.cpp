#include "output_path.hpp"

#include <stdexcept>
#include <string>

namespace mint::infrastructure_detail {

std::filesystem::path validated_output_path(std::filesystem::path path, std::string_view label,
                                            HardLinkPolicy hard_link_policy) {
    const std::string name(label);
    if (path.empty() || path.filename().empty()) {
        throw std::invalid_argument(name + "路径不能为空且必须包含文件名");
    }

    std::error_code error;
    auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::current_path();
    parent = std::filesystem::weakly_canonical(parent, error);
    if (error || !std::filesystem::is_directory(parent)) {
        throw std::invalid_argument(name + "父目录不存在或不是目录");
    }

    const auto resolved = parent / path.filename();
    const auto status = std::filesystem::symlink_status(resolved, error);
    if (!error && std::filesystem::is_symlink(status)) {
        throw std::invalid_argument(name + "路径不能是符号链接");
    }
    if (!error && std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) {
        throw std::invalid_argument(name + "已有路径必须是普通文件");
    }
    if (!error && hard_link_policy == HardLinkPolicy::reject &&
        std::filesystem::is_regular_file(status)) {
        const auto links = std::filesystem::hard_link_count(resolved, error);
        if (!error && links > 1) {
            throw std::invalid_argument(name + "路径不能是多重硬链接文件");
        }
    }
    return resolved;
}

} // namespace mint::infrastructure_detail
