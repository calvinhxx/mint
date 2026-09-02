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

} // namespace mint::private_path
