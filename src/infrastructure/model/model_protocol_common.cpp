#include "model_protocol_internal.hpp"

#include "mint/localization/localization.hpp"

#include <stdexcept>
#include <string>

namespace mint::detail::protocol {

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

void append_bounded(std::string& target, std::string_view value, std::size_t limit,
                    std::string_view resource) {
    if (target.size() > limit || value.size() > limit - target.size()) {
        throw_byte_limit(resource, limit);
    }
    target.append(value);
}

void validate_limits(const ModelResponseLimits& limits) {
    if (!valid_model_response_limits(limits)) {
        throw std::invalid_argument(message(Message::model_protocol_limits_invalid));
    }
}

OutputBudget::OutputBudget(const ModelResponseLimits& configured_limits)
    : limits(configured_limits) {}

void OutputBudget::add_text(std::size_t bytes) {
    add_bytes(text_bytes, bytes, limits.max_text_bytes, message(Message::model_resource_text));
}

void OutputBudget::add_reasoning(std::size_t bytes) {
    add_bytes(reasoning_bytes, bytes, limits.max_reasoning_bytes,
              message(Message::model_resource_reasoning));
}

void OutputBudget::add_tool(std::size_t id_bytes, std::size_t name_bytes,
                            std::size_t argument_bytes) {
    if (tool_calls >= limits.max_tool_calls) {
        throw_count_limit(message(Message::model_resource_tool_calls), limits.max_tool_calls);
    }
    ++tool_calls;
    add_bytes(tool_metadata_bytes, id_bytes, limits.max_tool_metadata_bytes,
              message(Message::model_resource_tool_metadata));
    add_bytes(tool_metadata_bytes, name_bytes, limits.max_tool_metadata_bytes,
              message(Message::model_resource_tool_metadata));
    add_bytes(tool_argument_bytes, argument_bytes, limits.max_tool_arguments_bytes,
              message(Message::model_resource_tool_arguments));
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
        throw std::runtime_error(message(Message::model_protocol_arguments_object_or_string));
    }

    try {
        auto parsed = Json::parse(value.get<std::string>());
        if (!parsed.is_object()) {
            throw std::runtime_error(message(Message::model_protocol_arguments_root_object));
        }
        return parsed;
    } catch (const Json::exception& error) {
        throw std::runtime_error(message(Message::model_protocol_arguments_invalid_json,
                                         {arg(Placeholder::error, error.what())}));
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
    return message(Message::model_protocol_stream_error_event);
}

std::string response_status_error(const Json& response) {
    if (response.contains("error") && response.at("error").is_object() &&
        response.at("error").contains("message") &&
        response.at("error").at("message").is_string()) {
        return response.at("error").at("message").get<std::string>();
    }
    return message(Message::model_protocol_responses_incomplete,
                   {arg(Placeholder::status, response.value("status", "unknown"))});
}

} // namespace mint::detail::protocol
