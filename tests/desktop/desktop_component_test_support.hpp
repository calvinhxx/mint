#pragma once

#include "application/agent_runtime.hpp"
#include "application/model_settings_runtime.hpp"
#include "application/preferences/preferences_repository.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace mint::test {

class ComponentRuntime final : public mint::desktop::AgentRuntime,
                               public mint::desktop::ModelSettingsRuntime {
  public:
    void set_callbacks(mint::desktop::AgentRuntimeCallbacks value) override {
        callbacks = std::move(value);
    }
    void refresh_tasks(const std::filesystem::path&, const std::string&) override {}
    void load_task(const std::string& task_id) override {
        loaded_task = task_id;
        ++load_count;
    }
    void start_task(mint::desktop::RunRequest request) override {
        last_run_request = std::move(request);
        ++run_count;
    }
    void resume_task(mint::desktop::ResumeRequest request) override {
        last_resume_request = std::move(request);
        ++resume_count;
    }
    [[nodiscard]] bool resolve_approval(bool) override {
        return true;
    }
    void cancel() override {}
    [[nodiscard]] bool running() const noexcept override {
        return false;
    }

    void set_model_settings_callbacks(mint::desktop::ModelSettingsCallbacks value) override {
        model_settings_callbacks = std::move(value);
    }
    void inspect_model(mint::desktop::ModelInspectionRequest request) override {
        ++inspection_count;
        last_inspection = std::move(request);
        if (model_settings_callbacks.inspected) {
            auto provider = mint::desktop::ProviderDescriptor{
                .provider = "deepseek",
                .model = "deepseek-v4-flash",
                .adapter = "chat_completions",
                .credential_scope = credential_scope,
                .config = last_inspection.config,
                .credential_source = configured_credential_source,
                .credential_available = configured_credential_available,
                .revision = "fixture-revision"};
            if (const auto configured = providers.find(last_inspection.config.generic_string());
                configured != providers.end()) {
                provider = configured->second;
                provider.config = last_inspection.config;
            }
            model_settings_callbacks.inspected(std::move(provider));
        }
    }
    void update_model(mint::desktop::ModelUpdateRequest request) override {
        last_update = std::move(request);
        if (defer_model_update) {
            return;
        }
        if (next_failure) {
            model_settings_callbacks.error(*next_failure);
            return;
        }
        if (model_settings_callbacks.saved) {
            model_settings_callbacks.saved(
                {.provider = "custom",
                 .model = last_update.model,
                 .adapter =
                     std::string(mint::desktop::custom_model_adapter_code(last_update.adapter)),
                 .endpoint = last_update.base_url,
                 .config = "/state/model.json",
                 .revision = "saved-revision",
                 .saved_api_key = !last_update.remove_api_key});
        }
    }
    void resolve_legacy_conflict(mint::desktop::LegacyModelResolution request) override {
        last_resolution = std::move(request);
    }

    mint::desktop::AgentRuntimeCallbacks callbacks;
    mint::desktop::ModelSettingsCallbacks model_settings_callbacks;
    mint::desktop::ModelInspectionRequest last_inspection;
    int inspection_count = 0;
    mint::desktop::RunRequest last_run_request;
    mint::desktop::ResumeRequest last_resume_request;
    mint::desktop::LegacyModelResolution last_resolution;
    int run_count = 0;
    int resume_count = 0;
    bool defer_model_update = false;
    std::string loaded_task;
    int load_count = 0;
    std::unordered_map<std::string, mint::desktop::ProviderDescriptor> providers;
    mint::desktop::ModelUpdateRequest last_update;
    std::optional<mint::desktop::ModelSettingsFailure> next_failure;
    mint::desktop::CredentialSource configured_credential_source =
        mint::desktop::CredentialSource::inline_key;
    bool configured_credential_available = true;
    std::string credential_scope = "v1:deepseek:https://api.deepseek.com/chat/completions";
};

class MemoryPreferencesRepository final : public mint::desktop::PreferencesRepository {
  public:
    [[nodiscard]] std::optional<mint::desktop::GuiPreferences> load() const override {
        return saved;
    }

    bool save(const mint::desktop::GuiPreferences& preferences) override {
        saved = preferences;
        return save_succeeds;
    }

    std::optional<mint::desktop::GuiPreferences> saved;
    bool save_succeeds = true;
};

} // namespace mint::test
