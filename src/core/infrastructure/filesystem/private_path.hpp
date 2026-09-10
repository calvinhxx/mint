#pragma once

#include <cstdio>
#include <filesystem>
#include <string_view>

namespace mint::private_path {

enum class ExistingDirectoryPolicy { require_private, migrate_owned };

void ensure_directory(
    const std::filesystem::path& path, std::string_view description,
    ExistingDirectoryPolicy existing_policy = ExistingDirectoryPolicy::require_private);

[[nodiscard]] bool create_directory(const std::filesystem::path& path,
                                    std::string_view description);

void secure_open_file(const std::filesystem::path& path, std::FILE* stream);

// EN: Enforce the platform's existing owner-only policy on an already opened file.
// ZH-CN: 对已打开文件执行平台既有的仅属主可访问策略。
void verify_open_file(std::FILE* stream);

} // namespace mint::private_path
