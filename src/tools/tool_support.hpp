#pragma once

#include "mint/domain/model.hpp"

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

namespace mint::tools::detail {

[[nodiscard]] std::string dump_json(const Json& value);
[[nodiscard]] Json error_result(std::string message);
void require_only_fields(const Json& arguments, std::string_view context,
                         std::initializer_list<std::string_view> allowed_fields);
[[nodiscard]] std::string require_string(const Json& arguments, std::string_view name);
[[nodiscard]] bool is_entry_within(const std::filesystem::path& root,
                                   const std::filesystem::path& candidate);
[[nodiscard]] bool is_ignored_directory(const std::filesystem::path& path);
[[nodiscard]] bool contains_ignored_component(const std::filesystem::path& root,
                                              const std::filesystem::path& path);
[[nodiscard]] bool contains_text(std::string_view text, std::string_view query,
                                 bool case_sensitive);
[[nodiscard]] std::string shorten_line(std::string line);

} // namespace mint::tools::detail
