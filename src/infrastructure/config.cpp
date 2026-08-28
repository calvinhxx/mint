#include "mint/infrastructure/config.hpp"

#include "model_provider_profile.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mint {
namespace {

std::string required_string(const Json& document, const char* field,
                            const std::filesystem::path& config_path) {
    if (!document.contains(field) || !document.at(field).is_string() ||
        document.at(field).get_ref<const std::string&>().empty()) {
        throw std::runtime_error("配置文件 " + config_path.string() + " 中的 \"" + field +
                                 "\" 必须是非空字符串");
    }
    return document.at(field).get<std::string>();
}

std::string optional_string(const Json& document, const char* field,
                            const std::filesystem::path& config_path) {
    if (!document.contains(field)) {
        return {};
    }
    if (!document.at(field).is_string()) {
        throw std::runtime_error("配置文件 " + config_path.string() + " 中的 \"" + field +
                                 "\" 必须是字符串");
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
        throw std::runtime_error("配置文件 " + config_path.string() + " 中的 \"" + field +
                                 "\" 必须是整数");
    }
    const auto parsed = value.get<long long>();
    if (parsed < minimum || parsed > maximum) {
        throw std::runtime_error("配置文件 " + config_path.string() + " 中的 \"" + field +
                                 "\" 必须在 " + std::to_string(minimum) + " 到 " +
                                 std::to_string(maximum) + " 之间");
    }
    return static_cast<long>(parsed);
}

bool optional_boolean(const Json& document, const char* field, bool fallback,
                      const std::filesystem::path& config_path) {
    if (!document.contains(field)) {
        return fallback;
    }
    if (!document.at(field).is_boolean()) {
        throw std::runtime_error("配置文件 " + config_path.string() + " 中的 \"" + field +
                                 "\" 必须是布尔值");
    }
    return document.at(field).get<bool>();
}

ModelAdapter optional_adapter(const Json& document, const std::filesystem::path& config_path) {
    if (!document.contains("adapter")) {
        return ModelAdapter::chat_completions;
    }
    const auto adapter = required_string(document, "adapter", config_path);
    if (const auto parsed = model_detail::parse_model_adapter(adapter)) {
        return *parsed;
    }
    throw std::runtime_error("配置文件 " + config_path.string() +
                             " 中的 \"adapter\" 只能是 chat_completions 或 responses");
}

ModelProvider optional_provider(const Json& document, const std::filesystem::path& config_path) {
    if (!document.contains("provider")) {
        return ModelProvider::automatic;
    }
    const auto provider = required_string(document, "provider", config_path);
    if (const auto parsed = model_detail::parse_model_provider(provider)) {
        return *parsed;
    }
    throw std::runtime_error("配置文件 " + config_path.string() +
                             " 中的 \"provider\" 只能是 auto、custom、openai、groq 或 "
                             "deepseek");
}

ModelTokenLimitParameter token_limit_parameter(const Json& capabilities, ModelAdapter adapter,
                                               const std::filesystem::path& config_path) {
    const auto fallback =
        adapter == ModelAdapter::responses ? "max_output_tokens" : "max_completion_tokens";
    const auto parameter = capabilities.contains("token_limit_parameter")
                               ? required_string(capabilities, "token_limit_parameter", config_path)
                               : std::string(fallback);
    if (const auto parsed = model_detail::parse_model_token_limit_parameter(parameter)) {
        return *parsed;
    }
    throw std::runtime_error("配置文件 " + config_path.string() +
                             " 中 capabilities.token_limit_parameter 无效");
}

std::optional<ModelProviderCapabilities>
optional_capabilities(const Json& document, ModelAdapter adapter,
                      const std::filesystem::path& config_path) {
    if (!document.contains("capabilities")) {
        return std::nullopt;
    }
    const auto& capabilities = document.at("capabilities");
    if (!capabilities.is_object()) {
        throw std::runtime_error("配置文件 " + config_path.string() +
                                 " 中的 \"capabilities\" 必须是对象");
    }
    constexpr std::string_view allowed[] = {"function_tools",        "streaming",
                                            "stream_usage",          "stateless_reasoning_replay",
                                            "token_limit_parameter", "explicit_tool_choice",
                                            "chat_reasoning_replay", "requires_tool_call_content"};
    for (auto item = capabilities.begin(); item != capabilities.end(); ++item) {
        if (std::find(allowed, std::end(allowed), item.key()) == std::end(allowed)) {
            throw std::runtime_error("配置文件 " + config_path.string() +
                                     " 包含未知 capability: " + item.key());
        }
    }

    ModelProviderCapabilities result;
    if (adapter == ModelAdapter::responses) {
        result.stream_usage = false;
        result.stateless_reasoning_replay = true;
        result.token_limit_parameter = ModelTokenLimitParameter::max_output_tokens;
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

} // namespace

ModelProviderConfig load_model_provider_config(const std::filesystem::path& config_path) {
    std::ifstream input(config_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("找不到配置文件 " + config_path.string() +
                                 "。请复制一份 provider 配置为 config.json，并设置对应的 API Key "
                                 "环境变量。");
    }

    Json document;
    try {
        input >> document;
    } catch (const Json::exception& error) {
        throw std::runtime_error("配置文件 " + config_path.string() +
                                 " 不是有效 JSON: " + error.what());
    }

    if (!document.is_object()) {
        throw std::runtime_error("配置文件 " + config_path.string() + " 的最外层必须是 JSON 对象");
    }

    ModelProviderConfig config;
    config.adapter = optional_adapter(document, config_path);
    config.provider = optional_provider(document, config_path);
    config.api_url = required_string(document, "api_url", config_path);
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
    config.stream = optional_boolean(document, "stream", config.stream, config_path);
    config.capabilities = optional_capabilities(document, config.adapter, config_path);
    try {
        model_detail::validate_model_provider_credentials(config);
        (void)resolve_model_provider_profile(config);
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error("配置文件 " + config_path.string() + ": " + error.what());
    }
    return config;
}

ModelProviderConfig load_chat_completions_config(const std::filesystem::path& config_path) {
    return load_model_provider_config(config_path);
}

} // namespace mint
