#include "mint/infrastructure/config.hpp"

#include "mint/localization/localization.hpp"

#include "model_provider_profile.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mint {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

std::string required_string(const Json& document, const char* field,
                            const std::filesystem::path& config_path) {
    if (!document.contains(field) || !document.at(field).is_string() ||
        document.at(field).get_ref<const std::string&>().empty()) {
        throw std::runtime_error(message(
            Message::model_config_nonempty_string,
            {arg(Placeholder::path, config_path.string()), arg(Placeholder::field, field)}));
    }
    return document.at(field).get<std::string>();
}

std::string optional_string(const Json& document, const char* field,
                            const std::filesystem::path& config_path) {
    if (!document.contains(field)) {
        return {};
    }
    if (!document.at(field).is_string()) {
        throw std::runtime_error(
            message(Message::model_config_string, {arg(Placeholder::path, config_path.string()),
                                                   arg(Placeholder::field, field)}));
    }
    return document.at(field).get<std::string>();
}

long optional_integer(const Json& document, const char* field, long fallback, long minimum,
                      long maximum, const std::filesystem::path& config_path) {
    if (!document.contains(field)) {
        return fallback;
    }
    const auto& value = document.at(field);
    if (!value.is_number_integer()) {
        throw std::runtime_error(
            message(Message::model_config_integer, {arg(Placeholder::path, config_path.string()),
                                                    arg(Placeholder::field, field)}));
    }
    const auto parsed = value.get<long long>();
    if (parsed < minimum || parsed > maximum) {
        throw std::runtime_error(
            message(Message::model_config_range,
                    {arg(Placeholder::path, config_path.string()), arg(Placeholder::field, field),
                     arg(Placeholder::minimum, minimum), arg(Placeholder::maximum, maximum)}));
    }
    return static_cast<long>(parsed);
}

std::size_t optional_size(const Json& document, const char* field, std::size_t fallback,
                          std::size_t minimum, std::size_t maximum,
                          const std::filesystem::path& config_path) {
    if (!document.contains(field)) {
        return fallback;
    }
    const auto& value = document.at(field);
    std::uint64_t parsed = 0;
    if (value.is_number_unsigned()) {
        parsed = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            throw std::runtime_error(message(
                Message::model_config_range,
                {arg(Placeholder::path, config_path.string()), arg(Placeholder::field, field),
                 arg(Placeholder::minimum, minimum), arg(Placeholder::maximum, maximum)}));
        }
        parsed = static_cast<std::uint64_t>(signed_value);
    } else {
        throw std::runtime_error(
            message(Message::model_config_integer, {arg(Placeholder::path, config_path.string()),
                                                    arg(Placeholder::field, field)}));
    }
    if (parsed < minimum || parsed > maximum) {
        throw std::runtime_error(
            message(Message::model_config_range,
                    {arg(Placeholder::path, config_path.string()), arg(Placeholder::field, field),
                     arg(Placeholder::minimum, minimum), arg(Placeholder::maximum, maximum)}));
    }
    return static_cast<std::size_t>(parsed);
}

bool optional_boolean(const Json& document, const char* field, bool fallback,
                      const std::filesystem::path& config_path) {
    if (!document.contains(field)) {
        return fallback;
    }
    if (!document.at(field).is_boolean()) {
        throw std::runtime_error(
            message(Message::model_config_boolean, {arg(Placeholder::path, config_path.string()),
                                                    arg(Placeholder::field, field)}));
    }
    return document.at(field).get<bool>();
}

ModelAdapter optional_adapter(const Json& document, const std::filesystem::path& config_path) {
    if (!document.contains("adapter")) {
        if (document.contains("provider") && document.at("provider").is_string()) {
            const auto& provider = document.at("provider").get_ref<const std::string&>();
            if (provider == "anthropic" || provider == "claude") {
                return ModelAdapter::anthropic_messages;
            }
            if (provider == "codex") {
                return ModelAdapter::responses;
            }
        }
        return ModelAdapter::chat_completions;
    }
    const auto adapter = required_string(document, "adapter", config_path);
    if (const auto parsed = model_detail::parse_model_adapter(adapter)) {
        return *parsed;
    }
    throw std::runtime_error(message(Message::model_config_adapter_invalid,
                                     {arg(Placeholder::path, config_path.string())}));
}

ModelProvider optional_provider(const Json& document, const std::filesystem::path& config_path) {
    if (!document.contains("provider")) {
        return ModelProvider::automatic;
    }
    const auto provider = required_string(document, "provider", config_path);
    if (const auto parsed = model_detail::parse_model_provider(provider)) {
        return *parsed;
    }
    throw std::runtime_error(message(Message::model_config_provider_invalid,
                                     {arg(Placeholder::path, config_path.string())}));
}

std::string optional_endpoint(const Json& document, const std::filesystem::path& config_path) {
    if (document.contains("endpoint") && document.contains("api_url")) {
        throw std::runtime_error(message(Message::model_config_endpoint_exclusive,
                                         {arg(Placeholder::path, config_path.string())}));
    }
    return document.contains("endpoint") ? optional_string(document, "endpoint", config_path)
                                         : optional_string(document, "api_url", config_path);
}

ModelTokenLimitParameter token_limit_parameter(const Json& capabilities, ModelAdapter adapter,
                                               const std::filesystem::path& config_path) {
    const auto fallback = adapter == ModelAdapter::responses            ? "max_output_tokens"
                          : adapter == ModelAdapter::anthropic_messages ? "max_tokens"
                                                                        : "max_completion_tokens";
    const auto parameter = capabilities.contains("token_limit_parameter")
                               ? required_string(capabilities, "token_limit_parameter", config_path)
                               : std::string(fallback);
    if (const auto parsed = model_detail::parse_model_token_limit_parameter(parameter)) {
        return *parsed;
    }
    throw std::runtime_error(message(Message::model_config_token_limit_parameter_invalid,
                                     {arg(Placeholder::path, config_path.string())}));
}

std::optional<ModelProviderCapabilities>
optional_capabilities(const Json& document, ModelAdapter adapter,
                      const std::filesystem::path& config_path) {
    if (!document.contains("capabilities")) {
        return std::nullopt;
    }
    const auto& capabilities = document.at("capabilities");
    if (!capabilities.is_object()) {
        throw std::runtime_error(
            message(Message::model_config_object, {arg(Placeholder::path, config_path.string()),
                                                   arg(Placeholder::field, "capabilities")}));
    }
    constexpr std::string_view allowed[] = {"function_tools",        "streaming",
                                            "stream_usage",          "stateless_reasoning_replay",
                                            "token_limit_parameter", "explicit_tool_choice",
                                            "chat_reasoning_replay", "requires_tool_call_content"};
    for (auto item = capabilities.begin(); item != capabilities.end(); ++item) {
        if (std::find(allowed, std::end(allowed), item.key()) == std::end(allowed)) {
            throw std::runtime_error(message(Message::model_config_unknown_capability,
                                             {arg(Placeholder::path, config_path.string()),
                                              arg(Placeholder::field, item.key())}));
        }
    }

    ModelProviderCapabilities result;
    if (adapter == ModelAdapter::responses) {
        result.stream_usage = false;
        result.stateless_reasoning_replay = false;
        result.token_limit_parameter = ModelTokenLimitParameter::max_output_tokens;
    } else if (adapter == ModelAdapter::anthropic_messages) {
        result.token_limit_parameter = ModelTokenLimitParameter::max_tokens;
    }
    result.function_tools =
        optional_boolean(capabilities, "function_tools", result.function_tools, config_path);
    result.streaming = optional_boolean(capabilities, "streaming", result.streaming, config_path);
    result.stream_usage =
        optional_boolean(capabilities, "stream_usage", result.stream_usage, config_path);
    result.stateless_reasoning_replay = optional_boolean(
        capabilities, "stateless_reasoning_replay", result.stateless_reasoning_replay, config_path);
    result.token_limit_parameter = token_limit_parameter(capabilities, adapter, config_path);
    result.explicit_tool_choice = optional_boolean(capabilities, "explicit_tool_choice",
                                                   result.explicit_tool_choice, config_path);
    result.chat_reasoning_replay = optional_boolean(capabilities, "chat_reasoning_replay",
                                                    result.chat_reasoning_replay, config_path);
    result.requires_tool_call_content = optional_boolean(
        capabilities, "requires_tool_call_content", result.requires_tool_call_content, config_path);
    return result;
}

ModelResponseLimits optional_response_limits(const Json& document,
                                             const std::filesystem::path& config_path) {
    ModelResponseLimits result;
    if (!document.contains("response_limits")) {
        return result;
    }
    const auto& limits = document.at("response_limits");
    if (!limits.is_object()) {
        throw std::runtime_error(
            message(Message::model_config_object, {arg(Placeholder::path, config_path.string()),
                                                   arg(Placeholder::field, "response_limits")}));
    }
    constexpr std::string_view allowed[] = {
        "max_http_body_bytes",      "max_sse_line_bytes",      "max_sse_event_bytes",
        "max_sse_events",           "max_text_bytes",          "max_reasoning_bytes",
        "max_tool_arguments_bytes", "max_tool_metadata_bytes", "max_tool_calls"};
    for (auto item = limits.begin(); item != limits.end(); ++item) {
        if (std::find(allowed, std::end(allowed), item.key()) == std::end(allowed)) {
            throw std::runtime_error(message(Message::model_config_unknown_response_limit,
                                             {arg(Placeholder::path, config_path.string()),
                                              arg(Placeholder::field, item.key())}));
        }
    }

    const auto bytes = [&](const char* field, std::size_t fallback) {
        return optional_size(limits, field, fallback, 1, model_response_bounds::max_bytes,
                             config_path);
    };
    result.max_http_body_bytes = bytes("max_http_body_bytes", result.max_http_body_bytes);
    result.max_sse_line_bytes = bytes("max_sse_line_bytes", result.max_sse_line_bytes);
    result.max_sse_event_bytes = bytes("max_sse_event_bytes", result.max_sse_event_bytes);
    result.max_sse_events = optional_size(limits, "max_sse_events", result.max_sse_events, 1,
                                          model_response_bounds::max_sse_events, config_path);
    result.max_text_bytes = bytes("max_text_bytes", result.max_text_bytes);
    result.max_reasoning_bytes = bytes("max_reasoning_bytes", result.max_reasoning_bytes);
    result.max_tool_arguments_bytes =
        bytes("max_tool_arguments_bytes", result.max_tool_arguments_bytes);
    result.max_tool_metadata_bytes =
        bytes("max_tool_metadata_bytes", result.max_tool_metadata_bytes);
    result.max_tool_calls = optional_size(limits, "max_tool_calls", result.max_tool_calls, 1,
                                          model_response_bounds::max_tool_calls, config_path);
    return result;
}

} // namespace

ModelProviderConfig load_model_provider_config(const std::filesystem::path& config_path) {
    std::ifstream input(config_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(message(Message::model_config_not_found,
                                         {arg(Placeholder::path, config_path.string())}));
    }

    Json document;
    try {
        input >> document;
    } catch (const Json::exception& error) {
        throw std::runtime_error(message(
            Message::model_config_invalid_json,
            {arg(Placeholder::path, config_path.string()), arg(Placeholder::error, error.what())}));
    }

    if (!document.is_object()) {
        throw std::runtime_error(message(Message::model_config_root_object,
                                         {arg(Placeholder::path, config_path.string())}));
    }

    ModelProviderConfig config;
    config.adapter = optional_adapter(document, config_path);
    config.provider = optional_provider(document, config_path);
    config.api_url = optional_endpoint(document, config_path);
    config.api_key = optional_string(document, "api_key", config_path);
    config.api_key_env = optional_string(document, "api_key_env", config_path);
    config.model = required_string(document, "model", config_path);
    constexpr auto max_long = std::numeric_limits<long>::max();
    config.connect_timeout_seconds =
        optional_integer(document, "connect_timeout_seconds", config.connect_timeout_seconds, 1,
                         max_long, config_path);
    config.request_timeout_seconds =
        optional_integer(document, "request_timeout_seconds", config.request_timeout_seconds, 1,
                         max_long, config_path);
    config.max_retries = optional_integer(document, "max_retries", config.max_retries, 0,
                                          model_provider_bounds::max_retries, config_path);
    config.retry_initial_delay_ms =
        optional_integer(document, "retry_initial_delay_ms", config.retry_initial_delay_ms, 1,
                         model_provider_bounds::max_retry_initial_delay_ms, config_path);
    config.max_completion_tokens =
        optional_integer(document, "max_completion_tokens", config.max_completion_tokens, 1,
                         model_provider_bounds::max_completion_tokens, config_path);
    config.max_request_tokens = optional_integer(
        document, "max_request_tokens", config.max_request_tokens, 0, max_long, config_path);
    config.request_token_safety_margin =
        optional_integer(document, "request_token_safety_margin",
                         config.request_token_safety_margin, 0, max_long, config_path);
    config.request_token_estimate_bytes_per_token = optional_integer(
        document, "request_token_estimate_bytes_per_token",
        config.request_token_estimate_bytes_per_token,
        model_provider_bounds::min_request_token_estimate_bytes_per_token,
        model_provider_bounds::max_request_token_estimate_bytes_per_token, config_path);
    if (config.max_request_tokens != 0) {
        if (config.request_token_safety_margin >= config.max_request_tokens) {
            throw std::runtime_error(message(Message::model_config_request_margin_invalid,
                                             {arg(Placeholder::path, config_path.string())}));
        }
        if (config.max_completion_tokens >=
            config.max_request_tokens - config.request_token_safety_margin) {
            throw std::runtime_error(message(Message::model_config_request_budget_invalid,
                                             {arg(Placeholder::path, config_path.string())}));
        }
    }
    config.stream = optional_boolean(document, "stream", config.stream, config_path);
    config.capabilities = optional_capabilities(document, config.adapter, config_path);
    config.response_limits = optional_response_limits(document, config_path);
    try {
        model_detail::normalize_model_provider_endpoint(config);
        model_detail::validate_model_provider_credentials(config);
        (void)resolve_model_provider_profile(config);
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error(
            message(Message::model_config_invalid, {arg(Placeholder::path, config_path.string()),
                                                    arg(Placeholder::error, error.what())}));
    }
    return config;
}

} // namespace mint
