#include "model_settings_profile.hpp"

#include "mint/infrastructure/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace mint::model_settings_detail {

constexpr std::size_t max_url_bytes = 8192;
constexpr std::size_t max_model_bytes = 512;

[[noreturn]] void invalid_config() {
    throw ModelConfigError(ModelConfigErrorCode::invalid_config,
                           "The model configuration is invalid.");
}

[[noreturn]] void invalid_input() {
    throw ModelConfigError(ModelConfigErrorCode::invalid_input,
                           "Check the API type, HTTPS or local Base URL, model name and API key.");
}

bool valid_text(std::string_view value, std::size_t limit) {
    if (value.empty() || value.size() > limit ||
        std::any_of(value.begin(), value.end(),
                    [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; })) {
        return false;
    }
    try {
        const Document text{Json(value), {}};
        auto encoded = text.value.dump();
        const SecretGuard erase(encoded);
        return true;
    } catch (...) {
        return false;
    }
}

std::string trim(std::string value) {
    const auto space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), space).base(), value.end());
    return value;
}

void validate_endpoint_text(std::string_view endpoint) {
    if (!valid_text(endpoint, max_url_bytes) || endpoint.find_first_of("?#\\") != endpoint.npos ||
        std::any_of(endpoint.begin(), endpoint.end(),
                    [](unsigned char ch) { return std::isspace(ch) != 0; })) {
        invalid_config();
    }
    const auto scheme = endpoint.find("://");
    if (scheme == endpoint.npos) {
        invalid_config();
    }
    const auto authority_end = endpoint.find('/', scheme + 3);
    const auto authority = endpoint.substr(scheme + 3, authority_end - scheme - 3);
    if (authority.empty() || authority.find('@') != authority.npos ||
        authority.find('%') != authority.npos) {
        invalid_config();
    }
}

std::string normalized_url(std::string base_url) {
    base_url = trim(std::move(base_url));
    validate_endpoint_text(base_url);
    const auto scheme_end = base_url.find("://");
    const auto authority_end = base_url.find('/', scheme_end + 3);
    const auto prefix_end = authority_end == base_url.npos ? base_url.size() : authority_end;
    std::transform(base_url.begin(), base_url.begin() + static_cast<std::ptrdiff_t>(prefix_end),
                   base_url.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const auto authority = base_url.substr(scheme_end + 3, prefix_end - scheme_end - 3);
    const auto default_port = base_url.starts_with("https://") ? ":443" : ":80";
    if (authority.ends_with(default_port)) {
        base_url.erase(prefix_end - std::char_traits<char>::length(default_port),
                       std::char_traits<char>::length(default_port));
    }
    while (base_url.ends_with('/')) {
        base_url.pop_back();
    }
    return base_url;
}

std::string normalized_endpoint(std::string base_url, ModelAdapter adapter) {
    base_url = normalized_url(std::move(base_url));
    std::string_view suffix;
    switch (adapter) {
    case ModelAdapter::chat_completions:
        suffix = "/chat/completions";
        break;
    case ModelAdapter::responses:
        suffix = "/responses";
        break;
    case ModelAdapter::anthropic_messages:
        suffix = "/messages";
        break;
    default:
        invalid_input();
    }
    if (!base_url.ends_with(suffix)) {
        base_url += suffix;
    }
    return base_url;
}

ModelProviderConfig validated_config(const Json& document) {
    try {
        auto config = parse_model_provider_config(document, "model.json");
        SecretGuard erase(config.api_key);
        validate_endpoint_text(config.api_url);
        if (!valid_text(config.model, max_model_bytes) ||
            (!config.api_key.empty() && !valid_text(config.api_key, max_key_bytes))) {
            invalid_config();
        }
        erase.release();
        return config;
    } catch (...) {
        invalid_config();
    }
}

ModelProviderConfig override_config(const Json& document) {
    try {
        // EN: Explicit CLI profiles retain their protocol/escaping semantics.
        // ZH-CN: 显式 CLI 配置保留现有协议和转义语义。
        return parse_model_provider_config(document, "model override");
    } catch (...) {
        invalid_config();
    }
}

std::string normalize_update(ModelConfigUpdate& update) {
    if (update.api_key && (update.remove_api_key || !valid_text(*update.api_key, max_key_bytes))) {
        invalid_input();
    }
    update.model = trim(std::move(update.model));
    try {
        auto endpoint = normalized_endpoint(std::move(update.base_url), update.adapter);
        (void)validated_config({{"provider", "custom"},
                                {"adapter", model_adapter_name(update.adapter)},
                                {"endpoint", endpoint},
                                {"model", update.model}});
        return endpoint;
    } catch (...) {
        invalid_input();
    }
}

bool environment_available(const ModelProviderConfig& config) {
    if (config.api_key_env.empty()) {
        return false;
    }
    const auto* value = std::getenv(config.api_key_env.c_str());
    return value != nullptr && *value != '\0';
}

ModelConfigSnapshot redacted_snapshot(ModelProviderConfig config, const std::filesystem::path& path,
                                      const std::string& revision, bool is_override,
                                      bool saved_api_key) {
    const SecretGuard erase(config.api_key);
    scrub(config.api_key);
    config.api_key.clear();
    if (is_override) {
        // EN: Legacy query/fragment text may contain credentials; keep it out of metadata.
        // ZH-CN: 旧查询串或片段可能含凭据，不得进入展示元数据。
        const auto suffix = config.api_url.find_first_of("?#");
        if (suffix != std::string::npos) {
            scrub_memory(config.api_url.data() + suffix, config.api_url.size() - suffix);
            config.api_url.erase(suffix);
        }
    }
    ModelConfigSnapshot result;
    result.path = path;
    result.revision = revision;
    result.configured = true;
    result.saved_api_key = saved_api_key;
    result.credential_available = saved_api_key || environment_available(config);
    result.is_override = is_override;
    result.config = std::move(config);
    return result;
}

} // namespace mint::model_settings_detail
