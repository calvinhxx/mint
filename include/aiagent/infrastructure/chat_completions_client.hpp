#pragma once

#include <memory>
#include <string>

#include "aiagent/domain/model.hpp"

namespace aiagent {

class TaskControl;

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
