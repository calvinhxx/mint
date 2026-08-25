#include "agent_support.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace aiagent::agent_detail {
namespace {

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
    // Provider-specific continuation data is intentionally discarded when a historical
    // message is compacted. The canonical content and tool_calls fields remain sufficient.
    message.erase("_aiagent_provider_state");
    if (message.contains("content") && message.at("content").is_string()) {
        auto content = message.at("content").get<std::string>();
        if (message.value("role", "") == "tool") {
            try {
                auto parsed = Json::parse(content);
                compact_json_strings(parsed, string_limit);
                content = parsed.dump();
            } catch (const Json::exception&) {
                // A bounded raw fallback is still valid context.
            }
        }
        truncate_utf8(content, string_limit * 4);
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

Json shrink_group(Json group, std::size_t available) {
    for (const auto limit : {std::size_t{2048}, std::size_t{512}, std::size_t{128}}) {
        for (auto& message : group) {
            compact_message_payload(message, limit);
        }
        if (serialized_size(group) <= available) {
            return group;
        }
    }

    for (auto& message : group) {
        if (!message.is_object()) {
            continue;
        }
        const auto role = message.value("role", "");
        if (role == "tool") {
            message["content"] =
                R"({"ok":true,"context_compacted":true,"detail":"historical tool payload omitted"})";
        } else if (role == "assistant" && message.contains("tool_calls") &&
                   message.at("tool_calls").is_array()) {
            message["content"] = "[Historical tool call payload compacted by harness]";
            for (auto& call : message["tool_calls"]) {
                if (call.is_object() && call.contains("function") && call["function"].is_object()) {
                    call["function"]["arguments"] = "{}";
                }
            }
        } else if (message.contains("content") && message.at("content").is_string()) {
            auto content = message.at("content").get<std::string>();
            truncate_utf8(content, 256);
            message["content"] = std::move(content);
        }
    }
    return group;
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
        throw std::runtime_error("模型上下文格式无效");
    }

    Json prefix = Json::array({messages.at(0), messages.at(1)});
    const auto prefix_bytes = serialized_size(prefix);
    constexpr std::size_t summary_reserve = 512;
    if (prefix_bytes + summary_reserve >= byte_limit) {
        throw std::runtime_error("系统提示与用户任务超过 --max-context-bytes 限制");
    }

    std::vector<Json> groups;
    for (std::size_t index = 2; index < messages.size(); ++index) {
        const auto starts_group =
            messages.at(index).is_object() && messages.at(index).value("role", "") == "assistant";
        if (groups.empty() || starts_group) {
            groups.push_back(Json::array());
        }
        groups.back().push_back(messages.at(index));
    }

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
        }
        if (bytes <= remaining) {
            selected_reverse.push_back(std::move(group));
            remaining -= bytes;
        } else {
            ++result.dropped_groups;
        }
    }

    result.messages = std::move(prefix);
    result.messages.push_back(
        {{"role", "system"},
         {"content",
          "[Harness context summary] " + std::to_string(result.dropped_groups) +
              " older assistant/tool groups were omitted to enforce the context byte budget. "
              "The complete history remains in the local checkpoint. Continue from the retained "
              "recent evidence."}});
    for (auto iterator = selected_reverse.rbegin(); iterator != selected_reverse.rend();
         ++iterator) {
        for (auto& message : *iterator) {
            result.messages.push_back(std::move(message));
        }
    }
    result.sent_bytes = serialized_size(result.messages);
    if (result.sent_bytes > byte_limit) {
        throw std::runtime_error("无法在不破坏最新工具调用配对的情况下压缩模型上下文");
    }
    return result;
}

} // namespace aiagent::agent_detail
