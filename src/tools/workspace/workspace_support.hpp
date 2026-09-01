#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace mint::tools::detail {

[[nodiscard]] bool is_entry_within(const std::filesystem::path& root,
                                   const std::filesystem::path& candidate);
[[nodiscard]] bool is_ignored_directory(const std::filesystem::path& path);
[[nodiscard]] bool contains_ignored_component(const std::filesystem::path& root,
                                              const std::filesystem::path& path);
[[nodiscard]] bool contains_text(std::string_view text, std::string_view query,
                                 bool case_sensitive);
[[nodiscard]] std::string shorten_line(std::string line);

} // namespace mint::tools::detail
