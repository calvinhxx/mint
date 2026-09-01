#pragma once

#include <string_view>

namespace mint::tools::name {

inline constexpr std::string_view list_files = "list_files";
inline constexpr std::string_view read_file = "read_file";
inline constexpr std::string_view search_text = "search_text";
inline constexpr std::string_view apply_patch = "apply_patch";
inline constexpr std::string_view apply_changeset = "apply_changeset";
inline constexpr std::string_view workspace_changes = "workspace_changes";
inline constexpr std::string_view run_command = "run_command";
inline constexpr std::string_view run_recipe = "run_recipe";

} // namespace mint::tools::name
