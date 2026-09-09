#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace mint::desktop {

enum class CredentialSource { none, environment, inline_key };
enum class CustomModelAdapter { chat_completions, responses, anthropic_messages };

inline constexpr std::array custom_model_adapter_allowlist{CustomModelAdapter::chat_completions,
                                                           CustomModelAdapter::responses,
                                                           CustomModelAdapter::anthropic_messages};

[[nodiscard]] constexpr std::string_view
custom_model_adapter_code(CustomModelAdapter adapter) noexcept {
    switch (adapter) {
    case CustomModelAdapter::chat_completions:
        return "chat_completions";
    case CustomModelAdapter::responses:
        return "responses";
    case CustomModelAdapter::anthropic_messages:
        return "anthropic_messages";
    }
    return {};
}

struct ProviderDescriptor {
    std::string provider;
    std::string model;
    std::string adapter;
    std::string endpoint;
    std::string credential_scope;
    std::filesystem::path config;
    CredentialSource credential_source = CredentialSource::none;
    std::string api_key_environment;
    bool credential_available = false;
    std::string revision;
    bool configured = true;
    bool saved_api_key = false;
    bool is_override = false;
};

struct ModelInspectionRequest {
    std::filesystem::path root;
    std::filesystem::path config;
    std::string language;
};

struct ModelUpdateRequest {
    std::string expected_revision;
    CustomModelAdapter adapter = CustomModelAdapter::chat_completions;
    std::string base_url;
    std::string model;
    std::optional<std::string> api_key;
    bool remove_api_key = false;
};

enum class ModelSettingsFailure { invalid_input, conflict, unavailable };

struct LegacyModelConflict {
    ProviderDescriptor shared;
    std::optional<ProviderDescriptor> legacy;
    std::filesystem::path legacy_config;
};

enum class LegacyModelChoice { import_legacy, keep_shared };

struct LegacyModelResolution {
    LegacyModelChoice choice = LegacyModelChoice::keep_shared;
    std::string expected_revision;
};

struct ModelSettingsCallbacks {
    std::function<void(ProviderDescriptor)> inspected;
    std::function<void(ProviderDescriptor)> saved;
    std::function<void(ModelSettingsFailure)> error;
    std::function<void()> changed;
    std::function<void(LegacyModelConflict)> migration_conflict;
};

class ModelSettingsRuntime {
  public:
    virtual ~ModelSettingsRuntime() = default;
    virtual void set_model_settings_callbacks(ModelSettingsCallbacks callbacks) = 0;
    virtual void inspect_model(ModelInspectionRequest request) = 0;
    virtual void update_model(ModelUpdateRequest request) = 0;
    virtual void resolve_legacy_conflict(LegacyModelResolution request) = 0;
};

} // namespace mint::desktop
