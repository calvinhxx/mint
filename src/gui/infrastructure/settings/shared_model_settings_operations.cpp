#include "shared_model_settings_operations.hpp"

#include "mint/infrastructure/model_config_service.hpp"

namespace mint::infrastructure::desktop {
namespace {

mint::desktop::ProviderDescriptor describe(const ModelConfigSnapshot& snapshot) {
    return {.provider = snapshot.configured
                            ? std::string(model_provider_name(
                                  resolve_model_provider_profile(snapshot.config).provider))
                            : std::string{},
            .model = snapshot.configured ? snapshot.config.model : std::string{},
            .adapter = std::string(model_adapter_name(snapshot.config.adapter)),
            .endpoint = snapshot.configured ? snapshot.config.api_url : std::string{},
            .config = snapshot.path,
            .credential_available = snapshot.credential_available,
            .revision = snapshot.revision,
            .configured = snapshot.configured,
            .saved_api_key = snapshot.saved_api_key,
            .is_override = snapshot.is_override};
}

mint::desktop::LegacyModelConflict describe_conflict(ModelConfigService& service,
                                                     const ModelConfigSnapshot& shared,
                                                     const std::filesystem::path& legacy) {
    mint::desktop::LegacyModelConflict result{.shared = describe(shared), .legacy_config = legacy};
    try {
        result.legacy = describe(service.inspect(legacy));
    } catch (...) {
        // EN: An unavailable old source still requires an explicit choice to keep shared settings.
        // ZH-CN: 旧来源不可用时，仍须明确选择保留共享配置。
    }
    return result;
}

ModelAdapter adapter_from(mint::desktop::CustomModelAdapter adapter) {
    switch (adapter) {
    case mint::desktop::CustomModelAdapter::chat_completions:
        return ModelAdapter::chat_completions;
    case mint::desktop::CustomModelAdapter::responses:
        return ModelAdapter::responses;
    case mint::desktop::CustomModelAdapter::anthropic_messages:
        return ModelAdapter::anthropic_messages;
    }
    throw ModelConfigError(ModelConfigErrorCode::invalid_input,
                           "The selected API type is invalid.");
}

} // namespace

ModelSettingsOperation::~ModelSettingsOperation() {
    clear_api_key();
}

void ModelSettingsOperation::clear_api_key() noexcept {
    if (auto* update = std::get_if<mint::desktop::ModelUpdateRequest>(&request);
        update != nullptr && update->api_key) {
        volatile char* bytes = update->api_key->data();
        for (std::size_t index = 0; index < update->api_key->size(); ++index) {
            bytes[index] = '\0';
        }
        update->api_key.reset();
    }
}

bool ModelSettingsOperation::is_mutation() const noexcept {
    return !std::holds_alternative<mint::desktop::ModelInspectionRequest>(request);
}

ModelSettingsOperationResult
execute_model_settings_operation(const std::filesystem::path& state_directory,
                                 const std::filesystem::path& legacy,
                                 ModelSettingsOperation& operation) {
    struct ClearRequest {
        ModelSettingsOperation& operation;
        ~ClearRequest() {
            operation.clear_api_key();
        }
    } clear_request{operation};
    ModelSettingsOperationResult result;
    try {
        ModelConfigService service(state_directory);
        try {
            result.shared_config = service.config_path();
        } catch (const ModelConfigError&) {
            const auto* inspection =
                std::get_if<mint::desktop::ModelInspectionRequest>(&operation.request);
            if (inspection == nullptr || inspection->config.empty()) {
                throw;
            }
        }
        if (auto* update = std::get_if<mint::desktop::ModelUpdateRequest>(&operation.request)) {
            result.provider =
                describe(service.update({.expected_revision = std::move(update->expected_revision),
                                         .adapter = adapter_from(update->adapter),
                                         .base_url = std::move(update->base_url),
                                         .model = std::move(update->model),
                                         .api_key = std::move(update->api_key),
                                         .remove_api_key = update->remove_api_key}));
            result.migrated = !legacy.empty();
        } else if (auto* resolution =
                       std::get_if<mint::desktop::LegacyModelResolution>(&operation.request)) {
            if (legacy.empty() || resolution->expected_revision.empty()) {
                throw ModelConfigError(ModelConfigErrorCode::invalid_input,
                                       "No previous GUI configuration is selected.");
            }
            if (resolution->choice == mint::desktop::LegacyModelChoice::import_legacy) {
                result.provider =
                    describe(service.migrate_legacy(legacy, resolution->expected_revision));
            } else if (resolution->choice == mint::desktop::LegacyModelChoice::keep_shared) {
                const auto shared = service.inspect();
                if (!shared.configured || shared.revision != resolution->expected_revision) {
                    throw ModelConfigError(ModelConfigErrorCode::conflict,
                                           "Shared model settings changed before selection.");
                }
                result.provider = describe(shared);
            } else {
                throw ModelConfigError(ModelConfigErrorCode::invalid_input,
                                       "The model configuration selection is invalid.");
            }
            result.migrated = true;
        } else {
            const auto& inspection =
                std::get<mint::desktop::ModelInspectionRequest>(operation.request);
            auto config = inspection.config;
            if (config.empty()) {
                auto snapshot = service.inspect();
                if (!legacy.empty()) {
                    // EN: Preserve the old explicit choice until import or user resolution.
                    //     Never discover workspace files or choose a profile by modification
                    //     time.
                    // ZH-CN:
                    // 导入或用户明确选择前保留旧入口；不发现工作区文件，也不按修改时间猜配置。
                    if (!snapshot.configured) {
                        try {
                            snapshot = service.migrate_legacy(legacy);
                            result.migrated = true;
                        } catch (const ModelConfigError& error) {
                            if (error.code() != ModelConfigErrorCode::conflict) {
                                throw;
                            }
                            snapshot = service.inspect();
                            if (!snapshot.configured) {
                                throw;
                            }
                        }
                    }
                    if (!result.migrated) {
                        result.conflict = describe_conflict(service, snapshot, legacy);
                    }
                }
                result.provider = describe(snapshot);
            } else {
                if (config.is_relative()) {
                    config = inspection.root / config;
                }
                result.provider = describe(service.inspect(config));
            }
        }
    } catch (const ModelConfigError& error) {
        result.failure = error.code() == ModelConfigErrorCode::conflict
                             ? mint::desktop::ModelSettingsFailure::conflict
                         : error.code() == ModelConfigErrorCode::invalid_input
                             ? mint::desktop::ModelSettingsFailure::invalid_input
                             : mint::desktop::ModelSettingsFailure::unavailable;
    } catch (...) {
        // EN: Parser and filesystem diagnostics may contain sensitive source text.
        // ZH-CN: 解析器或文件系统诊断可能包含敏感原文。
        result.failure = mint::desktop::ModelSettingsFailure::unavailable;
    }
    return result;
}

} // namespace mint::infrastructure::desktop
