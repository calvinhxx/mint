#include "agent_context.hpp"

#include "mint/localization/localization.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace mint::agent_detail {
namespace {

using localization::Message;
using localization::message;
using localization::Placeholder;

constexpr std::array<std::size_t, 3> payload_limits = {2048, 512, 128};
constexpr std::size_t summary_reserve = 512;
constexpr std::size_t compacted_text_limit = 256;

constexpr std::array<std::string_view, 16> retained_tool_fields = {"ok",
                                                                   "status",
                                                                   "exit_code",
                                                                   "signal",
                                                                   "timed_out",
                                                                   "task_timed_out",
                                                                   "cancelled",
                                                                   "resource_limited",
                                                                   "resource_limit",
                                                                   "verification_eligible",
                                                                   "workspace_changed",
                                                                   "recipe",
                                                                   "program",
                                                                   "path",
                                                                   "operation_count",
                                                                   "output_truncated"};

Json compacted_tool_result(const Json& message);

bool requires_exact_chat_continuation(const Json& message) {
    if (!message.is_object() || message.value("role", "") != "assistant" ||
        !message.contains("tool_calls") || !message.at("tool_calls").is_array() ||
        message.at("tool_calls").empty()) {
        return false;
    }
    if (message.contains("reasoning_content") && !message.at("reasoning_content").is_null()) {
        return true;
    }
    for (const auto& call : message.at("tool_calls")) {
        if (!call.is_object()) {
            continue;
        }
        for (const auto& [key, value] : call.items()) {
            (void)value;
            if (key != "id" && key != "type" && key != "function") {
                return true;
            }
        }
        if (!call.contains("function") || !call.at("function").is_object()) {
            continue;
        }
        for (const auto& [key, value] : call.at("function").items()) {
            (void)value;
            if (key != "name" && key != "arguments") {
                return true;
            }
        }
    }
    return false;
}

void truncate_utf8(std::string& value, std::size_t limit) {
    if (value.size() <= limit) {
        return;
    }
    auto boundary = limit;
    while (boundary > 0 && (static_cast<unsigned char>(value[boundary]) & 0xC0U) == 0x80U) {
        --boundary;
    }
    const auto removed = value.size() - boundary;
    value.resize(boundary);
    value += "...[context compacted, " + std::to_string(removed) + " bytes omitted]";
}

void compact_json_strings(Json& value, std::size_t string_limit) {
    if (value.is_string()) {
        auto text = value.get<std::string>();
        truncate_utf8(text, string_limit);
        value = std::move(text);
        return;
    }
    if (value.is_array()) {
        for (auto& item : value) {
            compact_json_strings(item, string_limit);
        }
        return;
    }
    if (value.is_object()) {
        for (auto& [key, item] : value.items()) {
            (void)key;
            compact_json_strings(item, string_limit);
        }
    }
}

void compact_message_payload(Json& message, std::size_t string_limit) {
    if (!message.is_object()) {
        return;
    }
    if (requires_exact_chat_continuation(message)) {
        return;
    }
    // EN: Only the newest assistant/tool group reaches payload compaction. Provider state in this
    //     group may contain signed thinking or encrypted reasoning required for the next tool
    //     turn, so canonical fields may shrink but provider state must remain byte-for-byte intact.
    // ZH-CN: 只有最新的 assistant/tool 分组会进入载荷压缩。该分组的供应商状态可能包含下一轮
    //        工具调用所需的签名思考或加密推理，因此可压缩规范字段，但供应商状态必须逐字节
    //        保持不变。
    if (message.contains("content") && message.at("content").is_string()) {
        auto content = message.at("content").get<std::string>();
        bool structured_tool_result = false;
        if (message.value("role", "") == "tool") {
            try {
                auto parsed = Json::parse(content);
                compact_json_strings(parsed, string_limit);
                content = parsed.dump();
                structured_tool_result = true;
            } catch (const Json::exception&) {
                // EN: A bounded raw fallback is still valid context.
                // ZH-CN: 受长度限制的原始回退内容仍然是有效上下文。
            }
        }
        if (structured_tool_result && content.size() > string_limit * 4) {
            content = compacted_tool_result(message).dump();
        } else {
            truncate_utf8(content, string_limit * 4);
        }
        message["content"] = std::move(content);
    }
    if (message.value("role", "") != "assistant" || !message.contains("tool_calls") ||
        !message.at("tool_calls").is_array()) {
        return;
    }
    for (auto& call : message["tool_calls"]) {
        if (!call.is_object() || !call.contains("function") || !call["function"].is_object() ||
            !call["function"].contains("arguments") || !call["function"]["arguments"].is_string()) {
            continue;
        }
        auto arguments = call["function"]["arguments"].get<std::string>();
        try {
            auto parsed = Json::parse(arguments);
            compact_json_strings(parsed, std::max<std::size_t>(128, string_limit / 2));
            arguments = parsed.dump();
        } catch (const Json::exception&) {
        }
        truncate_utf8(arguments, string_limit * 2);
        call["function"]["arguments"] = std::move(arguments);
    }
}

std::size_t serialized_size(const Json& value) {
    return value.dump().size();
}

void retain_scalar_field(const Json& source, Json& target, std::string_view field) {
    const auto found = source.find(std::string(field));
    if (found == source.end() || !found->is_primitive()) {
        return;
    }
    target[std::string(field)] = *found;
}

Json compacted_tool_result(const Json& message) {
    Json summary = {{"ok", nullptr},
                    {"context_compacted", true},
                    {"detail", "Tool payload omitted; retained status fields are original."}};

    if (!message.contains("content") || !message.at("content").is_string()) {
        return summary;
    }

    try {
        const auto original = Json::parse(message.at("content").get<std::string>());
        if (!original.is_object()) {
            summary["detail"] = "Tool payload omitted; original result was not a JSON object.";
            return summary;
        }
        for (const auto field : retained_tool_fields) {
            retain_scalar_field(original, summary, field);
        }
        if (const auto limits = original.find("resource_limits");
            limits != original.end() && limits->is_object() && limits->dump().size() <= 512) {
            summary["resource_limits"] = *limits;
        }
        if (const auto error = original.find("error");
            error != original.end() && error->is_string()) {
            auto error_text = error->get<std::string>();
            truncate_utf8(error_text, compacted_text_limit);
            summary["error"] = std::move(error_text);
        }
    } catch (const Json::exception&) {
        summary["detail"] = "Tool payload omitted; original result was not valid JSON.";
    }
    return summary;
}

void compact_assistant_tool_call(Json& message) {
    message["content"] = "[Historical tool call payload compacted by harness]";
    for (auto& call : message["tool_calls"]) {
        if (call.is_object() && call.contains("function") && call["function"].is_object()) {
            call["function"]["arguments"] = "{}";
        }
    }
}

void compact_message_fallback(Json& message) {
    if (!message.is_object()) {
        return;
    }
    if (requires_exact_chat_continuation(message)) {
        return;
    }
    const auto role = message.value("role", "");
    if (role == "tool") {
        message["content"] = compacted_tool_result(message).dump();
        return;
    }
    if (role == "assistant" && message.contains("tool_calls") &&
        message.at("tool_calls").is_array()) {
        compact_assistant_tool_call(message);
        return;
    }
    if (message.contains("content") && message.at("content").is_string()) {
        auto content = message.at("content").get<std::string>();
        truncate_utf8(content, compacted_text_limit);
        message["content"] = std::move(content);
    }
}

Json shrink_group(Json group, std::size_t available) {
    for (const auto limit : payload_limits) {
        for (auto& message : group) {
            compact_message_payload(message, limit);
        }
        if (serialized_size(group) <= available) {
            return group;
        }
    }

    for (auto& message : group) {
        compact_message_fallback(message);
    }
    return group;
}

std::vector<Json> message_groups(const Json& messages) {
    std::vector<Json> groups;
    for (std::size_t index = 2; index < messages.size(); ++index) {
        const auto starts_group =
            messages.at(index).is_object() && messages.at(index).value("role", "") == "assistant";
        if (groups.empty() || starts_group) {
            groups.push_back(Json::array());
        }
        groups.back().push_back(messages.at(index));
    }
    return groups;
}

Json context_summary(std::size_t dropped_groups) {
    return {{"role", "system"},
            {"content",
             "[Harness context summary] " + std::to_string(dropped_groups) +
                 " older assistant/tool groups were omitted to enforce the context byte budget. "
                 "The complete history remains in the local checkpoint. Continue from the retained "
                 "recent evidence."}};
}

} // namespace

CompactedContext compact_context(const Json& messages, std::size_t byte_limit) {
    CompactedContext result;
    result.full_bytes = serialized_size(messages);
    if (result.full_bytes <= byte_limit) {
        result.messages = messages;
        result.sent_bytes = result.full_bytes;
        return result;
    }
    if (!messages.is_array() || messages.size() < 2) {
        throw std::runtime_error(message(Message::agent_context_invalid));
    }

    Json prefix = Json::array({messages.at(0), messages.at(1)});
    const auto prefix_bytes = serialized_size(prefix);
    if (prefix_bytes + summary_reserve >= byte_limit) {
        throw std::runtime_error(message(Message::agent_context_prefix_too_large));
    }

    const auto groups = message_groups(messages);

    auto remaining = byte_limit - prefix_bytes - summary_reserve;
    std::vector<Json> selected_reverse;
    for (std::size_t offset = 0; offset < groups.size(); ++offset) {
        const auto index = groups.size() - 1 - offset;
        auto group = groups.at(index);
        auto bytes = serialized_size(group);
        if (offset == 0 && bytes > remaining) {
            group = shrink_group(std::move(group), remaining);
            bytes = serialized_size(group);
            result.payloads_compacted = true;
            if (bytes > remaining) {
                throw std::runtime_error(
                    message(Message::agent_context_continuation_compaction_failed));
            }
        }
        if (bytes <= remaining) {
            selected_reverse.push_back(std::move(group));
            remaining -= bytes;
        } else {
            ++result.dropped_groups;
        }
    }

    result.messages = std::move(prefix);
    result.messages.push_back(context_summary(result.dropped_groups));
    for (auto iterator = selected_reverse.rbegin(); iterator != selected_reverse.rend();
         ++iterator) {
        for (auto& message : *iterator) {
            result.messages.push_back(std::move(message));
        }
    }
    result.sent_bytes = serialized_size(result.messages);
    if (result.sent_bytes > byte_limit) {
        throw std::runtime_error(message(Message::agent_context_tool_pair_compaction_failed));
    }
    return result;
}

} // namespace mint::agent_detail
