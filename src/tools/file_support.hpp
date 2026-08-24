#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aiagent::tools::detail {

inline constexpr std::uintmax_t max_edit_file_bytes = 256 * 1024;

[[nodiscard]] bool contains_nul(std::string_view data);
[[nodiscard]] bool is_valid_utf8(std::string_view data);
[[nodiscard]] std::string display_path(const std::filesystem::path& root,
                                       const std::filesystem::path& path);
void replace_file_safely(const std::filesystem::path& target, std::string_view content,
                         bool target_exists);
void remove_file_safely(const std::filesystem::path& target);

} // namespace aiagent::tools::detail
