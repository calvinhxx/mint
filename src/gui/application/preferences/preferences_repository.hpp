#pragma once

#include "application/preferences/gui_preferences.hpp"

#include <optional>

namespace mint::desktop {

class PreferencesRepository {
  public:
    virtual ~PreferencesRepository() = default;

    [[nodiscard]] virtual std::optional<GuiPreferences> load() const = 0;
    [[nodiscard]] virtual bool save(const GuiPreferences& preferences) = 0;
};

} // namespace mint::desktop
