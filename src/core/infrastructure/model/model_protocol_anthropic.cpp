#include "model_protocol_anthropic.hpp"

#include "mint/localization/localization.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mint::detail {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

[[noreturn]] void throw_byte_limit(std::string_view resource, std::size_t limit) {
    throw std::runtime_error(
        message(Message::model_protocol_byte_limit,
                {arg(Placeholder::resource, resource), arg(Placeholder::limit, limit)}));
}

[[noreturn]] void throw_count_limit(std::string_view resource, std::size_t limit) {
    throw std::runtime_error(
        message(Message::model_protocol_count_limit,
                {arg(Placeholder::resource, resource), arg(Placeholder::limit, limit)}));
}

void add_bytes(std::size_t& current, std::size_t added, std::size_t limit,
               std::string_view resource) {
    if (current > limit || added > limit - current) {
        throw_byte_limit(resource, limit);
    }
    current += added;
}

std::string stream_error_message(const Json& value) {
    if (value.is_object() && value.contains("message") && value.at("message").is_string()) {
        return value.at("message").get<std::string>();
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return message(Message::model_anthropic_stream_error_event);
}

std::size_t output_index(const Json& event) {
    if (!event.contains("index")) {
        return 0;
    }
    const auto& value = event.at("index");
    std::uint64_t index = 0;
    if (value.is_number_unsigned()) {
        index = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_index = value.get<std::int64_t>();
        if (signed_index < 0) {
            throw std::runtime_error(message(Message::model_anthropic_output_index_invalid));
        }
        index = static_cast<std::uint64_t>(signed_index);
    } else {
        throw std::runtime_error(message(Message::model_anthropic_output_index_invalid));
    }
    if (index > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(message(Message::model_anthropic_output_index_overflow));
    }
    return static_cast<std::size_t>(index);
}

Json parse_arguments(std::string_view arguments) {
    try {
        auto parsed = Json::parse(arguments);
        if (!parsed.is_object()) {
            throw std::runtime_error(message(Message::model_anthropic_arguments_root_object));
        }
        return parsed;
    } catch (const Json::exception& error) {
        throw std::runtime_error(message(Message::model_anthropic_arguments_invalid_json,
                                         {arg(Placeholder::error, error.what())}));
    }
}

struct StreamToolCall {
    std::size_t output_index = 0;
    std::string id;
    std::string name;
    std::string arguments;
};

struct StreamBlock {
    std::size_t output_index = 0;
    Json content;
};

} // namespace

struct AnthropicStreamAccumulator::State {
    State(ModelStreamCallback event_callback, ModelResponseLimits configured_limits)
        : callback(std::move(event_callback)), limits(std::move(configured_limits)) {}

    ModelStreamCallback callback;
    ModelResponseLimits limits;
    std::optional<Json> complete_response;
    std::string id;
    std::string model;
    Json usage;
    std::vector<StreamToolCall> tools;
    std::vector<StreamBlock> blocks;
    std::size_t delta_bytes = 0;
    std::size_t text_bytes = 0;
    std::size_t reasoning_bytes = 0;
    std::size_t tool_argument_bytes = 0;
    std::size_t tool_metadata_bytes = 0;
    bool saw_payload = false;
    bool finished = false;

    void emit(ModelStreamEvent event) {
        if (event.kind == ModelStreamEventKind::text_delta) {
            add_bytes(text_bytes, event.delta.size(), limits.max_text_bytes,
                      message(Message::model_resource_text));
        } else {
            add_bytes(tool_argument_bytes, event.delta.size(), limits.max_tool_arguments_bytes,
                      message(Message::model_resource_tool_arguments));
        }
        delta_bytes += event.delta.size();
        if (callback && !event.delta.empty()) {
            callback(event);
        }
    }

    StreamToolCall& tool(std::size_t index) {
        const auto item = std::find_if(tools.begin(), tools.end(), [index](const auto& value) {
            return value.output_index == index;
        });
        if (item != tools.end()) {
            return *item;
        }
        if (tools.size() >= limits.max_tool_calls) {
            throw_count_limit(message(Message::model_resource_tool_calls), limits.max_tool_calls);
        }
        tools.emplace_back();
        tools.back().output_index = index;
        return tools.back();
    }

    StreamBlock& block(std::size_t index) {
        const auto item = std::find_if(blocks.begin(), blocks.end(), [index](const auto& value) {
            return value.output_index == index;
        });
        if (item == blocks.end()) {
            throw std::runtime_error(message(Message::model_anthropic_delta_before_block));
        }
        return *item;
    }

    void start_block(std::size_t index, Json content) {
        const auto duplicate =
            std::find_if(blocks.begin(), blocks.end(),
                         [index](const auto& value) { return value.output_index == index; });
        if (duplicate != blocks.end()) {
            throw std::runtime_error(message(Message::model_anthropic_block_index_duplicate));
        }
        if (blocks.size() >= limits.max_sse_events) {
            throw_count_limit(message(Message::model_resource_anthropic_content_blocks),
                              limits.max_sse_events);
        }
        blocks.push_back({.output_index = index, .content = std::move(content)});
    }

    void dispatch_start(const Json& event) {
        if (!event.contains("content_block") || !event.at("content_block").is_object()) {
            throw std::runtime_error(message(Message::model_anthropic_content_block_missing));
        }
        const auto index = output_index(event);
        const auto& content = event.at("content_block");
        const auto type = content.value("type", "");
        saw_payload = true;
        start_block(index, content);

        if (type == "text" && content.contains("text") && content.at("text").is_string()) {
            emit({.kind = ModelStreamEventKind::text_delta,
                  .output_index = index,
                  .delta = content.at("text").get<std::string>()});
            return;
        }
        if (type == "tool_use") {
            if (!content.contains("id") || !content.at("id").is_string() ||
                !content.contains("name") || !content.at("name").is_string()) {
                throw std::runtime_error(message(Message::model_protocol_anthropic_tool_identity));
            }
            auto& current = tool(index);
            current.id = content.at("id").get<std::string>();
            current.name = content.at("name").get<std::string>();
            add_bytes(tool_metadata_bytes, current.id.size() + current.name.size(),
                      limits.max_tool_metadata_bytes,
                      message(Message::model_resource_tool_metadata));
            return;
        }
        if (type == "thinking" || type == "redacted_thinking") {
            for (const char* field : {"thinking", "data", "signature"}) {
                if (content.contains(field) && content.at(field).is_string()) {
                    add_bytes(
                        reasoning_bytes, content.at(field).get_ref<const std::string&>().size(),
                        limits.max_reasoning_bytes, message(Message::model_resource_reasoning));
                }
            }
        }
    }

    void dispatch_delta(const Json& event) {
        if (!event.contains("delta") || !event.at("delta").is_object()) {
            throw std::runtime_error(message(Message::model_anthropic_delta_missing));
        }
        const auto index = output_index(event);
        const auto& delta = event.at("delta");
        const auto type = delta.value("type", "");
        saw_payload = true;
        auto& content = block(index).content;

        if (type == "text_delta" && delta.contains("text") && delta.at("text").is_string()) {
            const auto& text = delta.at("text").get_ref<const std::string&>();
            emit({.kind = ModelStreamEventKind::text_delta, .output_index = index, .delta = text});
            if (!content.contains("text") || !content.at("text").is_string()) {
                content["text"] = "";
            }
            content["text"].get_ref<std::string&>() += text;
        } else if (type == "input_json_delta" && delta.contains("partial_json") &&
                   delta.at("partial_json").is_string()) {
            auto& current = tool(index);
            const auto& arguments = delta.at("partial_json").get_ref<const std::string&>();
            emit({.kind = ModelStreamEventKind::tool_arguments_delta,
                  .output_index = index,
                  .item_id = current.id,
                  .name = current.name,
                  .delta = arguments});
            current.arguments += arguments;
        } else if (type == "thinking_delta" && delta.contains("thinking") &&
                   delta.at("thinking").is_string()) {
            const auto& thinking = delta.at("thinking").get_ref<const std::string&>();
            add_bytes(reasoning_bytes, thinking.size(), limits.max_reasoning_bytes,
                      message(Message::model_resource_reasoning));
            if (!content.contains("thinking") || !content.at("thinking").is_string()) {
                content["thinking"] = "";
            }
            content["thinking"].get_ref<std::string&>() += thinking;
        } else if (type == "signature_delta" && delta.contains("signature") &&
                   delta.at("signature").is_string()) {
            const auto& signature = delta.at("signature").get_ref<const std::string&>();
            add_bytes(reasoning_bytes, signature.size(), limits.max_reasoning_bytes,
                      message(Message::model_resource_reasoning_signature));
            if (!content.contains("signature") || !content.at("signature").is_string()) {
                content["signature"] = "";
            }
            content["signature"].get_ref<std::string&>() += signature;
        }
    }
};

AnthropicStreamAccumulator::AnthropicStreamAccumulator(ModelStreamCallback callback,
                                                       ModelResponseLimits limits)
    : state_(std::make_unique<State>(std::move(callback), std::move(limits))) {}

AnthropicStreamAccumulator::~AnthropicStreamAccumulator() = default;

void AnthropicStreamAccumulator::dispatch(const Json& event) {
    if (state_->finished) {
        throw std::logic_error(message(Message::model_anthropic_append_after_finish));
    }
    const auto type = event.value("type", "");
    if (type == "error") {
        throw std::runtime_error(
            stream_error_message(event.contains("error") ? event.at("error") : event));
    }
    if (type == "message" && event.contains("content") && event.at("content").is_array()) {
        state_->complete_response = event;
    } else if (type == "message_start") {
        if (!event.contains("message") || !event.at("message").is_object()) {
            throw std::runtime_error(message(Message::model_anthropic_message_start_missing));
        }
        const auto& message = event.at("message");
        state_->id = message.value("id", state_->id);
        state_->model = message.value("model", state_->model);
        if (message.contains("usage") && message.at("usage").is_object()) {
            state_->usage = message.at("usage");
        }
        state_->saw_payload = true;
    } else if (type == "content_block_start") {
        state_->dispatch_start(event);
    } else if (type == "content_block_delta") {
        state_->dispatch_delta(event);
    } else if (type == "message_delta" && event.contains("usage") &&
               event.at("usage").is_object()) {
        if (!state_->usage.is_object()) {
            state_->usage = Json::object();
        }
        state_->usage.update(event.at("usage"));
    }
}

Json AnthropicStreamAccumulator::finish() {
    if (state_->finished) {
        throw std::logic_error(message(Message::model_anthropic_already_finished));
    }
    state_->finished = true;
    if (state_->complete_response.has_value()) {
        return std::move(*state_->complete_response);
    }
    if (!state_->saw_payload) {
        throw std::runtime_error(message(Message::model_anthropic_message_event_missing));
    }

    std::sort(
        state_->blocks.begin(), state_->blocks.end(),
        [](const auto& left, const auto& right) { return left.output_index < right.output_index; });
    Json content = Json::array();
    for (auto& raw_block : state_->blocks) {
        auto block = std::move(raw_block.content);
        if (block.value("type", "") == "tool_use") {
            const auto tool = std::find_if(state_->tools.begin(), state_->tools.end(),
                                           [&raw_block](const auto& item) {
                                               return item.output_index == raw_block.output_index;
                                           });
            if (tool == state_->tools.end() || tool->id.empty() || tool->name.empty()) {
                throw std::runtime_error(message(Message::model_anthropic_stream_tool_identity));
            }
            if (!tool->arguments.empty()) {
                block["input"] = parse_arguments(tool->arguments);
            } else if (!block.contains("input") || !block.at("input").is_object()) {
                block["input"] = Json::object();
            }
        }
        content.push_back(std::move(block));
    }

    Json response = {{"type", "message"}, {"role", "assistant"}, {"content", std::move(content)}};
    if (!state_->id.empty()) {
        response["id"] = std::move(state_->id);
    }
    if (!state_->model.empty()) {
        response["model"] = std::move(state_->model);
    }
    if (state_->usage.is_object()) {
        response["usage"] = std::move(state_->usage);
    }
    return response;
}

std::size_t AnthropicStreamAccumulator::streamed_bytes() const noexcept {
    return state_->delta_bytes;
}

} // namespace mint::detail
