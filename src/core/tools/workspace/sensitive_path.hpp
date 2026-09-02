#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

namespace mint::tools::detail {

struct SensitivePathScan {
    std::vector<std::filesystem::path> paths;
    bool truncated = false;
};

[[nodiscard]] bool is_sensitive_path(const std::filesystem::path& root,
                                     const std::filesystem::path& path);
[[nodiscard]] bool is_reserved_write_path(const std::filesystem::path& root,
                                          const std::filesystem::path& path);
[[nodiscard]] SensitivePathScan scan_sensitive_paths(const std::filesystem::path& root,
                                                     std::size_t max_entries);

} // namespace mint::tools::detail
