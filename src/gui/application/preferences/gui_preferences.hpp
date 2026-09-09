#pragma once

#include "mint/localization/localization.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mint::desktop {

enum class ThemeMode { system, light, dark };

[[nodiscard]] constexpr std::string_view theme_mode_code(ThemeMode mode) noexcept {
    switch (mode) {
    case ThemeMode::system:
        return "system";
    case ThemeMode::light:
        return "light";
    case ThemeMode::dark:
        return "dark";
    }
    return "system";
}

[[nodiscard]] constexpr std::optional<ThemeMode>
theme_mode_from_code(std::string_view code) noexcept {
    if (code == "system") {
        return ThemeMode::system;
    }
    if (code == "light") {
        return ThemeMode::light;
    }
    if (code == "dark") {
        return ThemeMode::dark;
    }
    return std::nullopt;
}

struct GuiPreferences {
    localization::Language language = localization::Language::english;
    ThemeMode theme = ThemeMode::system;
};

} // namespace mint::desktop
