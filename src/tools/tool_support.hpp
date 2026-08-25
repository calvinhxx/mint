#pragma once

#include "aiagent/domain/model.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace aiagent::tools::detail {

inline constexpr std::uintmax_t max_read_bytes = 64 * 1024;

[[nodiscard]] std::string dump_json(const Json& value);
[[nodiscard]] Json error_result(std::string message);
[[nodiscard]] std::string require_string(const Json& arguments, std::string_view name);
[[nodiscard]] bool is_inside(const std::filesystem::path& root,
                             const std::filesystem::path& candidate);
[[nodiscard]] bool is_ignored_directory(const std::filesystem::path& path);
[[nodiscard]] bool contains_ignored_component(const std::filesystem::path& root,
                                              const std::filesystem::path& path);
[[nodiscard]] std::string lowercase_ascii(std::string value);
[[nodiscard]] std::string shorten_line(std::string line);

} // namespace aiagent::tools::detail
