#include "model_protocol_internal.hpp"

#include <stdexcept>
#include <string>

namespace mint::detail::protocol {

[[noreturn]] void throw_byte_limit(std::string_view resource, std::size_t limit) {
    throw std::runtime_error("模型响应超过资源上限: " + std::string(resource) + " 最多 " +
                             std::to_string(limit) + " 字节");
}

[[noreturn]] void throw_count_limit(std::string_view resource, std::size_t limit) {
    throw std::runtime_error("模型响应超过资源上限: " + std::string(resource) + " 最多 " +
                             std::to_string(limit));
}

void add_bytes(std::size_t& current, std::size_t added, std::size_t limit,
               std::string_view resource) {
    if (current > limit || added > limit - current) {
        throw_byte_limit(resource, limit);
    }
    current += added;
}

void append_bounded(std::string& target, std::string_view value, std::size_t limit,
                    std::string_view resource) {
    if (target.size() > limit || value.size() > limit - target.size()) {
        throw_byte_limit(resource, limit);
    }
    target.append(value);
}

void validate_limits(const ModelResponseLimits& limits) {
    if (!valid_model_response_limits(limits)) {
        throw std::invalid_argument("模型响应资源上限超出允许范围");
    }
}

OutputBudget::OutputBudget(const ModelResponseLimits& configured_limits)
    : limits(configured_limits) {}

void OutputBudget::add_text(std::size_t bytes) {
    add_bytes(text_bytes, bytes, limits.max_text_bytes, "文本");
}

void OutputBudget::add_reasoning(std::size_t bytes) {
    add_bytes(reasoning_bytes, bytes, limits.max_reasoning_bytes, "推理内容");
}

void OutputBudget::add_tool(std::size_t id_bytes, std::size_t name_bytes,
                            std::size_t argument_bytes) {
    if (tool_calls >= limits.max_tool_calls) {
        throw_count_limit("工具调用数量", limits.max_tool_calls);
    }
    ++tool_calls;
    add_bytes(tool_metadata_bytes, id_bytes, limits.max_tool_metadata_bytes, "工具调用元数据");
    add_bytes(tool_metadata_bytes, name_bytes, limits.max_tool_metadata_bytes, "工具调用元数据");
    add_bytes(tool_argument_bytes, argument_bytes, limits.max_tool_arguments_bytes, "工具参数");
}

const Json* provider_state(const Json& message) {
    for (const auto field : {provider_state_field, legacy_provider_state_field}) {
        if (message.contains(field) && message.at(field).is_object()) {
            return &message.at(field);
        }
    }
    return nullptr;
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

std::string content_text(const Json& content) {
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
            result += part.get_ref<const std::string&>();
        } else if (part.is_object() && part.contains("text") && part.at("text").is_string()) {
            result += part.at("text").get_ref<const std::string&>();
        }
    }
    return result;
}

std::string extract_text(const Json& content, OutputBudget& budget) {
    if (content.is_null()) {
        return {};
    }
    if (content.is_string()) {
        const auto& text = content.get_ref<const std::string&>();
        budget.add_text(text.size());
        return text;
    }
    if (!content.is_array()) {
        auto text = content.dump();
        budget.add_text(text.size());
        return text;
    }

    std::string result;
    for (const auto& part : content) {
        if (part.is_string()) {
            const auto& text = part.get_ref<const std::string&>();
            budget.add_text(text.size());
            result += text;
        } else if (part.is_object() && part.contains("text") && part.at("text").is_string()) {
            const auto& text = part.at("text").get_ref<const std::string&>();
            budget.add_text(text.size());
            result += text;
        }
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

std::string response_status_error(const Json& response) {
    if (response.contains("error") && response.at("error").is_object() &&
        response.at("error").contains("message") &&
        response.at("error").at("message").is_string()) {
        return response.at("error").at("message").get<std::string>();
    }
    return "Responses API 返回非完成状态: " + response.value("status", "unknown");
}

} // namespace mint::detail::protocol
