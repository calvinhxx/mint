#include "model_settings_credentials.hpp"

#include "model_settings_profile.hpp"

#include <algorithm>
#include <cctype>

namespace mint::model_settings_detail {
namespace {

// EN: Room for 64 legacy entries and the previous/new managed keys.
// ZH-CN: 为 64 个旧条目以及前后两个托管密钥预留空间。
constexpr std::size_t max_credentials = 66;

} // namespace

std::string reference(const Json& document) {
    const auto found = document.find("credential_ref");
    if (found == document.end()) {
        return {};
    }
    if (!found->is_string()) {
        invalid_config();
    }
    const auto value = found->get<std::string>();
    if (!value.starts_with("model:") || value.size() != 38 ||
        !std::all_of(value.begin() + 6, value.end(),
                     [](unsigned char ch) { return std::isxdigit(ch) != 0; })) {
        invalid_config();
    }
    return value;
}

Document credentials_document(const std::filesystem::path& path) {
    auto existing = read_document(path, true);
    Document result{existing ? std::move(existing->value)
                             : Json{{"schema_version", 1}, {"credentials", Json::object()}},
                    {}};
    const auto version = result.value.find("schema_version");
    const auto entries = result.value.find("credentials");
    if (version == result.value.end() || !version->is_number_integer() || *version != 1 ||
        entries == result.value.end() || !entries->is_object() ||
        entries->size() > max_credentials) {
        invalid_config();
    }
    for (auto item = entries->begin(); item != entries->end(); ++item) {
        if (!valid_text(item.key(), 4096) || !item.value().is_string() ||
            !valid_text(item.value().get_ref<const std::string&>(), max_key_bytes)) {
            invalid_config();
        }
    }
    const auto bindings = result.value.find("bindings");
    if (bindings != result.value.end() && !bindings->is_object()) {
        invalid_config();
    }
    return result;
}

std::string_view stored_key(const Json& credentials, const std::string& key_reference,
                            const ModelProviderConfig& config) {
    if (key_reference.empty()) {
        return {};
    }
    const auto& entries = credentials.at("credentials");
    const auto found = entries.find(key_reference);
    if (found == entries.end()) {
        throw ModelConfigError(ModelConfigErrorCode::invalid_config,
                               "The saved API key is missing. Save the key again.");
    }
    const auto bindings = credentials.find("bindings");
    if (bindings == credentials.end() || !bindings->contains(key_reference)) {
        invalid_config();
    }
    const auto& binding = bindings->at(key_reference);
    if (!binding.is_object() || !binding.contains("adapter") || !binding.contains("endpoint") ||
        binding.at("adapter") != model_adapter_name(config.adapter) ||
        binding.at("endpoint") != config.api_url) {
        throw ModelConfigError(ModelConfigErrorCode::invalid_config,
                               "The saved API key does not match this endpoint. Save it again.");
    }
    return found->get_ref<const std::string&>();
}

std::string_view selected_key(const Json& document, const ModelProviderConfig& config,
                              const Json& credentials) {
    const auto key_reference = reference(document);
    if (key_reference.empty()) {
        return config.api_key;
    }
    if (!config.api_key.empty() || !config.api_key_env.empty()) {
        invalid_config();
    }
    return stored_key(credentials, key_reference, config);
}

std::string_view legacy_key(const Json& credentials, const ModelProviderConfig& config) {
    const auto provider = resolve_model_provider_profile(config).provider;
    const auto scope = "v1:" + std::string(model_provider_name(provider)) + ':' + config.api_url;
    const auto& entries = credentials.at("credentials");
    const auto found = entries.find(scope);
    return found == entries.end() ? std::string_view{} : found->get_ref<const std::string&>();
}

bool prune_managed_keys(Json& document, const std::string& keep) {
    bool changed = false;
    auto& entries = document.at("credentials");
    for (auto entry = entries.begin(); entry != entries.end();) {
        if (entry.key().starts_with("model:") && entry.key() != keep) {
            if (document.contains("bindings")) {
                document.at("bindings").erase(entry.key());
            }
            scrub(entry.value());
            entry = entries.erase(entry);
            changed = true;
        } else {
            ++entry;
        }
    }
    return changed;
}

void remove_inline_key(Json& document) {
    const auto field = document.find("api_key");
    if (field != document.end()) {
        scrub(*field);
        document.erase(field);
    }
}

} // namespace mint::model_settings_detail
