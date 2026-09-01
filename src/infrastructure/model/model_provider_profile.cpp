#include "mint/infrastructure/model_provider_client.hpp"

#include "model_provider_profile.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
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
    std::pair{ModelAdapter::anthropic_messages, std::string_view("anthropic_messages")},
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
    std::string_view chat_endpoint_path;
    std::string_view responses_endpoint_path;
    std::string_view anthropic_messages_endpoint_path;
    ModelProviderCapabilities chat_capabilities;
};

constexpr ModelProviderCapabilities chat_capabilities(ModelTokenLimitParameter token_parameter,
                                                      bool explicit_tool_choice = true,
                                                      bool reasoning_replay = false,
                                                      bool requires_tool_call_content = false) {
    return {.function_tools = true,
            .streaming = true,
            .stream_usage = true,
            .stateless_reasoning_replay = false,
            .token_limit_parameter = token_parameter,
            .explicit_tool_choice = explicit_tool_choice,
            .chat_reasoning_replay = reasoning_replay,
            .requires_tool_call_content = requires_tool_call_content};
}

constexpr ModelProviderCapabilities responses_capabilities(bool encrypted_reasoning_replay) {
    return {.function_tools = true,
            .streaming = true,
            .stream_usage = false,
            .stateless_reasoning_replay = encrypted_reasoning_replay,
            .token_limit_parameter = ModelTokenLimitParameter::max_output_tokens,
            .explicit_tool_choice = true,
            .chat_reasoning_replay = false,
            .requires_tool_call_content = false};
}

constexpr std::array provider_catalog = {
    ProviderDefinition{ModelProvider::automatic, "auto", "", "", "", "",
                       chat_capabilities(ModelTokenLimitParameter::max_completion_tokens)},
    ProviderDefinition{ModelProvider::custom, "custom", "", "", "", "",
                       chat_capabilities(ModelTokenLimitParameter::max_completion_tokens)},
    ProviderDefinition{ModelProvider::openai, "openai", "api.openai.com", "/v1/chat/completions",
                       "/v1/responses", "",
                       chat_capabilities(ModelTokenLimitParameter::max_completion_tokens)},
    ProviderDefinition{ModelProvider::anthropic, "anthropic", "api.anthropic.com", "", "",
                       "/v1/messages", chat_capabilities(ModelTokenLimitParameter::max_tokens)},
    ProviderDefinition{ModelProvider::google, "google", "generativelanguage.googleapis.com",
                       "/v1beta/openai/chat/completions", "", "",
                       chat_capabilities(ModelTokenLimitParameter::max_completion_tokens)},
    ProviderDefinition{ModelProvider::xai, "xai", "api.x.ai", "/v1/chat/completions",
                       "/v1/responses", "",
                       chat_capabilities(ModelTokenLimitParameter::max_completion_tokens)},
    ProviderDefinition{
        ModelProvider::moonshot, "moonshot", "api.moonshot.ai", "/v1/chat/completions", "", "",
        chat_capabilities(ModelTokenLimitParameter::max_completion_tokens, true, true, true)},
    ProviderDefinition{ModelProvider::groq, "groq", "api.groq.com", "/openai/v1/chat/completions",
                       "", "", chat_capabilities(ModelTokenLimitParameter::max_completion_tokens)},
    ProviderDefinition{ModelProvider::deepseek, "deepseek", "api.deepseek.com", "/chat/completions",
                       "", "",
                       chat_capabilities(ModelTokenLimitParameter::max_tokens, false, true, true)},
};

const ProviderDefinition* provider_definition(ModelProvider provider) noexcept {
    const auto item = std::find_if(
        provider_catalog.begin(), provider_catalog.end(),
        [provider](const auto& definition) { return definition.provider == provider; });
    return item == provider_catalog.end() ? nullptr : &*item;
}

struct ParsedUrl {
    bool uses_tls = false;
    bool bracketed_host = false;
    std::string host;
    std::string path;
    bool has_query_or_fragment = false;
};

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

void validate_port(std::string_view port) {
    unsigned value = 0;
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
    if (port.empty() || parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() ||
        value > 65'535) {
        throw std::invalid_argument("模型接口地址包含无效端口");
    }
}

bool is_ipv4_loopback(std::string_view host) {
    std::array<unsigned, 4> octets{};
    std::size_t count = 0;
    for (auto& octet : octets) {
        const auto separator = host.find('.');
        const auto token = host.substr(0, separator);
        unsigned value = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (token.empty() || (token.size() > 1 && token.front() == '0') ||
            parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value > 255) {
            return false;
        }
        octet = value;
        ++count;
        if (separator == std::string_view::npos) {
            host = {};
            break;
        }
        if (count == octets.size()) {
            return false;
        }
        host.remove_prefix(separator + 1);
    }
    return host.empty() && count == octets.size() && octets.front() == 127;
}

bool parse_ipv6_groups(std::string_view text, std::array<std::uint16_t, 8>& groups,
                       std::size_t& count) {
    if (text.empty()) {
        return true;
    }
    while (true) {
        const auto separator = text.find(':');
        const auto token = text.substr(0, separator);
        unsigned value = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value, 16);
        if (token.empty() || count == groups.size() || parsed.ec != std::errc{} ||
            parsed.ptr != token.data() + token.size() || value > 0xffff) {
            return false;
        }
        groups[count++] = static_cast<std::uint16_t>(value);
        if (separator == std::string_view::npos) {
            return true;
        }
        text.remove_prefix(separator + 1);
    }
}

bool is_ipv6_loopback(std::string_view host) {
    std::array<std::uint16_t, 8> groups{};
    const auto compression = host.find("::");
    if (compression == std::string_view::npos) {
        std::size_t count = 0;
        return parse_ipv6_groups(host, groups, count) && count == groups.size() &&
               std::all_of(groups.begin(), groups.end() - 1,
                           [](std::uint16_t group) { return group == 0; }) &&
               groups.back() == 1;
    }
    if (host.find("::", compression + 2) != std::string_view::npos) {
        return false;
    }

    std::array<std::uint16_t, 8> left{};
    std::array<std::uint16_t, 8> right{};
    std::size_t left_count = 0;
    std::size_t right_count = 0;
    if (!parse_ipv6_groups(host.substr(0, compression), left, left_count) ||
        !parse_ipv6_groups(host.substr(compression + 2), right, right_count) ||
        left_count + right_count >= groups.size()) {
        return false;
    }
    std::copy_n(left.begin(), left_count, groups.begin());
    std::copy_n(right.begin(), right_count,
                groups.end() - static_cast<std::ptrdiff_t>(right_count));
    return std::all_of(groups.begin(), groups.end() - 1,
                       [](std::uint16_t group) { return group == 0; }) &&
           groups.back() == 1;
}

bool is_loopback_host(const ParsedUrl& url) {
    if (url.bracketed_host) {
        return is_ipv6_loopback(url.host);
    }
    return url.host == "localhost" || is_ipv4_loopback(url.host);
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
    std::string_view port;
    bool bracketed_host = false;
    if (authority.front() == '[') {
        bracketed_host = true;
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            throw std::invalid_argument("模型接口地址包含无效 IPv6 主机");
        }
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                throw std::invalid_argument("模型接口地址包含无效主机");
            }
            port = authority.substr(close + 2);
        }
    } else {
        const auto separator = authority.find(':');
        if (separator != std::string_view::npos) {
            if (authority.find(':', separator + 1) != std::string_view::npos) {
                throw std::invalid_argument("IPv6 模型接口地址必须使用方括号");
            }
            host = authority.substr(0, separator);
            port = authority.substr(separator + 1);
        } else {
            host = authority;
        }
    }
    if (host.empty()) {
        throw std::invalid_argument("模型接口地址缺少主机");
    }
    if (!port.empty() || authority.back() == ':') {
        validate_port(port);
    }
    std::string path;
    if (authority_end != std::string_view::npos && url[authority_end] == '/') {
        const auto path_end = url.find_first_of("?#", authority_end);
        path = std::string(url.substr(authority_end, path_end - authority_end));
    }
    ParsedUrl parsed{.uses_tls = scheme == "https",
                     .bracketed_host = bracketed_host,
                     .host = lowercase_ascii(std::string(host)),
                     .path = std::move(path),
                     .has_query_or_fragment =
                         url.find_first_of("?#", authority_begin) != std::string_view::npos};
    if (!parsed.uses_tls && !is_loopback_host(parsed)) {
        throw std::invalid_argument(
            "远程模型接口必须使用 https；明文 http 仅允许 localhost、127.0.0.0/8 或 ::1 "
            "回环地址");
    }
    return parsed;
}

std::optional<ModelProvider> provider_for_host(std::string_view host) {
    const auto item = std::find_if(
        provider_catalog.begin(), provider_catalog.end(), [host](const auto& definition) {
            return !definition.official_host.empty() && definition.official_host == host;
        });
    return item == provider_catalog.end() ? std::nullopt
                                          : std::optional<ModelProvider>(item->provider);
}

std::string_view endpoint_path(const ProviderDefinition& definition, ModelAdapter adapter) {
    switch (adapter) {
    case ModelAdapter::chat_completions:
        return definition.chat_endpoint_path;
    case ModelAdapter::responses:
        return definition.responses_endpoint_path;
    case ModelAdapter::anthropic_messages:
        return definition.anthropic_messages_endpoint_path;
    }
    return {};
}

std::optional<std::string> default_endpoint(ModelProvider provider, ModelAdapter adapter) {
    const auto* definition = provider_definition(provider);
    if (definition == nullptr || definition->official_host.empty()) {
        return std::nullopt;
    }
    const auto path = endpoint_path(*definition, adapter);
    if (path.empty()) {
        return std::nullopt;
    }
    return "https://" + std::string(definition->official_host) + std::string(path);
}

ModelProviderCapabilities default_capabilities(ModelProvider provider, ModelAdapter adapter) {
    if (adapter == ModelAdapter::responses) {
        return responses_capabilities(provider == ModelProvider::openai ||
                                      provider == ModelProvider::xai ||
                                      provider == ModelProvider::custom);
    }
    if (adapter == ModelAdapter::anthropic_messages) {
        return chat_capabilities(ModelTokenLimitParameter::max_tokens);
    }
    const auto* definition = provider_definition(provider);
    return definition == nullptr
               ? chat_capabilities(ModelTokenLimitParameter::max_completion_tokens)
               : definition->chat_capabilities;
}

void validate_profile(const ModelProviderConfig& config, const ModelProviderProfile& profile) {
    const auto* definition = provider_definition(profile.provider);
    if (profile.provider != ModelProvider::custom &&
        (definition == nullptr || endpoint_path(*definition, profile.adapter).empty())) {
        throw std::invalid_argument(std::string(model_provider_name(profile.provider)) +
                                    " profile 不支持 " +
                                    std::string(model_adapter_name(profile.adapter)) + " adapter");
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
        if (profile.capabilities.stream_usage ||
            profile.capabilities.token_limit_parameter !=
                ModelTokenLimitParameter::max_output_tokens ||
            profile.capabilities.chat_reasoning_replay ||
            profile.capabilities.requires_tool_call_content) {
            throw std::invalid_argument(
                "Responses adapter 需要 max_output_tokens，且不使用 Chat 专属能力");
        }
    } else if (profile.capabilities.stateless_reasoning_replay ||
               profile.capabilities.token_limit_parameter ==
                   ModelTokenLimitParameter::max_output_tokens) {
        throw std::invalid_argument("当前 adapter 不支持 Responses 推理续传或 max_output_tokens");
    }
    if (profile.adapter == ModelAdapter::anthropic_messages &&
        profile.capabilities.token_limit_parameter != ModelTokenLimitParameter::max_tokens) {
        throw std::invalid_argument("Anthropic Messages adapter 需要 max_tokens");
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
               model_token_limit_parameter_name(profile.capabilities.token_limit_parameter)},
              {"explicit_tool_choice", profile.capabilities.explicit_tool_choice},
              {"chat_reasoning_replay", profile.capabilities.chat_reasoning_replay},
              {"requires_tool_call_content", profile.capabilities.requires_tool_call_content}}}};
}

namespace model_detail {

bool is_plaintext_loopback_model_endpoint(std::string_view api_url) {
    const auto parsed = parse_http_url(api_url);
    return !parsed.uses_tls && is_loopback_host(parsed);
}

void normalize_model_provider_endpoint(ModelProviderConfig& config) {
    if (config.api_url.empty()) {
        const auto endpoint = default_endpoint(config.provider, config.adapter);
        if (!endpoint.has_value()) {
            throw std::invalid_argument(
                "custom 或 auto provider 必须配置完整 endpoint；官方 provider 可省略");
        }
        config.api_url = *endpoint;
        return;
    }

    const auto parsed = parse_http_url(config.api_url);
    const auto provider = config.provider != ModelProvider::automatic
                              ? std::optional<ModelProvider>(config.provider)
                              : provider_for_host(parsed.host);
    if (!provider.has_value()) {
        return;
    }
    const auto* definition = provider_definition(*provider);
    if (definition == nullptr || parsed.host != definition->official_host ||
        (!parsed.path.empty() && parsed.path != "/")) {
        return;
    }
    if (parsed.has_query_or_fragment) {
        throw std::invalid_argument("官方 provider 的根地址不能包含 query 或 fragment");
    }
    const auto endpoint = default_endpoint(*provider, config.adapter);
    if (!endpoint.has_value()) {
        throw std::invalid_argument(std::string(model_provider_name(*provider)) +
                                    " profile 目前不支持 " +
                                    std::string(model_adapter_name(config.adapter)) + " adapter");
    }
    config.api_url = *endpoint;
}

std::optional<ModelAdapter> parse_model_adapter(std::string_view value) noexcept {
    if (value == "messages") {
        return ModelAdapter::anthropic_messages;
    }
    return enum_from_name(value, adapter_names);
}

std::optional<ModelProvider> parse_model_provider(std::string_view value) noexcept {
    constexpr std::array aliases = {
        std::pair{std::string_view("claude"), ModelProvider::anthropic},
        std::pair{std::string_view("gemini"), ModelProvider::google},
        std::pair{std::string_view("grok"), ModelProvider::xai},
        std::pair{std::string_view("kimi"), ModelProvider::moonshot},
        std::pair{std::string_view("codex"), ModelProvider::openai},
    };
    const auto alias = std::find_if(aliases.begin(), aliases.end(),
                                    [value](const auto& item) { return item.first == value; });
    if (alias != aliases.end()) {
        return alias->second;
    }
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
