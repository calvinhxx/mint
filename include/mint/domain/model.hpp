#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mint {

using Json = nlohmann::json;

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
};

struct ModelReply {
    Json assistant_message{};
    std::string text{};
    std::vector<ToolCall> tool_calls{};
    ModelUsage usage{};
    ModelCallMetadata metadata{};
};

class ModelClient {
  public:
    virtual ~ModelClient() = default;

    virtual ModelReply complete(const Json& messages, const Json& tools) = 0;
};

} // namespace mint
