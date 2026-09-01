#pragma once

#include "mint/tools/tool_registry.hpp"

#include "workspace_change_tracker.hpp"

#include <optional>
#include <string_view>

namespace mint {

struct ToolRegistry::CommandWorkspaceState {
    std::optional<tools::detail::WorkspaceSnapshot> baseline;
};

namespace tools::detail {

inline constexpr std::string_view workspace_tracking_error_code = "workspace_reconciliation_failed";

} // namespace tools::detail
} // namespace mint
