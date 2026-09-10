#include "qsettings_preferences_repository.hpp"

#include "support/qt_path.hpp"

#include <QSettings>
#include <QString>

namespace mint::infrastructure::desktop {
namespace {

const auto language_key = QStringLiteral("interface/language");
const auto theme_key = QStringLiteral("appearance/theme");
const auto model_config_key = QStringLiteral("model/config");

QString qstring_language_code(localization::Language language) {
    return QString::fromUtf8(localization::language_code(language));
}

std::optional<localization::Language> parse_language(const QString& value) {
    if (value == qstring_language_code(localization::Language::english)) {
        return localization::Language::english;
    }
    if (value == qstring_language_code(localization::Language::simplified_chinese)) {
        return localization::Language::simplified_chinese;
    }
    return std::nullopt;
}

QString qstring_theme_code(mint::desktop::ThemeMode theme) {
    const auto code = mint::desktop::theme_mode_code(theme);
    return QString::fromUtf8(code.data(), static_cast<qsizetype>(code.size()));
}

std::optional<mint::desktop::ThemeMode> parse_theme(const QString& value) {
    return mint::desktop::theme_mode_from_code(value.toStdString());
}

} // namespace

QSettingsPreferencesRepository::QSettingsPreferencesRepository()
    : settings_(std::make_unique<QSettings>()) {
    // EN: Appearance and legacy credential references belong to this application's state domain.
    // ZH-CN: 外观及旧凭据配置引用只属于当前应用的状态域。
    settings_->setFallbacksEnabled(false);
}

QSettingsPreferencesRepository::QSettingsPreferencesRepository(const QString& file_name)
    : settings_(std::make_unique<QSettings>(file_name, QSettings::IniFormat)) {
    settings_->setFallbacksEnabled(false);
}

QSettingsPreferencesRepository::~QSettingsPreferencesRepository() = default;

std::optional<mint::desktop::GuiPreferences> QSettingsPreferencesRepository::load() const {
    const auto language = parse_language(settings_->value(language_key).toString());
    if (!language.has_value()) {
        return std::nullopt;
    }
    const auto theme = parse_theme(settings_->value(theme_key, QStringLiteral("system")).toString())
                           .value_or(mint::desktop::ThemeMode::system);
    return mint::desktop::GuiPreferences{.language = *language, .theme = theme};
}

bool QSettingsPreferencesRepository::save(const mint::desktop::GuiPreferences& preferences) {
    settings_->setValue(language_key, qstring_language_code(preferences.language));
    settings_->setValue(theme_key, qstring_theme_code(preferences.theme));
    settings_->sync();
    return settings_->status() == QSettings::NoError;
}

std::filesystem::path QSettingsPreferencesRepository::legacy_model_config() const {
    return mint::gui::path_from_qstring(settings_->value(model_config_key).toString());
}

bool QSettingsPreferencesRepository::clear_legacy_model_config(
    const std::filesystem::path& expected_config) {
    settings_->sync();
    if (settings_->status() != QSettings::NoError) {
        return false;
    }
    const auto current = legacy_model_config();
    if (current.empty()) {
        return true;
    }
    // EN: A still-running older GUI may have selected another profile since startup.
    // ZH-CN: 仍在运行的旧 GUI 可能已在本次启动后选择了另一份配置。
    if (current != expected_config) {
        return false;
    }
    settings_->remove(model_config_key);
    settings_->sync();
    return settings_->status() == QSettings::NoError;
}

} // namespace mint::infrastructure::desktop
