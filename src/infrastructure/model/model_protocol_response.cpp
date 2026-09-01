#include "model_protocol_internal.hpp"

#include "mint/localization/localization.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace mint::detail::protocol {
namespace {

using localization::Message;
using localization::message;
using localization::Placeholder;

std::size_t token_count(const Json& object, const char* field) {
    if (!object.contains(field) || !object.at(field).is_number_integer()) {
        return 0;
    }
    const auto value = object.at(field).get<long long>();
    return value < 0 ? 0 : static_cast<std::size_t>(value);
}

void parse_chat_usage(ModelReply& reply, const Json& response) {
    if (!response.contains("usage") || !response.at("usage").is_object()) {
        return;
    }
    const auto& usage = response.at("usage");
    reply.usage.available = usage.contains("prompt_tokens") ||
                            usage.contains("completion_tokens") || usage.contains("total_tokens") ||
                            usage.contains("prompt_cache_hit_tokens") ||
                            usage.contains("prompt_cache_miss_tokens");
    reply.usage.prompt_tokens = token_count(usage, "prompt_tokens");
    reply.usage.completion_tokens = token_count(usage, "completion_tokens");
    reply.usage.total_tokens = token_count(usage, "total_tokens");
    if (usage.contains("prompt_tokens_details") && usage.at("prompt_tokens_details").is_object()) {
        reply.usage.cached_tokens = token_count(usage.at("prompt_tokens_details"), "cached_tokens");
    }
    if (reply.usage.cached_tokens == 0) {
        reply.usage.cached_tokens = token_count(usage, "cached_tokens");
    }
    if (reply.usage.cached_tokens == 0) {
        reply.usage.cached_tokens = token_count(usage, "prompt_cache_hit_tokens");
    }
    if (reply.usage.prompt_tokens == 0 &&
        (usage.contains("prompt_cache_hit_tokens") || usage.contains("prompt_cache_miss_tokens"))) {
        reply.usage.prompt_tokens =
            reply.usage.cached_tokens + token_count(usage, "prompt_cache_miss_tokens");
    }
    if (reply.usage.total_tokens == 0 && reply.usage.available) {
        reply.usage.total_tokens = reply.usage.prompt_tokens + reply.usage.completion_tokens;
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

void parse_anthropic_usage(ModelReply& reply, const Json& response) {
    if (!response.contains("usage") || !response.at("usage").is_object()) {
        return;
    }
    const auto& usage = response.at("usage");
    const auto uncached = token_count(usage, "input_tokens");
    const auto cache_creation = token_count(usage, "cache_creation_input_tokens");
    const auto cache_read = token_count(usage, "cache_read_input_tokens");
    reply.usage.available = usage.contains("input_tokens") || usage.contains("output_tokens") ||
                            usage.contains("cache_creation_input_tokens") ||
                            usage.contains("cache_read_input_tokens");
    reply.usage.prompt_tokens = uncached + cache_creation + cache_read;
    reply.usage.completion_tokens = token_count(usage, "output_tokens");
    reply.usage.total_tokens = reply.usage.prompt_tokens + reply.usage.completion_tokens;
    reply.usage.cached_tokens = std::min(cache_read, reply.usage.prompt_tokens);
}

std::size_t argument_bytes(const Json& arguments) {
    return arguments.is_string() ? arguments.get_ref<const std::string&>().size()
                                 : arguments.dump().size();
}

ToolCall parse_tool_call(std::string id, std::string name, const Json& arguments,
                         OutputBudget& budget) {
    if (id.empty() || name.empty()) {
        throw std::runtime_error(message(Message::model_protocol_tool_call_identity));
    }
    budget.add_tool(id.size(), name.size(), argument_bytes(arguments));
    return ToolCall{std::move(id), std::move(name), parse_arguments(arguments)};
}

} // namespace

ModelReply parse_chat_response(const Json& response, const ModelResponseLimits& limits) {
    if (!response.contains("choices") || !response.at("choices").is_array() ||
        response.at("choices").empty()) {
        throw std::runtime_error(message(Message::model_protocol_chat_choice_missing));
    }

    const auto& choice = response.at("choices").at(0);
    if (!choice.contains("message") || !choice.at("message").is_object()) {
        throw std::runtime_error(message(Message::model_protocol_assistant_message_missing));
    }

    ModelReply reply;
    OutputBudget budget(limits);
    reply.metadata.response_id = response.value("id", "");
    reply.metadata.model = response.value("model", "");
    reply.assistant_message = choice.at("message");
    reply.assistant_message["role"] = "assistant";
    if (reply.assistant_message.contains("content")) {
        reply.text = extract_text(reply.assistant_message.at("content"), budget);
    }
    if (reply.assistant_message.contains("reasoning_content") &&
        reply.assistant_message.at("reasoning_content").is_string()) {
        budget.add_reasoning(
            reply.assistant_message.at("reasoning_content").get_ref<const std::string&>().size());
    }
    parse_chat_usage(reply, response);

    if (!reply.assistant_message.contains("tool_calls")) {
        return reply;
    }
    const auto& raw_calls = reply.assistant_message.at("tool_calls");
    if (!raw_calls.is_array()) {
        throw std::runtime_error(message(Message::model_protocol_response_tool_calls_array));
    }
    for (const auto& raw_call : raw_calls) {
        if (!raw_call.is_object() || raw_call.value("type", "function") != "function" ||
            !raw_call.contains("function") || !raw_call.at("function").is_object()) {
            throw std::runtime_error(message(Message::model_protocol_function_tool_calls_only));
        }
        const auto& function = raw_call.at("function");
        if (!raw_call.contains("id") || !raw_call.at("id").is_string() ||
            !function.contains("name") || !function.at("name").is_string()) {
            throw std::runtime_error(message(Message::model_protocol_tool_call_identity));
        }
        const auto arguments =
            function.contains("arguments") ? function.at("arguments") : Json(nullptr);
        reply.tool_calls.push_back(parse_tool_call(raw_call.at("id").get<std::string>(),
                                                   function.at("name").get<std::string>(),
                                                   arguments, budget));
    }
    return reply;
}

ModelReply parse_responses_response(const Json& response, const ModelResponseLimits& limits) {
    if (response.contains("status") && response.at("status").is_string() &&
        response.at("status") != "completed") {
        throw std::runtime_error(response_status_error(response));
    }
    if (!response.contains("output") || !response.at("output").is_array()) {
        throw std::runtime_error(message(Message::model_protocol_responses_output_missing));
    }

    ModelReply reply;
    OutputBudget budget(limits);
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
                    const auto& text = part.at("text").get_ref<const std::string&>();
                    budget.add_text(text.size());
                    reply.text += text;
                } else if (part.value("type", "") == "refusal" && part.contains("refusal") &&
                           part.at("refusal").is_string()) {
                    const auto& refusal = part.at("refusal").get_ref<const std::string&>();
                    budget.add_text(refusal.size());
                    reply.text += refusal;
                }
            }
        } else if (type == "reasoning") {
            budget.add_reasoning(item.dump().size());
        } else if (type == "function_call") {
            if (!item.contains("call_id") || !item.at("call_id").is_string() ||
                !item.contains("name") || !item.at("name").is_string()) {
                throw std::runtime_error(message(Message::model_protocol_responses_call_identity));
            }
            const auto arguments =
                item.contains("arguments") ? item.at("arguments") : Json(nullptr);
            auto call = parse_tool_call(item.at("call_id").get<std::string>(),
                                        item.at("name").get<std::string>(), arguments, budget);
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

ModelReply parse_anthropic_response(const Json& response, const ModelResponseLimits& limits) {
    if (response.value("type", "") == "error" || response.contains("error")) {
        const auto& error = response.contains("error") ? response.at("error") : response;
        throw std::runtime_error(stream_error_message(error));
    }
    if (!response.contains("content") || !response.at("content").is_array()) {
        throw std::runtime_error(message(Message::model_protocol_anthropic_content_missing));
    }

    ModelReply reply;
    OutputBudget budget(limits);
    reply.metadata.response_id = response.value("id", "");
    reply.metadata.model = response.value("model", "");
    Json canonical_calls = Json::array();

    for (const auto& block : response.at("content")) {
        if (!block.is_object()) {
            continue;
        }
        const auto type = block.value("type", "");
        if (type == "text" && block.contains("text") && block.at("text").is_string()) {
            const auto& text = block.at("text").get_ref<const std::string&>();
            budget.add_text(text.size());
            reply.text += text;
        } else if (type == "thinking" || type == "redacted_thinking") {
            budget.add_reasoning(block.dump().size());
        } else if (type == "tool_use") {
            if (!block.contains("id") || !block.at("id").is_string() || !block.contains("name") ||
                !block.at("name").is_string()) {
                throw std::runtime_error(message(Message::model_protocol_anthropic_tool_identity));
            }
            const auto input = block.contains("input") ? block.at("input") : Json::object();
            auto call = parse_tool_call(block.at("id").get<std::string>(),
                                        block.at("name").get<std::string>(), input, budget);
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
        {provider_state_field,
         {{"adapter", "anthropic_messages"}, {"content", response.at("content")}}}};
    if (!canonical_calls.empty()) {
        reply.assistant_message["tool_calls"] = std::move(canonical_calls);
    }
    parse_anthropic_usage(reply, response);
    return reply;
}

} // namespace mint::detail::protocol
