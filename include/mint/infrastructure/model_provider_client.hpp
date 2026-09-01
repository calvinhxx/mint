#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "mint/ports/model_client.hpp"

namespace mint {

class TaskControl;

enum class ModelAdapter { chat_completions, responses, anthropic_messages };

enum class ModelProvider {
    automatic,
    custom,
    openai,
    anthropic,
    google,
    xai,
    moonshot,
    groq,
    deepseek
};

enum class ModelTokenLimitParameter { max_completion_tokens, max_tokens, max_output_tokens };

enum class ModelProviderSource { explicit_config, endpoint, compatibility_default };

struct ModelProviderCapabilities {
    bool function_tools = true;
    bool streaming = true;
    bool stream_usage = true;
    bool stateless_reasoning_replay = false;
    ModelTokenLimitParameter token_limit_parameter =
        ModelTokenLimitParameter::max_completion_tokens;
    bool explicit_tool_choice = true;
    bool chat_reasoning_replay = false;
    bool requires_tool_call_content = false;
};

struct ModelProviderProfile {
    ModelProvider provider = ModelProvider::custom;
    ModelAdapter adapter = ModelAdapter::chat_completions;
    ModelProviderSource source = ModelProviderSource::compatibility_default;
    ModelProviderCapabilities capabilities{};
};

[[nodiscard]] std::string_view model_adapter_name(ModelAdapter adapter) noexcept;
[[nodiscard]] std::string_view model_provider_name(ModelProvider provider) noexcept;
[[nodiscard]] std::string_view model_provider_source_name(ModelProviderSource source) noexcept;
[[nodiscard]] std::string_view
model_token_limit_parameter_name(ModelTokenLimitParameter parameter) noexcept;

namespace model_provider_defaults {

inline constexpr long connect_timeout_seconds = 15;
inline constexpr long request_timeout_seconds = 120;
inline constexpr long max_retries = 2;
inline constexpr long retry_initial_delay_ms = 250;
inline constexpr long max_completion_tokens = 1024;
inline constexpr long max_request_tokens = 0;
inline constexpr long request_token_safety_margin = 256;
inline constexpr long request_token_estimate_bytes_per_token =
    static_cast<long>(model_token_estimation::serialized_bytes_per_token);
// Used until a provider advertises its own token-rate ceiling. Keeping the
// first request modest makes unknown OpenAI-compatible endpoints predictable.
inline constexpr std::size_t automatic_max_request_tokens = 8'000;

} // namespace model_provider_defaults

namespace model_provider_bounds {

inline constexpr long max_retries = 10;
inline constexpr long max_retry_initial_delay_ms = 60'000;
inline constexpr long max_completion_tokens = 65'536;
inline constexpr long min_request_token_estimate_bytes_per_token = 1;
inline constexpr long max_request_token_estimate_bytes_per_token = 8;

} // namespace model_provider_bounds

namespace model_response_defaults {

inline constexpr std::size_t max_http_body_bytes = 16U * 1024U * 1024U;
inline constexpr std::size_t max_sse_line_bytes = 4U * 1024U * 1024U;
inline constexpr std::size_t max_sse_event_bytes = 8U * 1024U * 1024U;
inline constexpr std::size_t max_sse_events = 100'000;
inline constexpr std::size_t max_text_bytes = 8U * 1024U * 1024U;
inline constexpr std::size_t max_reasoning_bytes = 8U * 1024U * 1024U;
inline constexpr std::size_t max_tool_arguments_bytes = 8U * 1024U * 1024U;
inline constexpr std::size_t max_tool_metadata_bytes = 1U * 1024U * 1024U;
inline constexpr std::size_t max_tool_calls = 128;

} // namespace model_response_defaults

namespace model_response_bounds {

inline constexpr std::size_t max_bytes = 256U * 1024U * 1024U;
inline constexpr std::size_t max_sse_events = 1'000'000;
inline constexpr std::size_t max_tool_calls = 4'096;

} // namespace model_response_bounds

struct ModelResponseLimits {
    std::size_t max_http_body_bytes = model_response_defaults::max_http_body_bytes;
    std::size_t max_sse_line_bytes = model_response_defaults::max_sse_line_bytes;
    std::size_t max_sse_event_bytes = model_response_defaults::max_sse_event_bytes;
    std::size_t max_sse_events = model_response_defaults::max_sse_events;
    std::size_t max_text_bytes = model_response_defaults::max_text_bytes;
    std::size_t max_reasoning_bytes = model_response_defaults::max_reasoning_bytes;
    std::size_t max_tool_arguments_bytes = model_response_defaults::max_tool_arguments_bytes;
    std::size_t max_tool_metadata_bytes = model_response_defaults::max_tool_metadata_bytes;
    std::size_t max_tool_calls = model_response_defaults::max_tool_calls;
};

[[nodiscard]] constexpr bool
valid_model_response_limits(const ModelResponseLimits& limits) noexcept {
    const auto valid_bytes = [](std::size_t value) {
        return value > 0 && value <= model_response_bounds::max_bytes;
    };
    return valid_bytes(limits.max_http_body_bytes) && valid_bytes(limits.max_sse_line_bytes) &&
           valid_bytes(limits.max_sse_event_bytes) && valid_bytes(limits.max_text_bytes) &&
           valid_bytes(limits.max_reasoning_bytes) &&
           valid_bytes(limits.max_tool_arguments_bytes) &&
           valid_bytes(limits.max_tool_metadata_bytes) && limits.max_sse_events > 0 &&
           limits.max_sse_events <= model_response_bounds::max_sse_events &&
           limits.max_tool_calls > 0 &&
           limits.max_tool_calls <= model_response_bounds::max_tool_calls;
}

enum class ModelProgressKind {
    attempt_started,
    stream_started,
    stream_completed,
    retry_scheduled,
    request_succeeded,
    request_failed
};

struct ModelProgress {
    ModelProgressKind kind = ModelProgressKind::attempt_started;
    std::size_t attempt = 1;
    std::size_t max_attempts = 1;
    long http_status = 0;
    long delay_ms = 0;
    long long elapsed_ms = 0;
    std::size_t stream_events = 0;
    std::size_t streamed_bytes = 0;
};

using ModelProgressCallback = std::function<void(const ModelProgress&)>;

[[nodiscard]] std::string_view model_progress_kind_name(ModelProgressKind kind) noexcept;
[[nodiscard]] Json model_progress_to_json(const ModelProgress& progress);

enum class ModelStreamEventKind { text_delta, tool_arguments_delta };

struct ModelStreamEvent {
    ModelStreamEventKind kind = ModelStreamEventKind::text_delta;
    std::size_t output_index = 0;
    std::string item_id{};
    std::string name{};
    std::string delta{};
};

using ModelStreamCallback = std::function<void(const ModelStreamEvent&)>;

struct ModelProviderConfig {
    std::string api_url{};
    std::string api_key{};
    std::string model{};
    long connect_timeout_seconds = model_provider_defaults::connect_timeout_seconds;
    long request_timeout_seconds = model_provider_defaults::request_timeout_seconds;
    long max_retries = model_provider_defaults::max_retries;
    long retry_initial_delay_ms = model_provider_defaults::retry_initial_delay_ms;
    long max_completion_tokens = model_provider_defaults::max_completion_tokens;
    std::shared_ptr<TaskControl> task_control{};
    ModelProgressCallback progress{};
    // Appended after the v1.3 fields so positional aggregate initialization of
    // ChatCompletionsConfig remains source compatible.
    ModelAdapter adapter = ModelAdapter::chat_completions;
    bool stream = false;
    ModelStreamCallback stream_event{};
    ModelProvider provider = ModelProvider::automatic;
    std::string api_key_env{};
    std::optional<ModelProviderCapabilities> capabilities{};
    long max_request_tokens = model_provider_defaults::max_request_tokens;
    long request_token_safety_margin = model_provider_defaults::request_token_safety_margin;
    long request_token_estimate_bytes_per_token =
        model_provider_defaults::request_token_estimate_bytes_per_token;
    // Appended to preserve positional aggregate initialization of earlier
    // ModelProviderConfig and ChatCompletionsConfig call sites.
    ModelResponseLimits response_limits{};
};

[[nodiscard]] ModelProviderProfile
resolve_model_provider_profile(const ModelProviderConfig& config);
[[nodiscard]] Json model_provider_profile_to_json(const ModelProviderProfile& profile);

class ModelProviderClient final : public ModelClient {
  public:
    explicit ModelProviderClient(ModelProviderConfig config);

    ModelRequestLimits request_limits(const Json& tools) const override;
    ModelReply complete(const Json& messages, const Json& tools) override;

  private:
    ModelProviderConfig config_;
    std::size_t learned_max_request_tokens_ = 0;
};

// Deterministic and network-free. It demonstrates the loop, not model quality.
class DemoModelClient final : public ModelClient {
  public:
    ModelReply complete(const Json& messages, const Json& tools) override;

  private:
    std::size_t step_ = 0;
};

} // namespace mint
