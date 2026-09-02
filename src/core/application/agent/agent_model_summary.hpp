#pragma once

#include "mint/application/agent.hpp"

#include <ostream>

namespace mint::agent_detail {

[[nodiscard]] Json model_usage_json(const ModelUsage& usage);
[[nodiscard]] Json model_metadata_json(const ModelCallMetadata& metadata);
void record_model_call(ModelSummary& summary, const ModelReply& reply);
[[nodiscard]] bool token_budget_exhausted(const ModelSummary& summary) noexcept;
[[nodiscard]] Json token_budget_to_json(const ModelSummary& summary);
[[nodiscard]] Json model_summary_to_json(const ModelSummary& summary);
[[nodiscard]] ModelSummary model_summary_from_json(const Json& value);
void print_model_usage(std::ostream& output, const ModelUsage& usage);

} // namespace mint::agent_detail
