#pragma once

#include "application/model_settings_controller.hpp"
#include "application/preferences/gui_preferences.hpp"

#include <QWidget>

namespace fluent::basicinput {
class Button;
class ComboBox;
} // namespace fluent::basicinput
namespace fluent::textfields {
class Label;
} // namespace fluent::textfields
class QButtonGroup;

namespace mint::ui {

class SettingsPanel final : public QWidget {
    Q_OBJECT

  public:
    explicit SettingsPanel(QWidget* parent = nullptr);

    void set_preferences(const desktop::GuiPreferences& preferences);
    void set_model_state(const desktop::ModelSettingsState& state);
    void set_persistence_error(bool failed);
    void set_theme_persistence_error(bool failed);
    void set_model_persistence_error(bool failed);
    void retranslate();

  signals:
    void language_selected(localization::Language language);
    void theme_selected(desktop::ThemeMode theme);
    void custom_model_save_requested(QString adapter, QString base_url, QString model,
                                     QString api_key, QString expected_revision);
    void api_key_remove_requested(QString expected_revision);
    void model_reload_requested();
    void legacy_model_import_requested();
    void legacy_model_keep_requested();
    void close_requested();

  private:
    void activate_api_key_action();
    void save_custom_model();
    void populate_model_editor();
    void render_model_state();
    void update_api_key_action();
    [[nodiscard]] bool custom_model_fields_valid() const;
    [[nodiscard]] bool model_form_dirty() const;

    fluent::textfields::Label* title_ = nullptr;
    fluent::textfields::Label* appearance_label_ = nullptr;
    class ThemeChoiceButton* theme_system_ = nullptr;
    class ThemeChoiceButton* theme_light_ = nullptr;
    class ThemeChoiceButton* theme_dark_ = nullptr;
    QButtonGroup* theme_group_ = nullptr;
    fluent::textfields::Label* theme_hint_ = nullptr;
    fluent::textfields::Label* model_section_ = nullptr;
    QWidget* migration_panel_ = nullptr;
    fluent::textfields::Label* migration_notice_ = nullptr;
    fluent::textfields::Label* migration_shared_ = nullptr;
    fluent::textfields::Label* migration_legacy_ = nullptr;
    fluent::basicinput::Button* migration_import_ = nullptr;
    fluent::basicinput::Button* migration_keep_ = nullptr;
    QWidget* custom_editor_ = nullptr;
    fluent::textfields::Label* custom_api_type_label_ = nullptr;
    fluent::basicinput::ComboBox* custom_api_type_ = nullptr;
    fluent::textfields::Label* custom_base_url_label_ = nullptr;
    class UnderlineFreeLineEdit* custom_base_url_ = nullptr;
    fluent::textfields::Label* custom_model_label_ = nullptr;
    class UnderlineFreeLineEdit* custom_model_ = nullptr;
    fluent::textfields::Label* api_key_label_ = nullptr;
    class UnderlineFreePasswordBox* api_key_ = nullptr;
    fluent::basicinput::Button* api_key_action_ = nullptr;
    fluent::textfields::Label* api_key_status_ = nullptr;
    fluent::basicinput::Button* model_reload_ = nullptr;
    fluent::textfields::Label* language_label_ = nullptr;
    fluent::textfields::Label* language_hint_ = nullptr;
    fluent::basicinput::ComboBox* language_ = nullptr;
    fluent::basicinput::Button* close_ = nullptr;
    desktop::ModelSettingsState model_state_;
    bool language_persistence_error_ = false;
    bool theme_persistence_error_ = false;
    bool model_persistence_error_ = false;
    bool custom_validation_error_ = false;
    bool pending_custom_submission_ = false;
    QString loaded_provider_fingerprint_;
    QString loaded_revision_;
    bool discard_draft_ = false;
    QString loaded_adapter_;
    QString loaded_base_url_;
    QString loaded_model_;
};

} // namespace mint::ui
