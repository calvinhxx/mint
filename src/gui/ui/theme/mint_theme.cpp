#include "mint_theme.hpp"

#include <FluentQt/Foundation.h>

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>

namespace mint::ui::theme {
namespace {

const Palette& light_palette() {
    static const Palette value{.canvas = QColor(QStringLiteral("#FFFFFF")),
                               .chrome = QColor(QStringLiteral("#F4F4F2")),
                               .panel = QColor(QStringLiteral("#F7F7F7")),
                               .selected = QColor(QStringLiteral("#EAEAEA")),
                               .navigation_selected = QColor(QStringLiteral("#E1E1DE")),
                               .raised = QColor(QStringLiteral("#F2F2F2")),
                               .row = QColor(QStringLiteral("#ECECEC")),
                               .stroke = QColor(QStringLiteral("#E0E0E0")),
                               .stroke_strong = QColor(QStringLiteral("#A8A8A8")),
                               .text_primary = QColor(QStringLiteral("#0D0D0D")),
                               .text_secondary = QColor(QStringLiteral("#565656")),
                               .text_tertiary = QColor(QStringLiteral("#6F6F6F")),
                               .text_disabled = QColor(QStringLiteral("#A0A0A0")),
                               .action = QColor(QStringLiteral("#0D0D0D")),
                               .action_secondary = QColor(QStringLiteral("#333333")),
                               .action_tertiary = QColor(QStringLiteral("#666666")),
                               .action_disabled = QColor(QStringLiteral("#A3A3A3")),
                               .action_text = QColor(QStringLiteral("#FFFFFF")),
                               .control_disabled = QColor(QStringLiteral("#F0F0F0")),
                               .subtle_hover = QColor(QStringLiteral("#F2F2F2")),
                               .subtle_pressed = QColor(QStringLiteral("#E8E8E8")),
                               .link = QColor(QStringLiteral("#0169CC")),
                               .attention = QColor(QStringLiteral("#8A6815")),
                               .attention_background = QColor(QStringLiteral("#FFF4D2")),
                               .success = QColor(QStringLiteral("#566270")),
                               .success_background = QColor(QStringLiteral("#ECEFF2")),
                               .critical = QColor(QStringLiteral("#B4233A")),
                               .critical_background = QColor(QStringLiteral("#FCE8EB")),
                               .info = QColor(QStringLiteral("#0169CC")),
                               .info_background = QColor(QStringLiteral("#E5F1FC"))};
    return value;
}

const Palette& dark_palette() {
    static const Palette value{.canvas = QColor(QStringLiteral("#181818")),
                               .chrome = QColor(QStringLiteral("#232321")),
                               .panel = QColor(QStringLiteral("#212121")),
                               .selected = QColor(QStringLiteral("#2F2F2F")),
                               .navigation_selected = QColor(QStringLiteral("#292927")),
                               .raised = QColor(QStringLiteral("#262626")),
                               .row = QColor(QStringLiteral("#2A2A2A")),
                               .stroke = QColor(QStringLiteral("#353535")),
                               .stroke_strong = QColor(QStringLiteral("#5E5E5E")),
                               .text_primary = QColor(QStringLiteral("#FFFFFF")),
                               .text_secondary = QColor(QStringLiteral("#C6C6C6")),
                               .text_tertiary = QColor(QStringLiteral("#929292")),
                               .text_disabled = QColor(QStringLiteral("#747474")),
                               .action = QColor(QStringLiteral("#FFFFFF")),
                               .action_secondary = QColor(QStringLiteral("#D7D7D7")),
                               .action_tertiary = QColor(QStringLiteral("#AFAFAF")),
                               .action_disabled = QColor(QStringLiteral("#6E6E6E")),
                               .action_text = QColor(QStringLiteral("#0D0D0D")),
                               .control_disabled = QColor(QStringLiteral("#202020")),
                               .subtle_hover = QColor(QStringLiteral("#2A2A2A")),
                               .subtle_pressed = QColor(QStringLiteral("#323232")),
                               .link = QColor(QStringLiteral("#78B7FF")),
                               .attention = QColor(QStringLiteral("#D4B875")),
                               .attention_background = QColor(QStringLiteral("#332D20")),
                               .success = QColor(QStringLiteral("#AAB4BF")),
                               .success_background = QColor(QStringLiteral("#293038")),
                               .critical = QColor(QStringLiteral("#E18B98")),
                               .critical_background = QColor(QStringLiteral("#382329")),
                               .info = QColor(QStringLiteral("#8FC5FF")),
                               .info_background = QColor(QStringLiteral("#203044"))};
    return value;
}

void install_colors(fluent::FluentElement::Colors& colors, const Palette& source) {
    colors.accentDefault = source.action;
    colors.accentSecondary = source.action_secondary;
    colors.accentTertiary = source.action_tertiary;
    colors.accentDisabled = source.action_disabled;
    colors.controlDefault = source.selected;
    colors.controlSecondary = source.row;
    colors.controlTertiary = source.raised;
    colors.controlDisabled = source.control_disabled;
    colors.controlAltSecondary = source.raised;
    colors.controlAltTertiary = source.selected;
    colors.subtleTransparent = QColor(0, 0, 0, 0);
    colors.subtleSecondary = source.subtle_hover;
    colors.subtleTertiary = source.subtle_pressed;
    colors.strokeDefault = source.stroke;
    colors.strokeSecondary = source.stroke_strong;
    colors.strokeStrong = source.stroke_strong;
    colors.strokeCard = source.stroke;
    colors.strokeDivider = source.stroke;
    colors.strokeSurface = source.stroke_strong;
    colors.strokeFocusOuter = source.action;
    colors.strokeFocusInner = source.canvas;
    colors.textPrimary = source.text_primary;
    colors.textSecondary = source.text_secondary;
    colors.textTertiary = source.text_tertiary;
    colors.textDisabled = source.text_disabled;
    colors.textOnAccent = source.action_text;
    colors.textAccentPrimary = source.link;
    colors.bgCanvas = source.canvas;
    colors.bgLayer = source.panel;
    colors.bgLayerAlt = source.chrome;
    colors.bgSolid = source.canvas;
    colors.bgLayerOverlay = source.raised;
    colors.systemCritical = source.critical;
    colors.systemCriticalBg = source.critical_background;
    colors.systemCaution = source.attention;
    colors.systemCautionBg = source.attention_background;
    colors.systemInfo = source.info;
    colors.systemInfoBg = source.info_background;
    colors.systemSuccess = source.success;
    colors.systemSuccessBg = source.success_background;
}

QString color_name(const QColor& color) {
    return color.name(QColor::HexRgb);
}

void replace_color(QString& value, QStringView token, const QColor& color) {
    value.replace(token.toString(), color_name(color));
}

void apply_application_palette(const Palette& colors) {
    if (qApp == nullptr) {
        return;
    }
    QPalette application_palette;
    application_palette.setColor(QPalette::Window, colors.canvas);
    application_palette.setColor(QPalette::WindowText, colors.text_primary);
    application_palette.setColor(QPalette::Base, colors.panel);
    application_palette.setColor(QPalette::AlternateBase, colors.raised);
    application_palette.setColor(QPalette::Text, colors.text_primary);
    application_palette.setColor(QPalette::Button, colors.selected);
    application_palette.setColor(QPalette::ButtonText, colors.text_primary);
    application_palette.setColor(QPalette::Highlight, colors.action);
    application_palette.setColor(QPalette::HighlightedText, colors.action_text);
    application_palette.setColor(QPalette::Link, colors.link);
    application_palette.setColor(QPalette::PlaceholderText, colors.text_tertiary);
    application_palette.setColor(QPalette::Disabled, QPalette::Text, colors.text_disabled);
    application_palette.setColor(QPalette::Disabled, QPalette::WindowText, colors.text_disabled);
    application_palette.setColor(QPalette::Disabled, QPalette::ButtonText, colors.text_disabled);
    application_palette.setColor(QPalette::Disabled, QPalette::Button, colors.control_disabled);
    qApp->setPalette(application_palette);
}

} // namespace

const Palette& palette() {
    return fluent::FluentElement::currentTheme() == fluent::FluentElement::Light ? light_palette()
                                                                                 : dark_palette();
}

const Palette& palette(Scheme scheme) {
    return scheme == Scheme::light ? light_palette() : dark_palette();
}

Scheme resolved_scheme(desktop::ThemeMode mode) {
    if (mode == desktop::ThemeMode::light) {
        return Scheme::light;
    }
    if (mode == desktop::ThemeMode::dark) {
        return Scheme::dark;
    }
    if (QGuiApplication::instance() != nullptr) {
        const auto system_scheme = QGuiApplication::styleHints()->colorScheme();
        if (system_scheme == Qt::ColorScheme::Light) {
            return Scheme::light;
        }
        if (system_scheme == Qt::ColorScheme::Dark) {
            return Scheme::dark;
        }
        const auto background = QGuiApplication::palette().color(QPalette::Window);
        return background.lightnessF() >= 0.5 ? Scheme::light : Scheme::dark;
    }
    return Scheme::dark;
}

QString shell_stylesheet(Scheme scheme) {
    const auto& colors = palette(scheme);
    auto stylesheet = QStringLiteral(R"(
#MintRoot, #MintWorkspaceSplitView, #MintTaskSidebar { background: transparent; }
#MintSettingsScroll, #MintSettingsScroll > QWidget > QWidget, #MintSettingsContent {
    background: transparent;
    border: none;
}
#MintConversationCanvas { background: @CANVAS@; }
#MintTitleBarIdentity, #MintTitleBarTaskRegion { background: transparent; }
#MintConversationActionLayer, #MintComposerLayer { background: transparent; }
#MintRunConversation { background: transparent; }
#MintConversationTranscript { background: transparent; color: @TEXT_PRIMARY@; border: none; }
#MintConversationTranscript QWidget { background: transparent; }
#MintComposer { background: @PANEL@; border: 1px solid @STROKE@; border-radius: 14px; }
#MintComposer[focused="true"] { border-color: @ACTION@; }
#MintEvidencePanel { background: @PANEL@; border-left: 1px solid @STROKE_STRONG@; }
#MintApprovalCard { background: @RAISED@; border: 1px solid @STROKE_STRONG@; border-radius: 14px; }
#MintProjectTitle, #MintPrimaryText { color: @TEXT_PRIMARY@; }
#MintSecondaryText, #MintSettingsLanguageHint, #MintSettingsThemeHint { color: @TEXT_SECONDARY@; }
#MintTertiaryText { color: @TEXT_TERTIARY@; }
#MintAttentionText { color: @ATTENTION@; }
#MintSuccessText { color: @SUCCESS@; }
#MintCriticalText { color: @CRITICAL@; }
#MintApiKeyStatus { color: @TEXT_SECONDARY@; }
#MintApiKeyStatus[tone="attention"] { color: @ATTENTION@; }
#MintApiKeyStatus[tone="success"] { color: @SUCCESS@; }
#MintApiKeyStatus[tone="critical"] { color: @CRITICAL@; }
QScrollBar:vertical { background: @CHROME@; width: 8px; margin: 0; }
QScrollBar::handle:vertical { background: @STROKE_STRONG@; min-height: 32px; border-radius: 4px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
)");
    replace_color(stylesheet, u"@CANVAS@", colors.canvas);
    replace_color(stylesheet, u"@CHROME@", colors.chrome);
    replace_color(stylesheet, u"@PANEL@", colors.panel);
    replace_color(stylesheet, u"@RAISED@", colors.raised);
    replace_color(stylesheet, u"@STROKE@", colors.stroke);
    replace_color(stylesheet, u"@STROKE_STRONG@", colors.stroke_strong);
    replace_color(stylesheet, u"@TEXT_PRIMARY@", colors.text_primary);
    replace_color(stylesheet, u"@TEXT_SECONDARY@", colors.text_secondary);
    replace_color(stylesheet, u"@TEXT_TERTIARY@", colors.text_tertiary);
    replace_color(stylesheet, u"@ACTION@", colors.action);
    replace_color(stylesheet, u"@ATTENTION@", colors.attention);
    replace_color(stylesheet, u"@SUCCESS@", colors.success);
    replace_color(stylesheet, u"@CRITICAL@", colors.critical);
    return stylesheet;
}

QString markdown_stylesheet() {
    const auto& colors = palette();
    auto stylesheet = QStringLiteral(R"(
body { color: @TEXT_PRIMARY@; background-color: transparent; }
a { color: @LINK@; text-decoration: underline; }
code, pre { color: @TEXT_PRIMARY@; background-color: @PANEL@; font-family: Menlo, Consolas, monospace; }
blockquote { color: @TEXT_SECONDARY@; border-left: 2px solid @STROKE_STRONG@; margin-left: 0; padding-left: 10px; }
hr { color: @STROKE_STRONG@; }
)");
    replace_color(stylesheet, u"@TEXT_PRIMARY@", colors.text_primary);
    replace_color(stylesheet, u"@TEXT_SECONDARY@", colors.text_secondary);
    replace_color(stylesheet, u"@LINK@", colors.link);
    replace_color(stylesheet, u"@PANEL@", colors.panel);
    replace_color(stylesheet, u"@STROKE_STRONG@", colors.stroke_strong);
    return stylesheet;
}

void install(desktop::ThemeMode mode) {
    auto snapshot = fluent::ThemeRegistry::defaultSnapshot();
    install_colors(snapshot.lightColors, light_palette());
    install_colors(snapshot.darkColors, dark_palette());
    fluent::ThemeRegistry::instance().applySnapshot(snapshot);
    apply(mode);
}

void apply(desktop::ThemeMode mode) {
    const auto scheme = resolved_scheme(mode);
    const auto fluent_theme =
        scheme == Scheme::light ? fluent::FluentElement::Light : fluent::FluentElement::Dark;
    apply_application_palette(palette(scheme));
    if (qApp != nullptr) {
        qApp->setStyleSheet(shell_stylesheet(scheme));
    }
    // EN: Global stylesheet changes repolish inputs; restore their Fluent palettes last.
    // ZH-CN: 全局样式表会重新 polish 输入框，最后广播主题以恢复 Fluent 语义调色板。
    if (fluent::FluentElement::currentTheme() != fluent_theme) {
        fluent::FluentElement::setTheme(fluent_theme);
    } else {
        fluent::FluentElement::refreshTheme();
    }
}

} // namespace mint::ui::theme
