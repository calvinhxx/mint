#pragma once

#include "mint/application/agent.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>

namespace mint::agent_detail {

struct RestoredSession {
    std::string user_request;
    Json messages;
    AgentResult result;
    std::deque<ToolCall> pending_calls;
    bool recovered_in_flight = false;
};

[[nodiscard]] Json make_checkpoint_document(const std::string& status,
                                            const std::string& user_request, const Json& messages,
                                            const AgentResult& result,
                                            const std::deque<ToolCall>& pending_calls,
                                            const std::optional<ToolCall>& in_flight_call,
                                            const ToolRegistry& tools, bool require_verification,
                                            std::size_t max_context_bytes);

[[nodiscard]] RestoredSession restore_session(const Json& snapshot, ToolRegistry& tools,
                                              bool require_verification,
                                              std::size_t max_context_bytes,
                                              bool retry_in_flight_tool);

} // namespace mint::agent_detail
