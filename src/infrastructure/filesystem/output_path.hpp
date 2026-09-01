#pragma once

#include <filesystem>
#include <string_view>

namespace mint::infrastructure_detail {

enum class HardLinkPolicy { allow, reject };

[[nodiscard]] std::filesystem::path validated_output_path(std::filesystem::path path,
                                                          std::string_view label,
                                                          HardLinkPolicy hard_link_policy);

} // namespace mint::infrastructure_detail
