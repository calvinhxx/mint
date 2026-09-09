#pragma once

#include "application/preferences/gui_preferences.hpp"

#include <QColor>
#include <QString>

namespace mint::ui::theme {

enum class Scheme { light, dark };

struct Palette {
    QColor canvas;
    QColor chrome;
    QColor panel;
    QColor selected;
    QColor navigation_selected;
    QColor raised;
    QColor row;
    QColor stroke;
    QColor stroke_strong;
    QColor text_primary;
    QColor text_secondary;
    QColor text_tertiary;
    QColor text_disabled;
    QColor action;
    QColor action_secondary;
    QColor action_tertiary;
    QColor action_disabled;
    QColor action_text;
    QColor control_disabled;
    QColor subtle_hover;
    QColor subtle_pressed;
    QColor link;
    QColor attention;
    QColor attention_background;
    QColor success;
    QColor success_background;
    QColor critical;
    QColor critical_background;
    QColor info;
    QColor info_background;
};

[[nodiscard]] const Palette& palette();
[[nodiscard]] const Palette& palette(Scheme scheme);
[[nodiscard]] Scheme resolved_scheme(desktop::ThemeMode mode);
[[nodiscard]] QString shell_stylesheet(Scheme scheme);
[[nodiscard]] QString markdown_stylesheet();
void install(desktop::ThemeMode mode);
void apply(desktop::ThemeMode mode);

} // namespace mint::ui::theme
