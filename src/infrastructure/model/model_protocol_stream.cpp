#include "model_protocol.hpp"
#include "model_protocol_anthropic.hpp"
#include "model_protocol_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mint::detail {
namespace {

struct StreamToolCall {
    std::string id;
    std::string name;
    std::string arguments;
    Json extensions = Json::object();
    Json function_extensions = Json::object();
};

std::size_t stream_output_index(const Json& event) {
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
            throw std::runtime_error("流式输出 index 无效");
        }
        index = static_cast<std::uint64_t>(signed_index);
    } else {
        throw std::runtime_error("流式输出 index 无效");
    }
    if (index > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("流式输出 index 超出平台范围");
    }
    return static_cast<std::size_t>(index);
}

std::size_t stream_tool_index(const Json& raw_call, const ModelResponseLimits& limits) {
    const auto index = stream_output_index(raw_call);
    if (index >= limits.max_tool_calls) {
        protocol::throw_count_limit("工具调用 index", limits.max_tool_calls - 1);
    }
    return index;
}

} // namespace

struct ModelStreamDecoder::State {
    State(ModelAdapter selected_adapter, ModelStreamCallback event_callback,
          ModelResponseLimits configured_limits)
        : adapter(selected_adapter), callback(std::move(event_callback)),
          limits(std::move(configured_limits)) {
        if (adapter == ModelAdapter::anthropic_messages) {
            anthropic_stream = std::make_unique<AnthropicStreamAccumulator>(callback, limits);
        }
    }

    ModelAdapter adapter;
    ModelStreamCallback callback;
    ModelResponseLimits limits;
    std::string line_buffer;
    std::string event_data;
    std::optional<Json> complete_response;
    std::string chat_text;
    std::string chat_reasoning_content;
    std::string chat_id;
    std::string chat_model;
    Json chat_usage;
    std::vector<StreamToolCall> chat_tools;
    std::unique_ptr<AnthropicStreamAccumulator> anthropic_stream;
    std::size_t events = 0;
    std::size_t sse_frames = 0;
    std::size_t delta_bytes = 0;
    std::size_t text_bytes = 0;
    std::size_t tool_argument_bytes = 0;
    std::size_t tool_metadata_bytes = 0;
    bool saw_chat_payload = false;
    bool saw_chat_reasoning_content = false;
    bool finished = false;

    void emit(ModelStreamEvent event) {
        if (event.kind == ModelStreamEventKind::text_delta) {
            protocol::add_bytes(text_bytes, event.delta.size(), limits.max_text_bytes, "文本");
        } else {
            protocol::add_bytes(tool_argument_bytes, event.delta.size(),
                                limits.max_tool_arguments_bytes, "工具参数");
        }
        delta_bytes += event.delta.size();
        if (callback && !event.delta.empty()) {
            callback(event);
        }
    }

    std::size_t resolve_chat_tool_index(const Json& raw_call, std::size_t fallback) const {
        if (raw_call.contains("index")) {
            return stream_tool_index(raw_call, limits);
        }
        if (raw_call.contains("id") && raw_call.at("id").is_string()) {
            const auto& id = raw_call.at("id").get_ref<const std::string&>();
            const auto existing = std::find_if(chat_tools.begin(), chat_tools.end(),
                                               [&id](const auto& tool) { return tool.id == id; });
            if (existing != chat_tools.end()) {
                return static_cast<std::size_t>(std::distance(chat_tools.begin(), existing));
            }
        }
        if (fallback >= limits.max_tool_calls) {
            protocol::throw_count_limit("工具调用 index", limits.max_tool_calls - 1);
        }
        return fallback;
    }

    void retain_chat_extension(Json& target, std::string_view key, const Json& value) {
        const auto before = target.dump().size();
        target[std::string(key)] = value;
        const auto after = target.dump().size();
        if (after > before) {
            protocol::add_bytes(tool_metadata_bytes, after - before, limits.max_tool_metadata_bytes,
                                "工具调用扩展元数据");
        }
    }

    void dispatch_chat(const Json& event) {
        if (event.contains("error")) {
            throw std::runtime_error(protocol::stream_error_message(event.at("error")));
        }
        if (event.contains("choices") && event.at("choices").is_array() &&
            !event.at("choices").empty() && event.at("choices").at(0).contains("message")) {
            complete_response = event;
            return;
        }
        chat_id = event.value("id", chat_id);
        chat_model = event.value("model", chat_model);
        if (event.contains("usage") && event.at("usage").is_object()) {
            chat_usage = event.at("usage");
        }
        if (!event.contains("choices") || !event.at("choices").is_array()) {
            return;
        }
        for (const auto& choice : event.at("choices")) {
            if (!choice.is_object() || !choice.contains("delta") ||
                !choice.at("delta").is_object()) {
                continue;
            }
            saw_chat_payload = true;
            const auto& delta = choice.at("delta");
            if (delta.contains("reasoning_content") && delta.at("reasoning_content").is_string()) {
                saw_chat_reasoning_content = true;
                protocol::append_bounded(
                    chat_reasoning_content,
                    delta.at("reasoning_content").get_ref<const std::string&>(),
                    limits.max_reasoning_bytes, "推理内容");
            }
            if (delta.contains("content") && !delta.at("content").is_null()) {
                protocol::OutputBudget event_budget(limits);
                const auto text = protocol::extract_text(delta.at("content"), event_budget);
                emit({.kind = ModelStreamEventKind::text_delta, .delta = text});
                chat_text += text;
            }
            if (!delta.contains("tool_calls")) {
                continue;
            }
            if (!delta.at("tool_calls").is_array()) {
                throw std::runtime_error("流式 tool_calls 不是数组");
            }
            std::size_t fallback_index = 0;
            for (const auto& raw_call : delta.at("tool_calls")) {
                if (!raw_call.is_object()) {
                    throw std::runtime_error("流式 tool_call 格式无效");
                }
                const auto index = resolve_chat_tool_index(raw_call, fallback_index++);
                if (chat_tools.size() <= index) {
                    chat_tools.resize(index + 1);
                }
                auto& tool = chat_tools.at(index);
                for (const auto& [key, value] : raw_call.items()) {
                    if (key != "index" && key != "id" && key != "type" && key != "function") {
                        retain_chat_extension(tool.extensions, key, value);
                    }
                }
                if (raw_call.contains("id") && raw_call.at("id").is_string()) {
                    const auto& id = raw_call.at("id").get_ref<const std::string&>();
                    if (id.size() > tool.id.size()) {
                        protocol::add_bytes(tool_metadata_bytes, id.size() - tool.id.size(),
                                            limits.max_tool_metadata_bytes, "工具调用元数据");
                    }
                    tool.id = id;
                }
                std::string argument_delta;
                if (raw_call.contains("function") && raw_call.at("function").is_object()) {
                    const auto& function = raw_call.at("function");
                    for (const auto& [key, value] : function.items()) {
                        if (key != "name" && key != "arguments") {
                            retain_chat_extension(tool.function_extensions, key, value);
                        }
                    }
                    if (function.contains("name") && function.at("name").is_string()) {
                        const auto& name = function.at("name").get_ref<const std::string&>();
                        protocol::add_bytes(tool_metadata_bytes, name.size(),
                                            limits.max_tool_metadata_bytes, "工具调用元数据");
                        tool.name += name;
                    }
                    if (function.contains("arguments") && function.at("arguments").is_string()) {
                        argument_delta = function.at("arguments").get<std::string>();
                    }
                }
                emit({.kind = ModelStreamEventKind::tool_arguments_delta,
                      .output_index = index,
                      .item_id = tool.id,
                      .name = tool.name,
                      .delta = argument_delta});
                tool.arguments += argument_delta;
            }
        }
    }

    void dispatch_responses(const Json& event) {
        const auto type = event.value("type", "");
        if (type == "response.output_text.delta") {
            emit({.kind = ModelStreamEventKind::text_delta,
                  .output_index = event.value("output_index", std::size_t{0}),
                  .item_id = event.value("item_id", ""),
                  .delta = event.value("delta", "")});
        } else if (type == "response.function_call_arguments.delta") {
            emit({.kind = ModelStreamEventKind::tool_arguments_delta,
                  .output_index = event.value("output_index", std::size_t{0}),
                  .item_id = event.value("item_id", ""),
                  .name = event.value("name", ""),
                  .delta = event.value("delta", "")});
        } else if (type == "response.completed") {
            if (!event.contains("response") || !event.at("response").is_object()) {
                throw std::runtime_error("response.completed 缺少 response 对象");
            }
            complete_response = event.at("response");
        } else if (type == "response.failed" || type == "response.incomplete") {
            throw std::runtime_error(event.contains("response")
                                         ? protocol::response_status_error(event.at("response"))
                                         : "Responses API 流式响应未完成");
        } else if (type == "error") {
            throw std::runtime_error(protocol::stream_error_message(event));
        } else if (type.empty() && event.value("object", "") == "response") {
            complete_response = event;
        }
    }

    void dispatch() {
        if (event_data.empty()) {
            return;
        }
        auto data = std::move(event_data);
        event_data.clear();
        if (sse_frames >= limits.max_sse_events) {
            protocol::throw_count_limit("SSE 事件数量", limits.max_sse_events);
        }
        ++sse_frames;
        if (data == "[DONE]") {
            return;
        }
        Json event;
        try {
            event = Json::parse(data);
        } catch (const Json::exception& error) {
            throw std::runtime_error("模型 SSE data 不是有效 JSON: " + std::string(error.what()));
        }
        ++events;
        if (adapter == ModelAdapter::chat_completions) {
            dispatch_chat(event);
        } else if (adapter == ModelAdapter::responses) {
            dispatch_responses(event);
        } else {
            anthropic_stream->dispatch(event);
        }
    }

    void line(std::string_view value) {
        if (!value.empty() && value.back() == '\r') {
            value.remove_suffix(1);
        }
        if (value.empty()) {
            dispatch();
            return;
        }
        if (!value.starts_with("data:")) {
            return;
        }
        value.remove_prefix(5);
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
        if (!event_data.empty()) {
            protocol::append_bounded(event_data, "\n", limits.max_sse_event_bytes, "SSE 事件");
        }
        protocol::append_bounded(event_data, value, limits.max_sse_event_bytes, "SSE 事件");
    }
};

ModelStreamDecoder::ModelStreamDecoder(ModelAdapter adapter, ModelStreamCallback callback,
                                       ModelResponseLimits limits)
    : state_(std::make_unique<State>(adapter, std::move(callback), std::move(limits))) {
    protocol::validate_limits(state_->limits);
}

ModelStreamDecoder::~ModelStreamDecoder() = default;

void ModelStreamDecoder::feed(std::string_view chunk) {
    if (state_->finished) {
        throw std::logic_error("不能向已结束的模型流追加数据");
    }
    while (!chunk.empty()) {
        const auto newline = chunk.find('\n');
        const auto fragment = chunk.substr(0, newline);
        protocol::append_bounded(state_->line_buffer, fragment, state_->limits.max_sse_line_bytes,
                                 "SSE 行");
        if (newline == std::string_view::npos) {
            break;
        }
        state_->line(state_->line_buffer);
        state_->line_buffer.clear();
        chunk.remove_prefix(newline + 1);
    }
}

Json ModelStreamDecoder::finish() {
    if (state_->finished) {
        throw std::logic_error("模型流已经结束");
    }
    state_->finished = true;
    if (!state_->line_buffer.empty()) {
        state_->line(state_->line_buffer);
        state_->line_buffer.clear();
    }
    state_->dispatch();
    if (state_->complete_response.has_value()) {
        return std::move(*state_->complete_response);
    }
    if (state_->adapter == ModelAdapter::responses) {
        throw std::runtime_error("Responses API 流在结束前没有 response.completed 事件");
    }
    if (state_->adapter == ModelAdapter::anthropic_messages) {
        return state_->anthropic_stream->finish();
    }
    if (!state_->saw_chat_payload && state_->chat_tools.empty()) {
        throw std::runtime_error("Chat Completions 流没有返回 choices delta");
    }

    Json message = {{"role", "assistant"},
                    {"content", state_->chat_text.empty() && !state_->chat_tools.empty()
                                    ? Json(nullptr)
                                    : Json(state_->chat_text)}};
    if (state_->saw_chat_reasoning_content) {
        message["reasoning_content"] = std::move(state_->chat_reasoning_content);
    }
    if (!state_->chat_tools.empty()) {
        message["tool_calls"] = Json::array();
        for (const auto& tool : state_->chat_tools) {
            Json function = {{"name", tool.name}, {"arguments", tool.arguments}};
            for (const auto& [key, value] : tool.function_extensions.items()) {
                function[key] = value;
            }
            Json call = {{"id", tool.id}, {"type", "function"}, {"function", std::move(function)}};
            for (const auto& [key, value] : tool.extensions.items()) {
                call[key] = value;
            }
            message["tool_calls"].push_back(std::move(call));
        }
    }
    Json response = {{"choices", Json::array({{{"message", std::move(message)}}})}};
    if (!state_->chat_id.empty()) {
        response["id"] = state_->chat_id;
    }
    if (!state_->chat_model.empty()) {
        response["model"] = state_->chat_model;
    }
    if (state_->chat_usage.is_object()) {
        response["usage"] = state_->chat_usage;
    }
    return response;
}

std::size_t ModelStreamDecoder::event_count() const noexcept {
    return state_->events;
}

std::size_t ModelStreamDecoder::streamed_bytes() const noexcept {
    return state_->adapter == ModelAdapter::anthropic_messages
               ? state_->anthropic_stream->streamed_bytes()
               : state_->delta_bytes;
}

} // namespace mint::detail
