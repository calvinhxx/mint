#pragma once

#include "mint/application/agent.hpp"

#include <ostream>
#include <string>

namespace mint::agent_detail {

void capture_final_state(AgentResult& result, const AgentTools& tools);
void print_final_state(std::ostream& output, const AgentResult& result);
[[nodiscard]] Json safe_tool_result(const std::string& raw_result);
[[nodiscard]] Json event_arguments_summary(const AgentTools& tools, const ToolCall& call);

} // namespace mint::agent_detail
