#include "preferences_controller.hpp"

#include <utility>

namespace mint::desktop {

PreferencesController::PreferencesController(PreferencesRepository& repository)
    : repository_(repository) {}

void PreferencesController::initialize(std::optional<localization::Language> language_override,
                                       std::optional<ThemeMode> theme_override) {
    localization::use_environment_language();
    auto initial = repository_.load().value_or(
        GuiPreferences{.language = localization::current_language(), .theme = ThemeMode::system});
    if (language_override.has_value()) {
        initial.language = *language_override;
    }
    if (theme_override.has_value()) {
        initial.theme = *theme_override;
    }
    (void)apply(initial, false);
}

void PreferencesController::set_listener(Listener listener) {
    listener_ = std::move(listener);
}

bool PreferencesController::set_language(localization::Language language) {
    auto next = preferences_;
    next.language = language;
    return apply(next, true);
}

bool PreferencesController::set_theme(ThemeMode theme) {
    auto next = preferences_;
    next.theme = theme;
    return apply(next, true);
}

const GuiPreferences& PreferencesController::preferences() const noexcept {
    return preferences_;
}

bool PreferencesController::apply(GuiPreferences preferences, bool persist) {
    const auto changed =
        preferences_.language != preferences.language || preferences_.theme != preferences.theme;
    preferences_ = preferences;
    localization::set_language(preferences_.language);
    const auto saved = !persist || repository_.save(preferences_);
    if (changed && listener_) {
        listener_(preferences_);
    }
    return saved;
}

} // namespace mint::desktop
