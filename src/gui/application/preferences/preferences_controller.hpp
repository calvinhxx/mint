#pragma once

#include "application/preferences/preferences_repository.hpp"

#include <functional>
#include <optional>

namespace mint::desktop {

class PreferencesController final {
  public:
    using Listener = std::function<void(const GuiPreferences&)>;

    explicit PreferencesController(PreferencesRepository& repository);

    void initialize(std::optional<localization::Language> language_override = std::nullopt,
                    std::optional<ThemeMode> theme_override = std::nullopt);
    void set_listener(Listener listener);
    [[nodiscard]] bool set_language(localization::Language language);
    [[nodiscard]] bool set_theme(ThemeMode theme);

    [[nodiscard]] const GuiPreferences& preferences() const noexcept;

  private:
    [[nodiscard]] bool apply(GuiPreferences preferences, bool persist);

    PreferencesRepository& repository_;
    GuiPreferences preferences_;
    Listener listener_;
};

} // namespace mint::desktop
