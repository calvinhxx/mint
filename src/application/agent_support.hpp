#pragma once

#include "aiagent/application/agent.hpp"

#include <cstddef>
#include <ostream>
#include <string>

namespace aiagent::agent_detail {

struct CompactedContext {
    Json messages;
    std::size_t full_bytes = 0;
    std::size_t sent_bytes = 0;
    std::size_t dropped_groups = 0;
    bool payloads_compacted = false;

    [[nodiscard]] std::size_t full_estimated_tokens() const noexcept {
        return (full_bytes + 3) / 4;
    }

    [[nodiscard]] std::size_t sent_estimated_tokens() const noexcept {
        return (sent_bytes + 3) / 4;
    }
};

[[nodiscard]] CompactedContext compact_context(const Json& messages, std::size_t byte_limit);

[[nodiscard]] Json model_usage_json(const ModelUsage& usage);
[[nodiscard]] Json model_metadata_json(const ModelCallMetadata& metadata);
void record_model_call(ModelSummary& summary, const ModelReply& reply);
[[nodiscard]] Json model_summary_to_json(const ModelSummary& summary);
[[nodiscard]] ModelSummary model_summary_from_json(const Json& value);
void print_model_usage(std::ostream& output, const ModelUsage& usage);

void record_execution(ExecutionSummary& summary, const ToolCall& call,
                      const std::string& raw_result);
[[nodiscard]] std::string verification_status(const ExecutionSummary& summary, bool has_changes);
[[nodiscard]] Json execution_to_json(const ExecutionSummary& summary);
[[nodiscard]] ExecutionSummary execution_from_json(const Json& value);
[[nodiscard]] Json tool_call_to_json(const ToolCall& call);
[[nodiscard]] ToolCall tool_call_from_json(const Json& value);

void capture_final_state(AgentResult& result, const ToolRegistry& tools);
void print_final_state(std::ostream& output, const AgentResult& result);
[[nodiscard]] Json safe_tool_result(const std::string& raw_result);
[[nodiscard]] Json event_arguments_summary(const ToolRegistry& tools, const ToolCall& call);

} // namespace aiagent::agent_detail
