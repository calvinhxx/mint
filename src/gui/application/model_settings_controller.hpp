#pragma once

#include "application/model_settings_runtime.hpp"

#include <functional>
#include <optional>

namespace mint::desktop {

struct ModelSettingsOptions {
    std::filesystem::path root;
    std::filesystem::path config;
    std::string language;
};

using CustomModelSettingsRequest = ModelUpdateRequest;
enum class ModelSettingsProblem { none, inspection_failed, save_failed, invalid_input, conflict };
enum class SavedCredentialState { unknown, missing, ready, unavailable, error };

struct ModelSettingsState {
    std::optional<ProviderDescriptor> provider;
    std::filesystem::path selected_config;
    ModelSettingsProblem problem = ModelSettingsProblem::none;
    SavedCredentialState saved_credential = SavedCredentialState::unknown;
    bool loading = false;
    bool saving = false;
    std::size_t save_generation = 0;
    std::optional<LegacyModelConflict> legacy_conflict;

    [[nodiscard]] bool can_start_task() const noexcept {
        return provider.has_value() && !loading && !saving && !legacy_conflict &&
               problem == ModelSettingsProblem::none;
    }
};

class ModelSettingsController final {
  public:
    using Listener = std::function<void()>;
    ModelSettingsController(ModelSettingsRuntime& runtime, ModelSettingsOptions options);
    ~ModelSettingsController();
    ModelSettingsController(const ModelSettingsController&) = delete;
    ModelSettingsController& operator=(const ModelSettingsController&) = delete;
    void set_listener(Listener listener);
    void set_saved_listener(Listener listener);
    [[nodiscard]] const ModelSettingsState& state() const noexcept;
    void activate();
    void refresh();
    void set_root(std::filesystem::path root);
    void set_config(std::filesystem::path config);
    void set_language(std::string language);
    [[nodiscard]] bool save_custom_profile(CustomModelSettingsRequest request);
    [[nodiscard]] bool remove_api_key(std::string expected_revision);
    [[nodiscard]] bool resolve_legacy_conflict(LegacyModelChoice choice);

  private:
    void inspect(bool foreground);
    void notify() const;
    void accept_provider(ProviderDescriptor provider, bool saved);
    void accept_error(ModelSettingsFailure failure);
    void accept_legacy_conflict(LegacyModelConflict conflict);
    ModelSettingsRuntime& runtime_;
    ModelSettingsOptions options_;
    ModelSettingsState state_;
    Listener listener_;
    Listener saved_listener_;
};

} // namespace mint::desktop
