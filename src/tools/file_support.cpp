#include "file_support.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace mint::tools::detail {
namespace {

std::atomic_uint64_t temporary_file_sequence{0};

std::filesystem::path unique_sibling_path(const std::filesystem::path& target,
                                          std::string_view label) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto sequence = temporary_file_sequence.fetch_add(1);
        const auto name = "." + target.filename().string() + ".mint-" + std::string(label) + "-" +
                          std::to_string(stamp) + "-" + std::to_string(sequence);
        const auto candidate = target.parent_path() / name;

        std::error_code error;
        if (!std::filesystem::exists(candidate, error) && !error) {
            return candidate;
        }
    }
    throw std::runtime_error("无法为文件修改创建唯一的临时路径");
}

void remove_if_present(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void write_complete_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("无法创建临时修改文件");
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();
    if (!output) {
        throw std::runtime_error("写入临时修改文件失败");
    }
}

} // namespace

bool contains_nul(std::string_view data) {
    return data.find('\0') != std::string_view::npos;
}

bool is_valid_utf8(std::string_view data) {
    std::size_t index = 0;
    while (index < data.size()) {
        const auto first = static_cast<unsigned char>(data[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0;
        unsigned int code_point = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1;
            code_point = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2;
            code_point = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= data.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto next = static_cast<unsigned char>(data[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3FU);
        }
        if ((continuation_count == 2 && code_point < 0x800U) ||
            (continuation_count == 3 && code_point < 0x10000U) ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU) || code_point > 0x10FFFFU) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

std::string display_path(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(root);
    return relative.empty() ? "." : relative.generic_string();
}

void replace_file_safely(const std::filesystem::path& target, std::string_view content,
                         bool target_exists) {
    const auto temporary = unique_sibling_path(target, "tmp");
    try {
        write_complete_file(temporary, content);
    } catch (...) {
        remove_if_present(temporary);
        throw;
    }

    if (target_exists) {
        std::error_code permission_error;
        const auto permissions = std::filesystem::status(target, permission_error).permissions();
        if (permission_error) {
            remove_if_present(temporary);
            throw std::runtime_error("无法读取原文件权限");
        }
        std::filesystem::permissions(temporary, permissions, std::filesystem::perm_options::replace,
                                     permission_error);
        if (permission_error) {
            remove_if_present(temporary);
            throw std::runtime_error("无法保留原文件权限");
        }
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary, target, rename_error);
    if (!rename_error) {
        return;
    }
    if (!target_exists) {
        remove_if_present(temporary);
        throw std::runtime_error("无法安装新文件: " + rename_error.message());
    }

    const auto backup = unique_sibling_path(target, "backup");
    rename_error.clear();
    std::filesystem::rename(target, backup, rename_error);
    if (rename_error) {
        remove_if_present(temporary);
        throw std::runtime_error("无法暂存原文件: " + rename_error.message());
    }

    rename_error.clear();
    std::filesystem::rename(temporary, target, rename_error);
    if (rename_error) {
        std::error_code rollback_error;
        std::filesystem::rename(backup, target, rollback_error);
        remove_if_present(temporary);
        if (rollback_error) {
            throw std::runtime_error("安装修改失败，且自动恢复原文件失败: " +
                                     rollback_error.message());
        }
        throw std::runtime_error("安装修改失败，已恢复原文件: " + rename_error.message());
    }
    remove_if_present(backup);
}

void remove_file_safely(const std::filesystem::path& target) {
    std::error_code error;
    if (!std::filesystem::remove(target, error) || error) {
        throw std::runtime_error("无法删除文件: " + target.generic_string());
    }
}

} // namespace mint::tools::detail
