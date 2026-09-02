#pragma once

#include "mint/domain/model.hpp"

#include <cstddef>

namespace mint::agent_detail {

struct CompactedContext {
    Json messages;
    std::size_t full_bytes = 0;
    std::size_t sent_bytes = 0;
    std::size_t dropped_groups = 0;
    bool payloads_compacted = false;

    [[nodiscard]] std::size_t full_estimated_tokens() const noexcept {
        return model_token_estimation::from_serialized_bytes(full_bytes);
    }

    [[nodiscard]] std::size_t sent_estimated_tokens() const noexcept {
        return model_token_estimation::from_serialized_bytes(sent_bytes);
    }
};

[[nodiscard]] CompactedContext compact_context(const Json& messages, std::size_t byte_limit);

} // namespace mint::agent_detail
