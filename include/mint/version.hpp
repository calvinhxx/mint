#pragma once

#include <string_view>

namespace mint {

inline constexpr std::string_view version = "1.0.0";
inline constexpr int session_schema_version = 5;
inline constexpr int workspace_change_schema_version = 3;
inline constexpr int policy_schema_version = 1;

} // namespace mint
