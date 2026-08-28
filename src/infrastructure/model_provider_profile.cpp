#include "mint/infrastructure/model_provider_client.hpp"

#include "model_provider_profile.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mint {
namespace {

template <typename Enum, std::size_t Size>
std::optional<Enum>
enum_from_name(std::string_view value,
               const std::array<std::pair<Enum, std::string_view>, Size>& entries) noexcept {
    const auto item = std::find_if(entries.begin(), entries.end(),
                                   [value](const auto& entry) { return entry.second == value; });
    return item == entries.end() ? std::nullopt : std::optional<Enum>(item->first);
}

template <typename Enum, std::size_t Size>
std::string_view
enum_name(Enum value, const std::array<std::pair<Enum, std::string_view>, Size>& entries) noexcept {
    const auto item = std::find_if(entries.begin(), entries.end(),
                                   [value](const auto& entry) { return entry.first == value; });
    return item == entries.end() ? "unknown" : item->second;
}

constexpr std::array adapter_names = {
    std::pair{ModelAdapter::chat_completions, std::string_view("chat_completions")},
    std::pair{ModelAdapter::responses, std::string_view("responses")},
};

constexpr std::array token_parameter_names = {
    std::pair{ModelTokenLimitParameter::max_completion_tokens,
              std::string_view("max_completion_tokens")},
    std::pair{ModelTokenLimitParameter::max_tokens, std::string_view("max_tokens")},
    std::pair{ModelTokenLimitParameter::max_output_tokens, std::string_view("max_output_tokens")},
};

struct ProviderDefinition {
    ModelProvider provider;
    std::string_view name;
    std::string_view official_host;
    ModelTokenLimitParameter chat_token_limit;
    bool supports_responses;
};

constexpr std::array provider_catalog = {
    ProviderDefinition{ModelProvider::automatic, "auto", "",
                       ModelTokenLimitParameter::max_completion_tokens, true},
    ProviderDefinition{ModelProvider::custom, "custom", "",
                       ModelTokenLimitParameter::max_completion_tokens, true},
    ProviderDefinition{ModelProvider::openai, "openai", "api.openai.com",
                       ModelTokenLimitParameter::max_completion_tokens, true},
    ProviderDefinition{ModelProvider::groq, "groq", "api.groq.com",
                       ModelTokenLimitParameter::max_completion_tokens, false},
    ProviderDefinition{ModelProvider::deepseek, "deepseek", "api.deepseek.com",
                       ModelTokenLimitParameter::max_tokens, false},
};

const ProviderDefinition* provider_definition(ModelProvider provider) noexcept {
    const auto item = std::find_if(
        provider_catalog.begin(), provider_catalog.end(),
        [provider](const auto& definition) { return definition.provider == provider; });
    return item == provider_catalog.end() ? nullptr : &*item;
}

struct ParsedUrl {
    std::string host;
};

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

ParsedUrl parse_http_url(std::string_view url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        throw std::invalid_argument("模型接口地址必须是 http 或 https URL");
    }
    const auto scheme = lowercase_ascii(std::string(url.substr(0, scheme_end)));
    if (scheme != "http" && scheme != "https") {
        throw std::invalid_argument("模型接口地址只支持 http 或 https");
    }

    const auto authority_begin = scheme_end + 3;
    const auto authority_end = url.find_first_of("/?#", authority_begin);
    const auto authority_size = authority_end == std::string_view::npos
                                    ? std::string_view::npos
                                    : authority_end - authority_begin;
    auto authority = url.substr(authority_begin, authority_size);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        throw std::invalid_argument("模型接口地址缺少主机或包含 URL 凭据");
    }

    std::string_view host;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            throw std::invalid_argument("模型接口地址包含无效 IPv6 主机");
        }
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] != ':') {
            throw std::invalid_argument("模型接口地址包含无效主机");
        }
    } else {
        const auto port = authority.rfind(':');
        host = port == std::string_view::npos ? authority : authority.substr(0, port);
    }
    if (host.empty()) {
        throw std::invalid_argument("模型接口地址缺少主机");
    }
    return {lowercase_ascii(std::string(host))};
}

std::optional<ModelProvider> provider_for_host(std::string_view host) {
    const auto item = std::find_if(
        provider_catalog.begin(), provider_catalog.end(), [host](const auto& definition) {
            return !definition.official_host.empty() && definition.official_host == host;
        });
    return item == provider_catalog.end() ? std::nullopt
                                          : std::optional<ModelProvider>(item->provider);
}

ModelProviderCapabilities chat_capabilities(ModelTokenLimitParameter token_parameter) {
    return {.function_tools = true,
            .streaming = true,
            .stream_usage = true,
            .stateless_reasoning_replay = false,
            .token_limit_parameter = token_parameter};
}

ModelProviderCapabilities responses_capabilities() {
    return {.function_tools = true,
            .streaming = true,
            .stream_usage = false,
            .stateless_reasoning_replay = true,
            .token_limit_parameter = ModelTokenLimitParameter::max_output_tokens};
}

ModelProviderCapabilities default_capabilities(ModelProvider provider, ModelAdapter adapter) {
    if (adapter == ModelAdapter::responses) {
        return responses_capabilities();
    }
    const auto* definition = provider_definition(provider);
    return chat_capabilities(definition == nullptr ? ModelTokenLimitParameter::max_completion_tokens
                                                   : definition->chat_token_limit);
}

void validate_profile(const ModelProviderConfig& config, const ModelProviderProfile& profile) {
    const auto* definition = provider_definition(profile.provider);
    if (profile.adapter == ModelAdapter::responses &&
        (definition == nullptr || !definition->supports_responses)) {
        throw std::invalid_argument(std::string(model_provider_name(profile.provider)) +
                                    " profile 目前只支持 chat_completions adapter");
    }
    if (profile.provider != ModelProvider::custom && config.capabilities.has_value()) {
        throw std::invalid_argument("只有 custom provider 可以覆盖 capabilities");
    }
    if (config.stream && !profile.capabilities.streaming) {
        throw std::invalid_argument("当前 provider profile 不支持流式响应");
    }
    if (profile.capabilities.stream_usage && !profile.capabilities.streaming) {
        throw std::invalid_argument("stream_usage 需要先启用 streaming 能力");
    }
    if (profile.adapter == ModelAdapter::responses) {
        if (profile.capabilities.stream_usage || profile.capabilities.token_limit_parameter !=
                                                     ModelTokenLimitParameter::max_output_tokens) {
            throw std::invalid_argument(
                "Responses adapter 需要 max_output_tokens，且不使用 Chat stream_usage 选项");
        }
    } else if (profile.capabilities.stateless_reasoning_replay ||
               profile.capabilities.token_limit_parameter ==
                   ModelTokenLimitParameter::max_output_tokens) {
        throw std::invalid_argument(
            "Chat Completions adapter 不支持 Responses 推理续传或 max_output_tokens");
    }
}

bool valid_environment_name(std::string_view name) {
    if (name.empty() || name.size() > 128 ||
        !(std::isalpha(static_cast<unsigned char>(name.front())) != 0 || name.front() == '_')) {
        return false;
    }
    return std::all_of(name.begin() + 1, name.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '_';
    });
}

} // namespace

std::string_view model_adapter_name(ModelAdapter adapter) noexcept {
    return enum_name(adapter, adapter_names);
}

std::string_view model_provider_name(ModelProvider provider) noexcept {
    const auto* definition = provider_definition(provider);
    return definition == nullptr ? "unknown" : definition->name;
}

std::string_view model_provider_source_name(ModelProviderSource source) noexcept {
    switch (source) {
    case ModelProviderSource::explicit_config:
        return "config";
    case ModelProviderSource::endpoint:
        return "endpoint";
    case ModelProviderSource::compatibility_default:
        return "compatibility_default";
    }
    return "unknown";
}

std::string_view model_token_limit_parameter_name(ModelTokenLimitParameter parameter) noexcept {
    return enum_name(parameter, token_parameter_names);
}

ModelProviderProfile resolve_model_provider_profile(const ModelProviderConfig& config) {
    const auto parsed_url = parse_http_url(config.api_url);
    ModelProviderProfile profile;
    profile.adapter = config.adapter;

    if (config.provider != ModelProvider::automatic) {
        profile.provider = config.provider;
        profile.source = ModelProviderSource::explicit_config;
    } else if (const auto detected = provider_for_host(parsed_url.host)) {
        profile.provider = *detected;
        profile.source = ModelProviderSource::endpoint;
    } else {
        profile.provider = ModelProvider::custom;
        profile.source = ModelProviderSource::compatibility_default;
    }

    profile.capabilities =
        config.capabilities.value_or(default_capabilities(profile.provider, profile.adapter));
    validate_profile(config, profile);
    return profile;
}

Json model_provider_profile_to_json(const ModelProviderProfile& profile) {
    return {{"provider", model_provider_name(profile.provider)},
            {"source", model_provider_source_name(profile.source)},
            {"adapter", model_adapter_name(profile.adapter)},
            {"capabilities",
             {{"function_tools", profile.capabilities.function_tools},
              {"streaming", profile.capabilities.streaming},
              {"stream_usage", profile.capabilities.stream_usage},
              {"stateless_reasoning_replay", profile.capabilities.stateless_reasoning_replay},
              {"token_limit_parameter",
               model_token_limit_parameter_name(profile.capabilities.token_limit_parameter)}}}};
}

namespace model_detail {

std::optional<ModelAdapter> parse_model_adapter(std::string_view value) noexcept {
    return enum_from_name(value, adapter_names);
}

std::optional<ModelProvider> parse_model_provider(std::string_view value) noexcept {
    const auto item =
        std::find_if(provider_catalog.begin(), provider_catalog.end(),
                     [value](const auto& definition) { return definition.name == value; });
    return item == provider_catalog.end() ? std::nullopt
                                          : std::optional<ModelProvider>(item->provider);
}

std::optional<ModelTokenLimitParameter>
parse_model_token_limit_parameter(std::string_view value) noexcept {
    return enum_from_name(value, token_parameter_names);
}

void validate_model_provider_credentials(const ModelProviderConfig& config) {
    if (!config.api_key.empty() && !config.api_key_env.empty()) {
        throw std::invalid_argument("api_key 与 api_key_env 不能同时设置");
    }
    if (!config.api_key_env.empty() && !valid_environment_name(config.api_key_env)) {
        throw std::invalid_argument("api_key_env 不是有效的环境变量名");
    }
}

void resolve_model_provider_credentials(ModelProviderConfig& config) {
    validate_model_provider_credentials(config);
    if (config.api_key_env.empty()) {
        return;
    }
    const auto* value = std::getenv(config.api_key_env.c_str());
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error("模型 API Key 环境变量未设置: " + config.api_key_env);
    }
    config.api_key = value;
}

} // namespace model_detail
} // namespace mint
