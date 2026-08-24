#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "aiagent/domain/model.hpp"

namespace aiagent {

class TaskControl;

enum class ModelProgressKind {
    attempt_started,
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
};

using ModelProgressCallback = std::function<void(const ModelProgress&)>;

[[nodiscard]] std::string_view model_progress_kind_name(ModelProgressKind kind) noexcept;
[[nodiscard]] Json model_progress_to_json(const ModelProgress& progress);

struct ChatCompletionsConfig {
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
};

class ChatCompletionsClient final : public ModelClient {
  public:
    explicit ChatCompletionsClient(ChatCompletionsConfig config);

    ModelReply complete(const Json& messages, const Json& tools) override;

  private:
    ChatCompletionsConfig config_;
};

// Deterministic and network-free. It demonstrates the loop, not model quality.
class DemoModelClient final : public ModelClient {
  public:
    ModelReply complete(const Json& messages, const Json& tools) override;

  private:
    std::size_t step_ = 0;
};

} // namespace aiagent
