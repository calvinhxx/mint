#pragma once

#include "mint/application/agent.hpp"

#include <string>

namespace mint::agent_detail {

void record_execution(ExecutionSummary& summary, const ToolCall& call,
                      const std::string& raw_result);
[[nodiscard]] std::string verification_status(const ExecutionSummary& summary, bool has_changes);
[[nodiscard]] Json execution_to_json(const ExecutionSummary& summary);
[[nodiscard]] ExecutionSummary execution_from_json(const Json& value);
[[nodiscard]] Json tool_call_to_json(const ToolCall& call);
[[nodiscard]] ToolCall tool_call_from_json(const Json& value);

} // namespace mint::agent_detail
