#include "model_protocol.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mint::detail {
namespace {

constexpr std::string_view provider_state_field = "_mint_provider_state";
constexpr std::string_view legacy_provider_state_field = "_aiagent_provider_state";

const Json* provider_state(const Json& message) {
    for (const auto field : {provider_state_field, legacy_provider_state_field}) {
        if (message.contains(field) && message.at(field).is_object()) {
            return &message.at(field);
        }
    }
    return nullptr;
}

std::size_t token_count(const Json& object, const char* field) {
    if (!object.contains(field) || !object.at(field).is_number_integer()) {
        return 0;
    }
    const auto value = object.at(field).get<long long>();
    return value < 0 ? 0 : static_cast<std::size_t>(value);
}

std::string extract_text(const Json& content) {
    if (content.is_null()) {
        return {};
    }
    if (content.is_string()) {
        return content.get<std::string>();
    }
    if (!content.is_array()) {
        return content.dump();
    }

    std::string result;
    for (const auto& part : content) {
        if (part.is_string()) {
            result += part.get<std::string>();
        } else if (part.is_object() && part.contains("text") && part.at("text").is_string()) {
            result += part.at("text").get<std::string>();
        }
    }
    return result;
}

Json parse_arguments(const Json& value) {
    if (value.is_null()) {
        return Json::object();
    }
    if (value.is_object()) {
        return value;
    }
    if (!value.is_string()) {
        throw std::runtime_error("工具参数必须是 JSON 对象或 JSON 字符串");
    }

    try {
        auto parsed = Json::parse(value.get<std::string>());
        if (!parsed.is_object()) {
            throw std::runtime_error("工具参数的 JSON 顶层必须是对象");
        }
        return parsed;
    } catch (const Json::exception& error) {
        throw std::runtime_error("工具参数不是有效 JSON: " + std::string(error.what()));
    }
}

void parse_chat_usage(ModelReply& reply, const Json& response) {
    if (!response.contains("usage") || !response.at("usage").is_object()) {
        return;
    }
    const auto& usage = response.at("usage");
    reply.usage.available = usage.contains("prompt_tokens") ||
                            usage.contains("completion_tokens") || usage.contains("total_tokens");
    reply.usage.prompt_tokens = token_count(usage, "prompt_tokens");
    reply.usage.completion_tokens = token_count(usage, "completion_tokens");
    reply.usage.total_tokens = token_count(usage, "total_tokens");
    if (reply.usage.total_tokens == 0 && reply.usage.available) {
        reply.usage.total_tokens = reply.usage.prompt_tokens + reply.usage.completion_tokens;
    }
    if (usage.contains("prompt_tokens_details") && usage.at("prompt_tokens_details").is_object()) {
        reply.usage.cached_tokens = token_count(usage.at("prompt_tokens_details"), "cached_tokens");
    }
    reply.usage.cached_tokens = std::min(reply.usage.cached_tokens, reply.usage.prompt_tokens);
}

void parse_responses_usage(ModelReply& reply, const Json& response) {
    if (!response.contains("usage") || !response.at("usage").is_object()) {
        return;
    }
    const auto& usage = response.at("usage");
    reply.usage.available = usage.contains("input_tokens") || usage.contains("output_tokens") ||
                            usage.contains("total_tokens");
    reply.usage.prompt_tokens = token_count(usage, "input_tokens");
    reply.usage.completion_tokens = token_count(usage, "output_tokens");
    reply.usage.total_tokens = token_count(usage, "total_tokens");
    if (reply.usage.total_tokens == 0 && reply.usage.available) {
        reply.usage.total_tokens = reply.usage.prompt_tokens + reply.usage.completion_tokens;
    }
    if (usage.contains("input_tokens_details") && usage.at("input_tokens_details").is_object()) {
        reply.usage.cached_tokens = token_count(usage.at("input_tokens_details"), "cached_tokens");
    }
    reply.usage.cached_tokens = std::min(reply.usage.cached_tokens, reply.usage.prompt_tokens);
}

ToolCall parse_tool_call(std::string id, std::string name, const Json& arguments) {
    if (id.empty() || name.empty()) {
        throw std::runtime_error("工具调用缺少 id 或 function.name");
    }
    return ToolCall{std::move(id), std::move(name), parse_arguments(arguments)};
}

ModelReply parse_chat_response(const Json& response) {
    if (!response.contains("choices") || !response.at("choices").is_array() ||
        response.at("choices").empty()) {
        throw std::runtime_error("模型响应缺少 choices[0]");
    }

    const auto& choice = response.at("choices").at(0);
    if (!choice.contains("message") || !choice.at("message").is_object()) {
        throw std::runtime_error("模型响应缺少 assistant message");
    }

    ModelReply reply;
    reply.metadata.response_id = response.value("id", "");
    reply.metadata.model = response.value("model", "");
    reply.assistant_message = choice.at("message");
    reply.assistant_message["role"] = "assistant";
    if (reply.assistant_message.contains("content")) {
        reply.text = extract_text(reply.assistant_message.at("content"));
    }
    parse_chat_usage(reply, response);

    if (!reply.assistant_message.contains("tool_calls")) {
        return reply;
    }
    const auto& raw_calls = reply.assistant_message.at("tool_calls");
    if (!raw_calls.is_array()) {
        throw std::runtime_error("模型响应中的 tool_calls 不是数组");
    }
    for (const auto& raw_call : raw_calls) {
        if (!raw_call.is_object() || raw_call.value("type", "function") != "function" ||
            !raw_call.contains("function") || !raw_call.at("function").is_object()) {
            throw std::runtime_error("只支持 function 类型的工具调用");
        }
        const auto& function = raw_call.at("function");
        if (!raw_call.contains("id") || !raw_call.at("id").is_string() ||
            !function.contains("name") || !function.at("name").is_string()) {
            throw std::runtime_error("工具调用缺少 id 或 function.name");
        }
        const auto arguments =
            function.contains("arguments") ? function.at("arguments") : Json(nullptr);
        reply.tool_calls.push_back(parse_tool_call(raw_call.at("id").get<std::string>(),
                                                   function.at("name").get<std::string>(),
                                                   arguments));
    }
    return reply;
}

std::string response_status_error(const Json& response) {
    if (response.contains("error") && response.at("error").is_object() &&
        response.at("error").contains("message") &&
        response.at("error").at("message").is_string()) {
        return response.at("error").at("message").get<std::string>();
    }
    return "Responses API 返回非完成状态: " + response.value("status", "unknown");
}

ModelReply parse_responses_response(const Json& response) {
    if (response.contains("status") && response.at("status").is_string() &&
        response.at("status") != "completed") {
        throw std::runtime_error(response_status_error(response));
    }
    if (!response.contains("output") || !response.at("output").is_array()) {
        throw std::runtime_error("Responses API 响应缺少 output 数组");
    }

    ModelReply reply;
    reply.metadata.response_id = response.value("id", "");
    reply.metadata.model = response.value("model", "");
    Json canonical_calls = Json::array();

    for (const auto& item : response.at("output")) {
        if (!item.is_object() || !item.contains("type") || !item.at("type").is_string()) {
            continue;
        }
        const auto type = item.at("type").get<std::string>();
        if (type == "message" && item.contains("content") && item.at("content").is_array()) {
            for (const auto& part : item.at("content")) {
                if (!part.is_object()) {
                    continue;
                }
                if (part.value("type", "") == "output_text" && part.contains("text") &&
                    part.at("text").is_string()) {
                    reply.text += part.at("text").get<std::string>();
                } else if (part.value("type", "") == "refusal" && part.contains("refusal") &&
                           part.at("refusal").is_string()) {
                    reply.text += part.at("refusal").get<std::string>();
                }
            }
        } else if (type == "function_call") {
            if (!item.contains("call_id") || !item.at("call_id").is_string() ||
                !item.contains("name") || !item.at("name").is_string()) {
                throw std::runtime_error("Responses API function_call 缺少 call_id 或 name");
            }
            const auto arguments =
                item.contains("arguments") ? item.at("arguments") : Json(nullptr);
            auto call = parse_tool_call(item.at("call_id").get<std::string>(),
                                        item.at("name").get<std::string>(), arguments);
            canonical_calls.push_back(
                {{"id", call.id},
                 {"type", "function"},
                 {"function", {{"name", call.name}, {"arguments", call.arguments.dump()}}}});
            reply.tool_calls.push_back(std::move(call));
        }
    }

    reply.assistant_message = {
        {"role", "assistant"},
        {"content", reply.text.empty() ? Json(nullptr) : Json(reply.text)},
        {provider_state_field, {{"adapter", "responses"}, {"output", response.at("output")}}}};
    if (!canonical_calls.empty()) {
        reply.assistant_message["tool_calls"] = std::move(canonical_calls);
    }
    parse_responses_usage(reply, response);
    return reply;
}

Json sanitized_chat_messages(const Json& messages) {
    if (!messages.is_array()) {
        throw std::invalid_argument("模型消息必须是数组");
    }
    Json result = Json::array();
    for (const auto& message : messages) {
        if (!message.is_object() || !message.contains("role") || !message.at("role").is_string()) {
            throw std::invalid_argument("模型消息缺少 role");
        }
        Json clean = {{"role", message.at("role")}};
        for (const char* field : {"content", "name", "tool_call_id", "tool_calls"}) {
            if (message.contains(field)) {
                clean[field] = message.at(field);
            }
        }
        result.push_back(std::move(clean));
    }
    return result;
}

std::string tool_output_text(const Json& content) {
    return content.is_string() ? content.get<std::string>() : content.dump();
}

Json responses_input(const Json& messages) {
    if (!messages.is_array()) {
        throw std::invalid_argument("模型消息必须是数组");
    }
    Json input = Json::array();
    for (const auto& message : messages) {
        if (!message.is_object() || !message.contains("role") || !message.at("role").is_string()) {
            throw std::invalid_argument("模型消息缺少 role");
        }
        const auto role = message.at("role").get<std::string>();
        if (const auto* state = provider_state(message);
            state != nullptr && state->value("adapter", "") == "responses" &&
            state->contains("output") && state->at("output").is_array()) {
            for (const auto& item : state->at("output")) {
                input.push_back(item);
            }
            continue;
        }
        if (role == "tool") {
            if (!message.contains("tool_call_id") || !message.at("tool_call_id").is_string() ||
                !message.contains("content")) {
                throw std::invalid_argument("tool 消息缺少 tool_call_id 或 content");
            }
            input.push_back({{"type", "function_call_output"},
                             {"call_id", message.at("tool_call_id")},
                             {"output", tool_output_text(message.at("content"))}});
            continue;
        }

        if (message.contains("content") && !message.at("content").is_null()) {
            input.push_back(
                {{"type", "message"}, {"role", role}, {"content", message.at("content")}});
        }
        if (role == "assistant" && message.contains("tool_calls")) {
            if (!message.at("tool_calls").is_array()) {
                throw std::invalid_argument("assistant tool_calls 必须是数组");
            }
            for (const auto& call : message.at("tool_calls")) {
                if (!call.is_object() || !call.contains("id") || !call.at("id").is_string() ||
                    !call.contains("function") || !call.at("function").is_object() ||
                    !call.at("function").contains("name") ||
                    !call.at("function").at("name").is_string()) {
                    throw std::invalid_argument("assistant tool_call 格式无效");
                }
                const auto& function = call.at("function");
                const auto arguments = function.contains("arguments")
                                           ? tool_output_text(function.at("arguments"))
                                           : std::string("{}");
                input.push_back({{"type", "function_call"},
                                 {"call_id", call.at("id")},
                                 {"name", function.at("name")},
                                 {"arguments", arguments}});
            }
        }
    }
    return input;
}

Json responses_tools(const Json& tools) {
    if (!tools.is_array()) {
        throw std::invalid_argument("工具定义必须是数组");
    }
    Json result = Json::array();
    for (const auto& tool : tools) {
        if (!tool.is_object() || tool.value("type", "") != "function" ||
            !tool.contains("function") || !tool.at("function").is_object()) {
            throw std::invalid_argument("Responses adapter 只支持 function 工具定义");
        }
        Json flattened = tool.at("function");
        flattened["type"] = "function";
        result.push_back(std::move(flattened));
    }
    return result;
}

std::string stream_error_message(const Json& value) {
    if (value.is_object() && value.contains("message") && value.at("message").is_string()) {
        return value.at("message").get<std::string>();
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return "模型流返回错误事件";
}

struct StreamToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

} // namespace

Json build_provider_request(const ModelProviderConfig& config, const Json& messages,
                            const Json& tools) {
    const auto profile = resolve_model_provider_profile(config);
    if (!tools.empty() && !profile.capabilities.function_tools) {
        throw std::invalid_argument("当前 provider profile 不支持 function tools");
    }

    if (profile.adapter == ModelAdapter::chat_completions) {
        Json request = {{"model", config.model}, {"messages", sanitized_chat_messages(messages)}};
        request[std::string(model_token_limit_parameter_name(
            profile.capabilities.token_limit_parameter))] = config.max_completion_tokens;
        if (!tools.empty()) {
            request["tools"] = tools;
            request["tool_choice"] = "auto";
        }
        if (config.stream) {
            request["stream"] = true;
            if (profile.capabilities.stream_usage) {
                request["stream_options"] = {{"include_usage", true}};
            }
        }
        return request;
    }

    Json request = {{"model", config.model},
                    {"input", responses_input(messages)},
                    {"max_output_tokens", config.max_completion_tokens},
                    {"store", false}};
    if (profile.capabilities.stateless_reasoning_replay) {
        request["include"] = Json::array({"reasoning.encrypted_content"});
    }
    if (!tools.empty()) {
        request["tools"] = responses_tools(tools);
        request["tool_choice"] = "auto";
    }
    if (config.stream) {
        request["stream"] = true;
    }
    return request;
}

ModelReply parse_provider_response(ModelAdapter adapter, const Json& response) {
    return adapter == ModelAdapter::chat_completions ? parse_chat_response(response)
                                                     : parse_responses_response(response);
}

struct ModelStreamDecoder::State {
    State(ModelAdapter selected_adapter, ModelStreamCallback event_callback)
        : adapter(selected_adapter), callback(std::move(event_callback)) {}

    ModelAdapter adapter;
    ModelStreamCallback callback;
    std::string line_buffer;
    std::string event_data;
    std::optional<Json> complete_response;
    std::string chat_text;
    std::string chat_id;
    std::string chat_model;
    Json chat_usage;
    std::vector<StreamToolCall> chat_tools;
    std::size_t events = 0;
    std::size_t delta_bytes = 0;
    bool saw_chat_payload = false;
    bool finished = false;

    void emit(ModelStreamEvent event) {
        delta_bytes += event.delta.size();
        if (callback && !event.delta.empty()) {
            callback(event);
        }
    }

    void dispatch_chat(const Json& event) {
        if (event.contains("error")) {
            throw std::runtime_error(stream_error_message(event.at("error")));
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
            if (delta.contains("content") && !delta.at("content").is_null()) {
                const auto text = extract_text(delta.at("content"));
                chat_text += text;
                emit({.kind = ModelStreamEventKind::text_delta, .delta = text});
            }
            if (!delta.contains("tool_calls")) {
                continue;
            }
            if (!delta.at("tool_calls").is_array()) {
                throw std::runtime_error("流式 tool_calls 不是数组");
            }
            for (const auto& raw_call : delta.at("tool_calls")) {
                if (!raw_call.is_object()) {
                    throw std::runtime_error("流式 tool_call 格式无效");
                }
                const auto index_value = raw_call.value("index", 0LL);
                if (index_value < 0) {
                    throw std::runtime_error("流式 tool_call index 无效");
                }
                const auto index = static_cast<std::size_t>(index_value);
                if (chat_tools.size() <= index) {
                    chat_tools.resize(index + 1);
                }
                auto& tool = chat_tools.at(index);
                if (raw_call.contains("id") && raw_call.at("id").is_string()) {
                    tool.id = raw_call.at("id").get<std::string>();
                }
                std::string argument_delta;
                if (raw_call.contains("function") && raw_call.at("function").is_object()) {
                    const auto& function = raw_call.at("function");
                    if (function.contains("name") && function.at("name").is_string()) {
                        tool.name += function.at("name").get<std::string>();
                    }
                    if (function.contains("arguments") && function.at("arguments").is_string()) {
                        argument_delta = function.at("arguments").get<std::string>();
                        tool.arguments += argument_delta;
                    }
                }
                emit({.kind = ModelStreamEventKind::tool_arguments_delta,
                      .output_index = index,
                      .item_id = tool.id,
                      .name = tool.name,
                      .delta = std::move(argument_delta)});
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
                                         ? response_status_error(event.at("response"))
                                         : "Responses API 流式响应未完成");
        } else if (type == "error") {
            throw std::runtime_error(stream_error_message(event));
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
        } else {
            dispatch_responses(event);
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
            event_data.push_back('\n');
        }
        event_data.append(value);
    }
};

ModelStreamDecoder::ModelStreamDecoder(ModelAdapter adapter, ModelStreamCallback callback)
    : state_(std::make_unique<State>(adapter, std::move(callback))) {}

ModelStreamDecoder::~ModelStreamDecoder() = default;

void ModelStreamDecoder::feed(std::string_view chunk) {
    if (state_->finished) {
        throw std::logic_error("不能向已结束的模型流追加数据");
    }
    state_->line_buffer.append(chunk);
    while (true) {
        const auto newline = state_->line_buffer.find('\n');
        if (newline == std::string::npos) {
            break;
        }
        const auto line = state_->line_buffer.substr(0, newline);
        state_->line_buffer.erase(0, newline + 1);
        state_->line(line);
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
    if (!state_->saw_chat_payload && state_->chat_tools.empty()) {
        throw std::runtime_error("Chat Completions 流没有返回 choices delta");
    }

    Json message = {{"role", "assistant"},
                    {"content", state_->chat_text.empty() && !state_->chat_tools.empty()
                                    ? Json(nullptr)
                                    : Json(state_->chat_text)}};
    if (!state_->chat_tools.empty()) {
        message["tool_calls"] = Json::array();
        for (const auto& tool : state_->chat_tools) {
            message["tool_calls"].push_back(
                {{"id", tool.id},
                 {"type", "function"},
                 {"function", {{"name", tool.name}, {"arguments", tool.arguments}}}});
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
    return state_->delta_bytes;
}

} // namespace mint::detail
