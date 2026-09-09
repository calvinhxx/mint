#include "model_settings_transactions.hpp"

#include "model_settings_credentials.hpp"
#include "model_settings_profile.hpp"

namespace mint::model_settings_detail {
namespace {

ModelConfigSnapshot committed_snapshot(const std::filesystem::path& path, const Json& credentials) {
    const auto document = read_document(path, true);
    if (!document) {
        throw ModelConfigError(ModelConfigErrorCode::unavailable,
                               "The saved model configuration is unavailable.");
    }
    auto config = validated_config(document->value);
    const SecretGuard erase(config.api_key);
    const auto saved = !selected_key(document->value, config, credentials).empty();
    return redacted_snapshot(std::move(config), path, document->revision, false, saved);
}

void commit(const std::filesystem::path& config_path, const std::filesystem::path& credentials_path,
            Json& document, Json& credentials, const std::string& key,
            const std::string& previous_reference) {
    bool credentials_changed = prune_managed_keys(credentials, previous_reference);
    std::string next_reference;
    if (!key.empty()) {
        next_reference = model_settings_detail::unique_reference();
        credentials.at("credentials")[next_reference] = key;
        auto config = validated_config(document);
        const SecretGuard erase(config.api_key);
        credentials["bindings"][next_reference] = {{"adapter", model_adapter_name(config.adapter)},
                                                   {"endpoint", config.api_url}};
        document["credential_ref"] = next_reference;
        document.erase("api_key_env");
        // EN: Copy-on-write keeps the old reference/key valid until model.json commits.
        // ZH-CN: 写时复制确保 model.json 提交前旧引用和旧密钥始终有效。
        write_document(credentials_path, credentials);
        credentials_changed = false;
    } else {
        document.erase("credential_ref");
    }
    remove_inline_key(document);
    write_document(config_path, document);
    // EN: Cleanup is not the commit point. A failure leaves an unused private key, not
    //     a model referring to an absent key. The next successful write retries cleanup.
    // ZH-CN: 清理不是提交点；清理失败只遗留未使用的私有密钥，不会产生悬空引用。
    //        后续成功写入会再次尝试清理。
    credentials_changed = prune_managed_keys(credentials, next_reference) || credentials_changed;
    try {
        if (credentials_changed && std::filesystem::exists(credentials_path)) {
            write_document(credentials_path, credentials);
        }
    } catch (...) {
    }
}

} // namespace

ModelConfigSnapshot migrate_locked(const std::filesystem::path& config_path,
                                   const std::filesystem::path& credentials_path,
                                   const std::filesystem::path& legacy_path,
                                   const std::optional<std::string>& expected_revision) {
    auto shared = read_document(config_path, true);
    if (expected_revision && (!shared || *expected_revision != shared->revision)) {
        throw ModelConfigError(ModelConfigErrorCode::conflict,
                               "Model settings changed elsewhere. Reload before importing.");
    }
    if (!expected_revision && shared) {
        throw ModelConfigError(ModelConfigErrorCode::conflict,
                               "Shared model settings already exist; import was not applied.");
    }
    // EN: Importing a managed destination as its own source would alter the original.
    // ZH-CN: 不能将托管目标文件作为导入源，否则会修改原文件。
    for (const auto& destination : {config_path, credentials_path}) {
        std::error_code error;
        if (std::filesystem::equivalent(legacy_path, destination, error)) {
            invalid_input();
        }
    }
    auto credentials = credentials_document(credentials_path);
    auto legacy = read_document(legacy_path, false);
    if (!legacy) {
        throw ModelConfigError(ModelConfigErrorCode::unavailable,
                               "The previous model configuration is unavailable.");
    }
    auto config = validated_config(legacy->value);
    const SecretGuard erase_config(config.api_key);
    std::string key{config.api_key.empty() ? legacy_key(credentials.value, config)
                                           : std::string_view(config.api_key)};
    const SecretGuard erase_key(key);
    // EN: A supplied legacy reference must not select an unrelated managed key.
    // ZH-CN: 传入的旧引用不允许选取无关的托管密钥。
    legacy->value.erase("credential_ref");
    const auto previous_reference = shared ? reference(shared->value) : std::string{};
    commit(config_path, credentials_path, legacy->value, credentials.value, key,
           previous_reference);
    return committed_snapshot(config_path, credentials.value);
}

ModelConfigSnapshot update_locked(const std::filesystem::path& config_path,
                                  const std::filesystem::path& credentials_path,
                                  ModelConfigUpdate update, std::string endpoint) {
    const SecretGuard erase(update.api_key ? &*update.api_key : nullptr);
    const auto shared = read_document(config_path, true);
    if (update.expected_revision != (shared ? shared->revision : std::string{})) {
        throw ModelConfigError(ModelConfigErrorCode::conflict,
                               "Model settings changed elsewhere. Reload before saving.");
    }
    auto credentials = credentials_document(credentials_path);
    Document next{shared ? shared->value : Json{{"stream", true}}, {}};
    std::string key;
    const SecretGuard erase_key(key);
    std::string previous_reference;
    bool same_endpoint = false;
    bool same_adapter = false;
    if (shared) {
        auto previous = validated_config(shared->value);
        const SecretGuard erase_previous(previous.api_key);
        previous_reference = reference(shared->value);
        same_adapter = previous.adapter == update.adapter;
        same_endpoint = same_adapter && normalized_url(previous.api_url) == endpoint;
        if (same_endpoint && !update.remove_api_key && !update.api_key) {
            key = previous_reference.empty()
                      ? previous.api_key
                      : stored_key(credentials.value, previous_reference, previous);
        }
    }
    if (update.api_key) {
        key = *update.api_key;
    }
    if (!same_endpoint || update.remove_api_key || update.api_key) {
        next.value.erase("api_key_env");
    }
    if (!same_adapter) {
        // EN: Protocol-specific capabilities cannot be carried to another API type.
        // ZH-CN: 协议专属的能力设置不能沿用到另一种 API 类型。
        next.value.erase("capabilities");
    }
    if (!same_endpoint) {
        next.value["provider"] = "custom";
    }
    next.value["adapter"] = model_adapter_name(update.adapter);
    next.value["endpoint"] = endpoint;
    next.value.erase("api_url");
    next.value["model"] = update.model;
    remove_inline_key(next.value);
    next.value.erase("credential_ref");
    (void)validated_config(next.value);
    commit(config_path, credentials_path, next.value, credentials.value, key, previous_reference);
    return committed_snapshot(config_path, credentials.value);
}

} // namespace mint::model_settings_detail
