#include "model_settings_controller.hpp"

#include <algorithm>
#include <utility>

namespace mint::desktop {

ModelSettingsController::ModelSettingsController(ModelSettingsRuntime& runtime,
                                                 ModelSettingsOptions options)
    : runtime_(runtime), options_(std::move(options)) {
    state_.selected_config = options_.config;
    runtime_.set_model_settings_callbacks(
        {.inspected =
             [this](ProviderDescriptor provider) { accept_provider(std::move(provider), false); },
         .saved =
             [this](ProviderDescriptor provider) { accept_provider(std::move(provider), true); },
         .error = [this](ModelSettingsFailure failure) { accept_error(failure); },
         .changed = [this] { inspect(false); },
         .migration_conflict =
             [this](LegacyModelConflict conflict) {
                 accept_legacy_conflict(std::move(conflict));
             }});
}

ModelSettingsController::~ModelSettingsController() {
    runtime_.set_model_settings_callbacks({});
}
void ModelSettingsController::set_listener(Listener listener) {
    listener_ = std::move(listener);
}
void ModelSettingsController::set_saved_listener(Listener listener) {
    saved_listener_ = std::move(listener);
}
const ModelSettingsState& ModelSettingsController::state() const noexcept {
    return state_;
}
void ModelSettingsController::activate() {
    refresh();
}
void ModelSettingsController::refresh() {
    inspect(true);
}

void ModelSettingsController::inspect(bool foreground) {
    if (state_.saving) {
        return;
    }
    if (foreground) {
        state_.loading = true;
        state_.problem = ModelSettingsProblem::none;
        notify();
    }
    runtime_.inspect_model(
        {.root = options_.root, .config = options_.config, .language = options_.language});
}

void ModelSettingsController::set_root(std::filesystem::path root) {
    if (root.empty() || root == options_.root) {
        return;
    }
    options_.root = std::move(root);
    // EN: Shared configuration is independent of the active workspace.
    // ZH-CN: 共享配置不依赖当前工作区。
    if (!options_.config.empty() && options_.config.is_relative()) {
        refresh();
    }
}

void ModelSettingsController::set_config(std::filesystem::path config) {
    config = config.lexically_normal();
    if (config == ".") {
        config.clear();
    }
    if (config == options_.config) {
        return;
    }
    options_.config = std::move(config);
    state_.selected_config = options_.config;
    refresh();
}

void ModelSettingsController::set_language(std::string language) {
    options_.language = std::move(language);
}

bool ModelSettingsController::save_custom_profile(CustomModelSettingsRequest request) {
    if (state_.saving || state_.loading || state_.legacy_conflict) {
        if (request.api_key) {
            std::fill(request.api_key->begin(), request.api_key->end(), '\0');
        }
        return false;
    }
    state_.saving = true;
    state_.problem = ModelSettingsProblem::none;
    notify();
    runtime_.update_model(std::move(request));
    return true;
}

bool ModelSettingsController::remove_api_key(std::string expected_revision) {
    if (!state_.provider || state_.saved_credential != SavedCredentialState::ready) {
        return false;
    }
    const auto& provider = *state_.provider;
    auto adapter = CustomModelAdapter::chat_completions;
    if (provider.adapter == "responses") {
        adapter = CustomModelAdapter::responses;
    }
    if (provider.adapter == "anthropic_messages") {
        adapter = CustomModelAdapter::anthropic_messages;
    }
    return save_custom_profile({.expected_revision = std::move(expected_revision),
                                .adapter = adapter,
                                .base_url = provider.endpoint,
                                .model = provider.model,
                                .remove_api_key = true});
}

bool ModelSettingsController::resolve_legacy_conflict(LegacyModelChoice choice) {
    if (!state_.legacy_conflict || state_.saving || state_.loading ||
        (choice == LegacyModelChoice::import_legacy && !state_.legacy_conflict->legacy)) {
        return false;
    }
    state_.saving = true;
    state_.problem = ModelSettingsProblem::none;
    notify();
    runtime_.resolve_legacy_conflict(
        {.choice = choice, .expected_revision = state_.legacy_conflict->shared.revision});
    return true;
}

void ModelSettingsController::notify() const {
    if (listener_) {
        listener_();
    }
}

void ModelSettingsController::accept_provider(ProviderDescriptor provider, bool saved) {
    state_.provider = std::move(provider);
    state_.loading = false;
    state_.legacy_conflict.reset();
    if (saved) {
        options_.config.clear();
        state_.selected_config.clear();
        state_.saving = false;
        state_.problem = ModelSettingsProblem::none;
        ++state_.save_generation;
    } else if (state_.problem != ModelSettingsProblem::conflict &&
               state_.problem != ModelSettingsProblem::save_failed &&
               state_.problem != ModelSettingsProblem::invalid_input) {
        state_.problem = ModelSettingsProblem::none;
    }
    state_.saved_credential = state_.provider->saved_api_key ? SavedCredentialState::ready
                                                             : SavedCredentialState::missing;
    if (saved && saved_listener_) {
        saved_listener_();
    }
    notify();
}

void ModelSettingsController::accept_legacy_conflict(LegacyModelConflict conflict) {
    state_.provider = conflict.shared;
    state_.legacy_conflict = std::move(conflict);
    state_.loading = false;
    state_.saving = false;
    state_.problem = ModelSettingsProblem::none;
    state_.saved_credential = state_.provider->saved_api_key ? SavedCredentialState::ready
                                                             : SavedCredentialState::missing;
    notify();
}

void ModelSettingsController::accept_error(ModelSettingsFailure failure) {
    const auto saving = state_.saving;
    state_.loading = false;
    state_.saving = false;
    state_.problem = failure == ModelSettingsFailure::conflict ? ModelSettingsProblem::conflict
                     : failure == ModelSettingsFailure::invalid_input
                         ? ModelSettingsProblem::invalid_input
                     : saving ? ModelSettingsProblem::save_failed
                              : ModelSettingsProblem::inspection_failed;
    // EN: Retain the last snapshot and draft on a transient error or conflict.
    // ZH-CN: 临时错误或冲突不会丢弃现有快照和草稿。
    notify();
}

} // namespace mint::desktop
