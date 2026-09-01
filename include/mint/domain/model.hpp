#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace mint {

using Json = nlohmann::json;

namespace model_token_estimation {

// Two serialized bytes per token is deliberately conservative for mixed CJK,
// code and JSON payloads. Providers with a known tokenizer may override it.
inline constexpr std::size_t serialized_bytes_per_token = 2;

[[nodiscard]] constexpr std::size_t
from_serialized_bytes(std::size_t bytes,
                      std::size_t bytes_per_token = serialized_bytes_per_token) noexcept {
    const auto divisor = bytes_per_token == 0 ? std::size_t{1} : bytes_per_token;
    return bytes / divisor + (bytes % divisor == 0 ? 0 : 1);
}

[[nodiscard]] constexpr std::size_t
to_serialized_bytes(std::size_t tokens,
                    std::size_t bytes_per_token = serialized_bytes_per_token) noexcept {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    const auto multiplier = bytes_per_token == 0 ? std::size_t{1} : bytes_per_token;
    return tokens > maximum / multiplier ? maximum : tokens * multiplier;
}

} // namespace model_token_estimation

struct ToolCall {
    std::string id{};
    std::string name{};
    Json arguments{};
};

struct ModelUsage {
    bool available = false;
    std::size_t prompt_tokens = 0;
    std::size_t completion_tokens = 0;
    std::size_t total_tokens = 0;
    std::size_t cached_tokens = 0;
};

namespace model_usage {

// Cache hits are always measured against normalized prompt/input tokens. Providers use
// different wire names for these values, but protocol adapters populate ModelUsage before this
// calculation. A response with no prompt tokens has no meaningful hit-rate denominator.
[[nodiscard]] constexpr std::optional<double> cache_hit_rate(std::size_t cached_tokens,
                                                             std::size_t prompt_tokens) noexcept {
    if (prompt_tokens == 0) {
        return std::nullopt;
    }
    const auto bounded_cached_tokens =
        cached_tokens > prompt_tokens ? prompt_tokens : cached_tokens;
    return static_cast<double>(bounded_cached_tokens) / static_cast<double>(prompt_tokens);
}

[[nodiscard]] constexpr std::optional<double> cache_hit_rate(const ModelUsage& usage) noexcept {
    return cache_hit_rate(usage.cached_tokens, usage.prompt_tokens);
}

[[nodiscard]] inline Json cache_hit_rate_json(std::size_t cached_tokens,
                                              std::size_t prompt_tokens) {
    const auto rate = cache_hit_rate(cached_tokens, prompt_tokens);
    return rate.has_value() ? Json(*rate) : Json(nullptr);
}

[[nodiscard]] inline Json cache_hit_rate_json(const ModelUsage& usage) {
    return cache_hit_rate_json(usage.cached_tokens, usage.prompt_tokens);
}

} // namespace model_usage

enum class ModelRequestLimitSource { unknown, automatic, config, response_header };

[[nodiscard]] constexpr std::string_view
model_request_limit_source_name(ModelRequestLimitSource source) noexcept {
    switch (source) {
    case ModelRequestLimitSource::automatic:
        return "automatic";
    case ModelRequestLimitSource::config:
        return "config";
    case ModelRequestLimitSource::response_header:
        return "response_header";
    case ModelRequestLimitSource::unknown:
        return "unknown";
    }
    return "unknown";
}

struct ModelCallMetadata {
    std::string adapter{};
    std::string provider{};
    std::string response_id{};
    std::string model{};
    std::size_t attempts = 1;
    std::size_t retries = 0;
    long http_status = 0;
    long long duration_ms = 0;
    bool streamed = false;
    std::size_t stream_events = 0;
    std::size_t streamed_bytes = 0;
    // The current response header, retained for transport diagnostics.
    std::size_t request_token_limit = 0;
    // Effective request budget after applying config and learned headers.
    std::size_t max_request_tokens = 0;
    ModelRequestLimitSource max_request_tokens_source = ModelRequestLimitSource::unknown;
    std::size_t response_header_max_request_tokens = 0;
    std::size_t request_token_estimate_bytes_per_token =
        model_token_estimation::serialized_bytes_per_token;
};

struct ModelReply {
    Json assistant_message{};
    std::string text{};
    std::vector<ToolCall> tool_calls{};
    ModelUsage usage{};
    ModelCallMetadata metadata{};
};

// A zero maximum means that the provider has not advertised a request budget.
// Reserved values describe tokens unavailable to conversation messages.
struct ModelRequestLimits {
    std::size_t max_request_tokens = 0;
    std::size_t reserved_output_tokens = 0;
    std::size_t safety_margin_tokens = 0;
    std::size_t request_overhead_tokens = 0;
    ModelRequestLimitSource max_request_tokens_source = ModelRequestLimitSource::unknown;
    std::size_t response_header_max_request_tokens = 0;
    std::size_t request_token_estimate_bytes_per_token =
        model_token_estimation::serialized_bytes_per_token;

    [[nodiscard]] bool bounded() const noexcept {
        return max_request_tokens != 0;
    }

    [[nodiscard]] std::size_t available_request_tokens() const noexcept {
        if (!bounded()) {
            return 0;
        }

        auto available = max_request_tokens;
        for (const auto reserved : {reserved_output_tokens, safety_margin_tokens}) {
            if (reserved >= available) {
                return 0;
            }
            available -= reserved;
        }
        return available;
    }

    [[nodiscard]] std::size_t available_input_tokens() const noexcept {
        const auto available = available_request_tokens();
        return request_overhead_tokens >= available ? 0 : available - request_overhead_tokens;
    }

    [[nodiscard]] std::size_t estimated_tokens(std::size_t serialized_bytes) const noexcept {
        return model_token_estimation::from_serialized_bytes(
            serialized_bytes, request_token_estimate_bytes_per_token);
    }

    [[nodiscard]] std::size_t available_input_bytes() const noexcept {
        return model_token_estimation::to_serialized_bytes(available_input_tokens(),
                                                           request_token_estimate_bytes_per_token);
    }
};

} // namespace mint
