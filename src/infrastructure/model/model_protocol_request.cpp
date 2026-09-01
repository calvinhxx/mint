#include "model_protocol_internal.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace mint::detail::protocol {
namespace {

Json sanitized_chat_messages(const Json& messages, const ModelProviderCapabilities& capabilities) {
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
        const auto role = message.at("role").get<std::string>();
        if (capabilities.chat_reasoning_replay && role == "assistant" &&
            message.contains("reasoning_content")) {
            const auto& reasoning = message.at("reasoning_content");
            if (!reasoning.is_string() && !reasoning.is_null()) {
                throw std::invalid_argument("assistant reasoning_content 必须是字符串或 null");
            }
            clean["reasoning_content"] = reasoning;
        }
        if (capabilities.requires_tool_call_content && role == "assistant" &&
            clean.contains("tool_calls") && clean.at("tool_calls").is_array() &&
            !clean.at("tool_calls").empty() &&
            (!clean.contains("content") || clean.at("content").is_null())) {
            clean["content"] = "";
        }
        result.push_back(std::move(clean));
    }
    return result;
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
                             {"output", content_text(message.at("content"))}});
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
                                           ? content_text(function.at("arguments"))
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

void append_anthropic_message(Json& messages, std::string_view role, Json blocks) {
    if (blocks.empty()) {
        return;
    }
    if (!messages.empty() && messages.back().value("role", "") == role) {
        auto& content = messages.back()["content"];
        for (auto& block : blocks) {
            content.push_back(std::move(block));
        }
        return;
    }
    messages.push_back({{"role", role}, {"content", std::move(blocks)}});
}

struct AnthropicConversation {
    std::string system;
    Json messages = Json::array();
};

AnthropicConversation anthropic_conversation(const Json& messages) {
    if (!messages.is_array()) {
        throw std::invalid_argument("模型消息必须是数组");
    }

    AnthropicConversation result;
    for (const auto& message : messages) {
        if (!message.is_object() || !message.contains("role") || !message.at("role").is_string()) {
            throw std::invalid_argument("模型消息缺少 role");
        }
        const auto role = message.at("role").get<std::string>();
        if (role == "system") {
            if (!message.contains("content")) {
                continue;
            }
            const auto text = content_text(message.at("content"));
            if (!text.empty()) {
                if (!result.system.empty()) {
                    result.system += "\n\n";
                }
                result.system += text;
            }
            continue;
        }

        Json blocks = Json::array();
        if (role == "tool") {
            if (!message.contains("tool_call_id") || !message.at("tool_call_id").is_string() ||
                !message.contains("content")) {
                throw std::invalid_argument("tool 消息缺少 tool_call_id 或 content");
            }
            blocks.push_back({{"type", "tool_result"},
                              {"tool_use_id", message.at("tool_call_id")},
                              {"content", content_text(message.at("content"))}});
            append_anthropic_message(result.messages, "user", std::move(blocks));
            continue;
        }
        if (role != "user" && role != "assistant") {
            throw std::invalid_argument("Anthropic Messages 只接受 system、user、assistant、tool");
        }

        if (role == "assistant") {
            if (const auto* state = provider_state(message);
                state != nullptr && state->value("adapter", "") == "anthropic_messages" &&
                state->contains("content") && state->at("content").is_array()) {
                append_anthropic_message(result.messages, "assistant", state->at("content"));
                continue;
            }
        }

        if (message.contains("content") && !message.at("content").is_null()) {
            const auto text = content_text(message.at("content"));
            if (!text.empty()) {
                blocks.push_back({{"type", "text"}, {"text", text}});
            }
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
                const auto input = function.contains("arguments")
                                       ? parse_arguments(function.at("arguments"))
                                       : Json::object();
                blocks.push_back({{"type", "tool_use"},
                                  {"id", call.at("id")},
                                  {"name", function.at("name")},
                                  {"input", input}});
            }
        }
        append_anthropic_message(result.messages, role, std::move(blocks));
    }
    return result;
}

Json anthropic_tools(const Json& tools) {
    if (!tools.is_array()) {
        throw std::invalid_argument("工具定义必须是数组");
    }
    Json result = Json::array();
    for (const auto& tool : tools) {
        if (!tool.is_object() || tool.value("type", "") != "function" ||
            !tool.contains("function") || !tool.at("function").is_object()) {
            throw std::invalid_argument("Anthropic Messages adapter 只支持 function 工具定义");
        }
        const auto& function = tool.at("function");
        if (!function.contains("name") || !function.at("name").is_string()) {
            throw std::invalid_argument("Anthropic 工具定义缺少 function.name");
        }
        Json converted = {
            {"name", function.at("name")},
            {"input_schema", function.contains("parameters")
                                 ? function.at("parameters")
                                 : Json({{"type", "object"}, {"properties", Json::object()}})}};
        if (function.contains("description") && function.at("description").is_string()) {
            converted["description"] = function.at("description");
        }
        result.push_back(std::move(converted));
    }
    return result;
}

} // namespace

Json build_chat_request(const ModelProviderConfig& config, const Json& messages, const Json& tools,
                        const ModelProviderCapabilities& capabilities) {
    Json request = {{"model", config.model},
                    {"messages", sanitized_chat_messages(messages, capabilities)}};
    request[std::string(model_token_limit_parameter_name(capabilities.token_limit_parameter))] =
        config.max_completion_tokens;
    if (!tools.empty()) {
        request["tools"] = tools;
        if (capabilities.explicit_tool_choice) {
            request["tool_choice"] = "auto";
        }
    }
    if (config.stream) {
        request["stream"] = true;
        if (capabilities.stream_usage) {
            request["stream_options"] = {{"include_usage", true}};
        }
    }
    return request;
}

Json build_responses_request(const ModelProviderConfig& config, const Json& messages,
                             const Json& tools, const ModelProviderCapabilities& capabilities) {
    Json request = {{"model", config.model},
                    {"input", responses_input(messages)},
                    {"max_output_tokens", config.max_completion_tokens},
                    {"store", false}};
    if (capabilities.stateless_reasoning_replay) {
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

Json build_anthropic_request(const ModelProviderConfig& config, const Json& messages,
                             const Json& tools, const ModelProviderCapabilities& capabilities) {
    auto conversation = anthropic_conversation(messages);
    Json request = {{"model", config.model},
                    {"max_tokens", config.max_completion_tokens},
                    {"messages", std::move(conversation.messages)}};
    if (!conversation.system.empty()) {
        request["system"] = std::move(conversation.system);
    }
    if (!tools.empty()) {
        request["tools"] = anthropic_tools(tools);
        if (capabilities.explicit_tool_choice) {
            request["tool_choice"] = {{"type", "auto"}};
        }
    }
    if (config.stream) {
        request["stream"] = true;
    }
    return request;
}

} // namespace mint::detail::protocol
