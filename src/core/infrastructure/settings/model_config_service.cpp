#include "mint/infrastructure/model_config_service.hpp"

#include "mint/infrastructure/project_store.hpp"

#include "model_settings_credentials.hpp"
#include "model_settings_profile.hpp"
#include "model_settings_transactions.hpp"

namespace mint {
namespace {

using model_settings_detail::credentials_document;
using model_settings_detail::Document;
using model_settings_detail::FileLock;
using model_settings_detail::invalid_input;
using model_settings_detail::migrate_locked;
using model_settings_detail::normalize_update;
using model_settings_detail::override_config;
using model_settings_detail::read_document;
using model_settings_detail::redacted_snapshot;
using model_settings_detail::reference;
using model_settings_detail::SecretGuard;
using model_settings_detail::selected_key;
using model_settings_detail::update_locked;
using model_settings_detail::validated_config;

std::string override_shared_revision(const ModelConfigService& service) {
    try {
        // EN: Only a revision is used; do not hydrate credentials or alter Windows ACLs.
        // ZH-CN: 仅读取版本号；不加载凭据，也不更改 Windows 文件 ACL。
        const auto shared = read_document(service.config_path(), false);
        return shared ? shared->revision : std::string{};
    } catch (const ModelConfigError&) {
        // EN: Missing or unavailable shared storage must not disable a standalone profile.
        // ZH-CN: 缺失或不可用的共享存储不能禁用独立配置。
        return {};
    }
}

} // namespace

ModelConfigService::ModelConfigService(std::filesystem::path state_directory) {
    if (state_directory.empty()) {
        return;
    }
    try {
        // EN: Keep symlink / '..' semantics until the filesystem boundary resolves the path.
        // ZH-CN: 保留符号链接与 '..' 的语义，由文件系统边界解析实际路径。
        state_directory_ = std::filesystem::absolute(std::move(state_directory));
    } catch (...) {
        throw ModelConfigError(ModelConfigErrorCode::unavailable,
                               "The model settings directory is unavailable.");
    }
}

ModelConfigService::StoragePaths ModelConfigService::storage_paths() const {
    try {
        const auto directory = state_directory_.empty()
                                   ? std::filesystem::absolute(default_mint_state_directory())
                                   : state_directory_;
        return {.directory = directory,
                .config = directory / "model.json",
                .credentials = directory / "credentials.json"};
    } catch (...) {
        throw ModelConfigError(ModelConfigErrorCode::unavailable,
                               "The model settings directory is unavailable.");
    }
}

std::filesystem::path ModelConfigService::config_path() const {
    return storage_paths().config;
}

void ModelConfigService::prepare_storage() const {
    const FileLock lock(storage_paths().directory);
}

std::filesystem::path ModelConfigService::credentials_path() const {
    return storage_paths().credentials;
}

ModelConfigSnapshot ModelConfigService::inspect(const std::filesystem::path& override_path) const {
    if (!override_path.empty()) {
        const auto selected = read_document(override_path, false);
        if (!selected) {
            throw ModelConfigError(ModelConfigErrorCode::unavailable,
                                   "The selected model configuration is unavailable.");
        }
        auto config = override_config(selected->value);
        const auto saved = !config.api_key.empty();
        const SecretGuard erase(config.api_key);
        return redacted_snapshot(std::move(config), override_path, override_shared_revision(*this),
                                 true, saved);
    }
    const auto paths = storage_paths();
    const FileLock lock(paths.directory);
    const auto shared = read_document(paths.config, true);
    if (!shared) {
        return {.path = paths.config};
    }
    auto config = validated_config(shared->value);
    const SecretGuard erase(config.api_key);
    std::optional<Document> credentials;
    if (!reference(shared->value).empty()) {
        credentials = credentials_document(paths.credentials);
    }
    const Json no_credentials;
    const auto& values = credentials ? credentials->value : no_credentials;
    const bool saved = !selected_key(shared->value, config, values).empty();
    return redacted_snapshot(std::move(config), paths.config, shared->revision, false, saved);
}

ModelConfigSnapshot ModelConfigService::update(ModelConfigUpdate update) {
    const SecretGuard erase(update.api_key ? &*update.api_key : nullptr);
    auto endpoint = normalize_update(update);
    const auto paths = storage_paths();
    const FileLock lock(paths.directory);
    return update_locked(paths.config, paths.credentials, std::move(update), std::move(endpoint));
}

ModelConfigSnapshot
ModelConfigService::migrate_legacy(const std::filesystem::path& legacy_path,
                                   std::optional<std::string> expected_revision) {
    if (legacy_path.empty() || (expected_revision && expected_revision->empty())) {
        invalid_input();
    }
    const auto paths = storage_paths();
    const FileLock lock(paths.directory);
    return migrate_locked(paths.config, paths.credentials, legacy_path, expected_revision);
}

ModelProviderConfig ModelConfigService::load_runtime(const std::filesystem::path& override_path,
                                                     ModelConfigSnapshot* inspected) const {
    if (!override_path.empty()) {
        const auto selected = read_document(override_path, false);
        if (!selected) {
            throw ModelConfigError(ModelConfigErrorCode::unavailable,
                                   "The selected model configuration is unavailable.");
        }
        auto config = override_config(selected->value);
        SecretGuard erase(config.api_key);
        if (inspected != nullptr) {
            *inspected = redacted_snapshot(config, override_path, override_shared_revision(*this),
                                           true, !config.api_key.empty());
        }
        erase.release();
        return config;
    }
    const auto paths = storage_paths();
    const FileLock lock(paths.directory);
    const auto selected = read_document(paths.config, true);
    if (!selected) {
        throw ModelConfigError(ModelConfigErrorCode::unavailable,
                               "No model is configured. Save model settings or use mint config "
                               "import --config path.");
    }
    auto config = validated_config(selected->value);
    SecretGuard erase(config.api_key);
    const auto key_reference = reference(selected->value);
    std::optional<Document> credentials;
    if (!key_reference.empty()) {
        credentials = credentials_document(paths.credentials);
    }
    if (!key_reference.empty()) {
        config.api_key = selected_key(selected->value, config, credentials->value);
    }
    if (inspected != nullptr) {
        *inspected = redacted_snapshot(config, paths.config, selected->revision, false,
                                       !config.api_key.empty());
    }
    erase.release();
    return config;
}

} // namespace mint
