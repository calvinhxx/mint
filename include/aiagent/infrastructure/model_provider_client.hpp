#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "aiagent/domain/model.hpp"

namespace aiagent {

class TaskControl;

enum class ModelAdapter { chat_completions, responses };

[[nodiscard]] std::string_view model_adapter_name(ModelAdapter adapter) noexcept;

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
    std::string item_id;
    std::string name;
    std::string delta;
};

using ModelStreamCallback = std::function<void(const ModelStreamEvent&)>;

struct ModelProviderConfig {
    std::string api_url;
    std::string api_key;
    std::string model;
    long connect_timeout_seconds = 15;
    long request_timeout_seconds = 120;
    long max_retries = 2;
    long retry_initial_delay_ms = 250;
    long max_completion_tokens = 1024;
    std::shared_ptr<TaskControl> task_control;
    ModelProgressCallback progress;
    // Appended after the v1.3 fields so positional aggregate initialization of
    // ChatCompletionsConfig remains source compatible.
    ModelAdapter adapter = ModelAdapter::chat_completions;
    bool stream = false;
    ModelStreamCallback stream_event;
};

class ModelProviderClient final : public ModelClient {
  public:
    explicit ModelProviderClient(ModelProviderConfig config);

    ModelReply complete(const Json& messages, const Json& tools) override;

  private:
    ModelProviderConfig config_;
};

// Deterministic and network-free. It demonstrates the loop, not model quality.
class DemoModelClient final : public ModelClient {
  public:
    ModelReply complete(const Json& messages, const Json& tools) override;

  private:
    std::size_t step_ = 0;
};

} // namespace aiagent
