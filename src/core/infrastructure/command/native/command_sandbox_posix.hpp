#pragma once

#include <filesystem>
#include <vector>

namespace mint::command_detail::native {

[[nodiscard]] bool is_executable_file(const std::filesystem::path& path);
[[nodiscard]] std::vector<std::filesystem::path>
normalize_read_only_paths(const std::filesystem::path& workspace,
                          std::vector<std::filesystem::path> paths,
                          const std::vector<std::filesystem::path>& denied_paths);

} // namespace mint::command_detail::native
