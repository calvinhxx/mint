#pragma once

#include "mint/domain/model.hpp"
#include "mint/domain/runtime_settings.hpp"

#include <string>

namespace mint::tools::detail {

[[nodiscard]] Json workspace_tool_definitions(bool allow_write, const ToolRuntimeSettings& runtime);
[[nodiscard]] std::string summarize_tool_call(const ToolCall& call);

} // namespace mint::tools::detail
