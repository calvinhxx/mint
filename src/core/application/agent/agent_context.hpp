#pragma once

#include "mint/domain/model.hpp"

#include <cstddef>

namespace mint::agent_detail {

struct CompactedContext {
    Json messages;
    std::size_t full_bytes = 0;
    std::size_t sent_bytes = 0;
    std::size_t dropped_groups = 0;
    std::size_t reused_prefix_bytes = 0;
    bool payloads_compacted = false;
    bool history_rebuilt = false;

    [[nodiscard]] std::size_t full_estimated_tokens() const noexcept {
        return model_token_estimation::from_serialized_bytes(full_bytes);
    }

    [[nodiscard]] std::size_t sent_estimated_tokens() const noexcept {
        return model_token_estimation::from_serialized_bytes(sent_bytes);
    }
};

[[nodiscard]] CompactedContext compact_context(const Json& messages, std::size_t byte_limit);

// EN: Request projections append between budget-driven rebuilds. Checkpoints keep full history.
// ZH-CN: 请求投影仅在预算触发重建时改写已有前缀，检查点仍保存完整历史。
class ContextWindow final {
  public:
    [[nodiscard]] CompactedContext prepare(const Json& messages, std::size_t byte_limit);

  private:
    Json projected_ = Json::array();
    std::size_t consumed_messages_ = 0;
};

} // namespace mint::agent_detail
