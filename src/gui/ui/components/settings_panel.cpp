#include "settings_panel.hpp"

#include "ui/components/theme_choice_button.hpp"
#include "ui/components/underline_free_input.hpp"
#include "ui/support/localized_text.hpp"
#include "ui/theme/layout_metrics.hpp"

#include <FluentQt/BasicInput.h>
#include <FluentQt/Layout.h>
#include <FluentQt/TextFields.h>

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QPalette>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QUrl>
#include <QVBoxLayout>

namespace mint::ui {
namespace {

std::string path_text(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

QString endpoint_for_provider(const desktop::ProviderDescriptor& provider) {
    if (!provider.endpoint.empty()) {
        return QString::fromStdString(provider.endpoint);
    }

    const auto prefix = std::string("v1:") + provider.provider + ":";
    if (provider.credential_scope.starts_with(prefix)) {
        return QString::fromStdString(provider.credential_scope.substr(prefix.size()));
    }
    return {};
}

QString base_url_for_provider(const desktop::ProviderDescriptor& provider) {
    auto endpoint = endpoint_for_provider(provider);
    const auto adapter = QString::fromStdString(provider.adapter);
    const auto suffix = adapter == QStringLiteral("responses") ? QStringLiteral("/responses")
                        : adapter == QStringLiteral("anthropic_messages")
                            ? QStringLiteral("/messages")
                            : QStringLiteral("/chat/completions");
    if (endpoint.endsWith(suffix)) {
        endpoint.chop(suffix.size());
    }
    return endpoint;
}

QString provider_fingerprint(const desktop::ProviderDescriptor& provider) {
    return QStringLiteral("%1\n%2\n%3\n%4")
        .arg(QString::fromStdString(provider.provider), QString::fromStdString(provider.adapter),
             endpoint_for_provider(provider), QString::fromStdString(provider.model));
}

bool is_loopback_host(const QString& host) {
    const auto normalized = host.trimmed().toLower();
    if (normalized == QStringLiteral("localhost") || normalized == QStringLiteral("::1")) {
        return true;
    }

    const auto octets = normalized.split(QLatin1Char('.'));
    if (octets.size() != 4 || octets.front() != QStringLiteral("127")) {
        return false;
    }
    for (const auto& octet : octets) {
        bool parsed = false;
        const auto value = octet.toInt(&parsed);
        if (!parsed || value < 0 || value > 255 ||
            (octet.size() > 1 && octet.startsWith(QLatin1Char('0')))) {
            return false;
        }
    }
    return true;
}

} // namespace

SettingsPanel::SettingsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MintSettingsPanel"));
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(metrics::space_xl, metrics::space_xl, metrics::space_xl,
                                    metrics::space_xl);
    root_layout->setSpacing(metrics::space_md);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    title_ = new fluent::textfields::Label(this);
    title_->setObjectName(QStringLiteral("MintPrimaryText"));
    title_->setFluentTypography(Typography::FontRole::Subtitle);
    header->addWidget(title_);
    header->addStretch();
    close_ = new fluent::basicinput::Button(this);
    close_->setObjectName(QStringLiteral("MintSettingsCloseButton"));
    close_->setIconGlyph(Typography::Icons::ChromeClose);
    close_->setFluentLayout(fluent::basicinput::Button::IconOnly);
    close_->setFluentStyle(fluent::basicinput::Button::Subtle);
    header->addWidget(close_);
    root_layout->addLayout(header);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("MintSettingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("MintSettingsContent"));
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, metrics::space_xs, 0);
    layout->setSpacing(metrics::space_md);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    scroll->setWidget(content);
    root_layout->addWidget(scroll, 1);

    model_section_ = new fluent::textfields::Label(this);
    model_section_->setObjectName(QStringLiteral("MintPrimaryText"));
    model_section_->setFluentTypography(Typography::FontRole::BodyStrong);
    layout->addWidget(model_section_);

    auto* model_card = new fluent::layout::Card(this);
    model_card->setObjectName(QStringLiteral("MintModelCard"));
    model_card->setAppearance(fluent::layout::Card::LayerAlt);
    auto* model_card_layout = new QVBoxLayout(model_card);
    model_card_layout->setContentsMargins(metrics::space_lg, metrics::space_md, metrics::space_lg,
                                          metrics::space_md);
    model_card_layout->setSpacing(metrics::space_sm);

    migration_panel_ = new QWidget(model_card);
    migration_panel_->setObjectName(QStringLiteral("MintLegacyModelConflict"));
    auto* migration_layout = new QVBoxLayout(migration_panel_);
    migration_layout->setContentsMargins(0, 0, 0, metrics::space_md);
    migration_layout->setSpacing(metrics::space_sm);
    migration_notice_ = new fluent::textfields::Label(migration_panel_);
    migration_notice_->setObjectName(QStringLiteral("MintLegacyModelNotice"));
    migration_notice_->setFluentTypography(Typography::FontRole::BodyStrong);
    migration_shared_ = new fluent::textfields::Label(migration_panel_);
    migration_shared_->setObjectName(QStringLiteral("MintLegacyModelShared"));
    migration_legacy_ = new fluent::textfields::Label(migration_panel_);
    migration_legacy_->setObjectName(QStringLiteral("MintLegacyModelPrevious"));
    for (auto* label : {migration_notice_, migration_shared_, migration_legacy_}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setMinimumWidth(0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        migration_layout->addWidget(label);
    }
    migration_import_ = new fluent::basicinput::Button(migration_panel_);
    migration_import_->setObjectName(QStringLiteral("MintLegacyModelImport"));
    migration_import_->setFluentStyle(fluent::basicinput::Button::Accent);
    migration_keep_ = new fluent::basicinput::Button(migration_panel_);
    migration_keep_->setObjectName(QStringLiteral("MintLegacyModelKeep"));
    for (auto* button : {migration_import_, migration_keep_}) {
        button->setFluentSize(fluent::basicinput::Button::Small);
        migration_layout->addWidget(button);
    }
    connect(migration_import_, &QPushButton::clicked, this,
            &SettingsPanel::legacy_model_import_requested);
    connect(migration_keep_, &QPushButton::clicked, this,
            &SettingsPanel::legacy_model_keep_requested);
    model_card_layout->addWidget(migration_panel_);

    custom_editor_ = new QWidget(model_card);
    custom_editor_->setObjectName(QStringLiteral("MintCustomModelEditor"));
    auto* custom_layout = new QVBoxLayout(custom_editor_);
    custom_layout->setContentsMargins(0, 0, 0, 0);
    custom_layout->setSpacing(metrics::space_sm);

    custom_api_type_label_ = new fluent::textfields::Label(custom_editor_);
    custom_api_type_label_->setObjectName(QStringLiteral("MintTertiaryText"));
    custom_api_type_label_->setFluentTypography(Typography::FontRole::Caption);
    custom_layout->addWidget(custom_api_type_label_);
    custom_api_type_ = new fluent::basicinput::ComboBox(custom_editor_);
    custom_api_type_->setObjectName(QStringLiteral("MintCustomModelApiType"));
    custom_layout->addWidget(custom_api_type_);

    custom_base_url_label_ = new fluent::textfields::Label(custom_editor_);
    custom_base_url_label_->setObjectName(QStringLiteral("MintTertiaryText"));
    custom_base_url_label_->setFluentTypography(Typography::FontRole::Caption);
    custom_layout->addWidget(custom_base_url_label_);
    custom_base_url_ = new UnderlineFreeLineEdit(custom_editor_);
    custom_base_url_->setObjectName(QStringLiteral("MintCustomModelBaseUrl"));
    custom_base_url_->setClearButtonEnabled(false);
    custom_layout->addWidget(custom_base_url_);

    custom_model_label_ = new fluent::textfields::Label(custom_editor_);
    custom_model_label_->setObjectName(QStringLiteral("MintTertiaryText"));
    custom_model_label_->setFluentTypography(Typography::FontRole::Caption);
    custom_layout->addWidget(custom_model_label_);
    custom_model_ = new UnderlineFreeLineEdit(custom_editor_);
    custom_model_->setObjectName(QStringLiteral("MintCustomModelName"));
    custom_model_->setClearButtonEnabled(false);
    custom_layout->addWidget(custom_model_);
    model_card_layout->addWidget(custom_editor_);

    api_key_label_ = new fluent::textfields::Label(model_card);
    api_key_label_->setObjectName(QStringLiteral("MintTertiaryText"));
    api_key_label_->setFluentTypography(Typography::FontRole::Caption);
    model_card_layout->addWidget(api_key_label_);

    api_key_ = new UnderlineFreePasswordBox(model_card);
    api_key_->setObjectName(QStringLiteral("MintApiKeyEditor"));
    api_key_->setPasswordRevealMode(fluent::textfields::PasswordBox::PasswordRevealMode::Hidden);
    api_key_action_ = new fluent::basicinput::Button(api_key_);
    api_key_action_->setObjectName(QStringLiteral("MintApiKeyActionButton"));
    api_key_action_->setFluentLayout(fluent::basicinput::Button::IconOnly);
    api_key_action_->setFluentSize(fluent::basicinput::Button::Small);
    api_key_action_->setFixedSize(metrics::settings_inline_action_size,
                                  metrics::settings_inline_action_size);
    api_key_->set_trailing_action(api_key_action_);
    model_card_layout->addWidget(api_key_);

    api_key_status_ = new fluent::textfields::Label(model_card);
    api_key_status_->setObjectName(QStringLiteral("MintApiKeyStatus"));
    api_key_status_->setFluentTypography(Typography::FontRole::Caption);
    api_key_status_->setWordWrap(true);
    model_card_layout->addWidget(api_key_status_);
    model_reload_ = new fluent::basicinput::Button(model_card);
    model_reload_->setObjectName(QStringLiteral("MintModelReloadButton"));
    model_reload_->setFluentStyle(fluent::basicinput::Button::Subtle);
    model_reload_->setFluentSize(fluent::basicinput::Button::Small);
    model_card_layout->addWidget(model_reload_, 0, Qt::AlignLeft);
    connect(model_reload_, &QPushButton::clicked, this, [this] {
        discard_draft_ = true;
        api_key_->clear();
        emit model_reload_requested();
    });
    layout->addWidget(model_card);

    layout->addSpacing(metrics::space_sm);

    language_label_ = new fluent::textfields::Label(this);
    language_label_->setObjectName(QStringLiteral("MintPrimaryText"));
    language_label_->setFluentTypography(Typography::FontRole::BodyStrong);
    layout->addWidget(language_label_);

    language_ = new fluent::basicinput::ComboBox(this);
    language_->setObjectName(QStringLiteral("MintLanguagePicker"));
    language_->setAccessibleName(text(localization::Message::gui_settings_language_label));
    language_->setMinimumWidth(220);
    layout->addWidget(language_);

    language_hint_ = new fluent::textfields::Label(this);
    language_hint_->setObjectName(QStringLiteral("MintSettingsLanguageHint"));
    language_hint_->setFluentTypography(Typography::FontRole::Caption);
    language_hint_->setWordWrap(true);
    language_hint_->hide();
    layout->addWidget(language_hint_);

    layout->addSpacing(metrics::space_sm);

    appearance_label_ = new fluent::textfields::Label(this);
    appearance_label_->setObjectName(QStringLiteral("MintPrimaryText"));
    appearance_label_->setFluentTypography(Typography::FontRole::BodyStrong);
    layout->addWidget(appearance_label_);

    theme_group_ = new QButtonGroup(this);
    theme_group_->setExclusive(true);
    auto* theme_choices = new QHBoxLayout;
    theme_choices->setContentsMargins(0, 0, 0, 0);
    theme_choices->setSpacing(metrics::space_sm);
    theme_system_ = new ThemeChoiceButton(desktop::ThemeMode::system, this);
    theme_system_->setObjectName(QStringLiteral("MintThemeSystemButton"));
    theme_light_ = new ThemeChoiceButton(desktop::ThemeMode::light, this);
    theme_light_->setObjectName(QStringLiteral("MintThemeLightButton"));
    theme_dark_ = new ThemeChoiceButton(desktop::ThemeMode::dark, this);
    theme_dark_->setObjectName(QStringLiteral("MintThemeDarkButton"));
    for (auto* choice : {theme_system_, theme_light_, theme_dark_}) {
        theme_group_->addButton(choice, static_cast<int>(choice->mode()));
        theme_choices->addWidget(choice, 1);
    }
    theme_system_->setChecked(true);
    layout->addLayout(theme_choices);

    theme_hint_ = new fluent::textfields::Label(this);
    theme_hint_->setObjectName(QStringLiteral("MintSettingsThemeHint"));
    theme_hint_->setFluentTypography(Typography::FontRole::Caption);
    theme_hint_->setWordWrap(true);
    theme_hint_->hide();
    layout->addWidget(theme_hint_);
    layout->addStretch();

    QWidget::setTabOrder(custom_api_type_, custom_base_url_);
    QWidget::setTabOrder(custom_base_url_, custom_model_);
    QWidget::setTabOrder(custom_model_, api_key_);
    QWidget::setTabOrder(api_key_, api_key_action_);
    QWidget::setTabOrder(api_key_action_, language_);

    connect(close_, &QPushButton::clicked, this, &SettingsPanel::close_requested);
    connect(api_key_, &QLineEdit::textChanged, this, [this] { update_api_key_action(); });
    connect(api_key_, &QLineEdit::returnPressed, this, &SettingsPanel::activate_api_key_action);
    connect(api_key_action_, &QPushButton::clicked, this, &SettingsPanel::activate_api_key_action);
    connect(custom_base_url_, &QLineEdit::textChanged, this, [this] {
        custom_validation_error_ = false;
        update_api_key_action();
    });
    connect(custom_model_, &QLineEdit::textChanged, this, [this] {
        custom_validation_error_ = false;
        update_api_key_action();
    });
    connect(custom_base_url_, &QLineEdit::returnPressed, this, &SettingsPanel::save_custom_model);
    connect(custom_model_, &QLineEdit::returnPressed, this, &SettingsPanel::save_custom_model);
    connect(custom_api_type_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        custom_validation_error_ = false;
        update_api_key_action();
    });
    connect(theme_group_, &QButtonGroup::idToggled, this, [this](int id, bool checked) {
        if (checked) {
            emit theme_selected(static_cast<desktop::ThemeMode>(id));
        }
    });
    connect(language_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        emit language_selected(index == 1 ? localization::Language::simplified_chinese
                                          : localization::Language::english);
    });
    retranslate();
}

void SettingsPanel::set_model_state(const desktop::ModelSettingsState& state) {
    const auto saved = state.save_generation != model_state_.save_generation;
    model_state_ = state;
    if (pending_custom_submission_ && !state.saving) {
        pending_custom_submission_ = false;
    }
    if (saved) {
        discard_draft_ = true;
        api_key_->clear();
    }
    populate_model_editor();
    render_model_state();
}

void SettingsPanel::set_preferences(const desktop::GuiPreferences& preferences) {
    {
        const QSignalBlocker blocker(language_);
        language_->setCurrentIndex(
            preferences.language == localization::Language::simplified_chinese ? 1 : 0);
    }
    const QSignalBlocker blocker(theme_group_);
    if (auto* choice = theme_group_->button(static_cast<int>(preferences.theme));
        choice != nullptr) {
        choice->setChecked(true);
    }
}

void SettingsPanel::set_persistence_error(bool failed) {
    language_persistence_error_ = failed;
    language_hint_->setText(failed ? text(localization::Message::gui_settings_language_save_failed)
                                   : QString{});
    language_hint_->setVisible(failed);
}

void SettingsPanel::set_theme_persistence_error(bool failed) {
    theme_persistence_error_ = failed;
    theme_hint_->setText(failed ? text(localization::Message::gui_settings_theme_save_failed)
                                : QString{});
    theme_hint_->setVisible(failed);
}

void SettingsPanel::set_model_persistence_error(bool failed) {
    model_persistence_error_ = failed;
    render_model_state();
}

void SettingsPanel::retranslate() {
    title_->setText(text(localization::Message::gui_settings_title));
    appearance_label_->setText(text(localization::Message::gui_settings_appearance_label));
    theme_system_->setText(text(localization::Message::gui_settings_theme_system));
    theme_light_->setText(text(localization::Message::gui_settings_theme_light));
    theme_dark_->setText(text(localization::Message::gui_settings_theme_dark));
    for (auto* choice : {theme_system_, theme_light_, theme_dark_}) {
        choice->setAccessibleName(choice->text());
        choice->setAccessibleDescription(text(localization::Message::gui_settings_theme_hint));
    }
    set_theme_persistence_error(theme_persistence_error_);
    model_section_->setText(text(localization::Message::gui_settings_model_section));
    model_reload_->setText(text(localization::Message::gui_settings_model_reload));
    migration_notice_->setText(text(localization::Message::gui_settings_legacy_conflict));
    migration_import_->setText(text(localization::Message::gui_settings_legacy_import));
    migration_keep_->setText(text(localization::Message::gui_settings_legacy_keep));
    migration_import_->setAccessibleName(migration_import_->text());
    migration_keep_->setAccessibleName(migration_keep_->text());
    custom_api_type_label_->setText(
        text(localization::Message::gui_settings_custom_model_api_type));
    custom_base_url_label_->setText(
        text(localization::Message::gui_settings_custom_model_base_url));
    custom_base_url_->setPlaceholderText(
        text(localization::Message::gui_settings_custom_model_base_url_placeholder));
    custom_base_url_->setAccessibleName(custom_base_url_label_->text());
    custom_model_label_->setText(text(localization::Message::gui_settings_custom_model_model));
    custom_model_->setPlaceholderText(
        text(localization::Message::gui_settings_custom_model_model_placeholder));
    custom_model_->setAccessibleName(custom_model_label_->text());
    const auto selected_api_type = custom_api_type_->currentData().toString();
    const QSignalBlocker api_type_blocker(custom_api_type_);
    custom_api_type_->clear();
    custom_api_type_->addItem(text(localization::Message::gui_settings_custom_model_api_type_chat),
                              QStringLiteral("chat_completions"));
    custom_api_type_->addItem(
        text(localization::Message::gui_settings_custom_model_api_type_responses),
        QStringLiteral("responses"));
    custom_api_type_->addItem(
        text(localization::Message::gui_settings_custom_model_api_type_anthropic),
        QStringLiteral("anthropic_messages"));
    const auto api_type_index = custom_api_type_->findData(selected_api_type);
    custom_api_type_->setCurrentIndex(api_type_index < 0 ? 0 : api_type_index);
    custom_api_type_->setAccessibleName(custom_api_type_label_->text());
    api_key_label_->setText(text(localization::Message::gui_settings_api_key_label));
    api_key_->setPlaceholderText(text(localization::Message::gui_settings_api_key_placeholder));
    api_key_->setAccessibleName(api_key_label_->text());
    api_key_->setAccessibleDescription(text(localization::Message::gui_settings_api_key_hint));
    api_key_action_->setText({});
    language_label_->setText(text(localization::Message::gui_settings_language_label));
    set_persistence_error(language_persistence_error_);
    close_->setToolTip(text(localization::Message::gui_action_close_settings));
    close_->setAccessibleName(close_->toolTip());
    language_->setAccessibleName(language_label_->text());
    language_->setAccessibleDescription(text(localization::Message::gui_settings_language_hint));

    const auto selected = language_->currentIndex();
    const QSignalBlocker blocker(language_);
    language_->clear();
    language_->addItem(text(localization::Message::gui_settings_language_english));
    language_->addItem(text(localization::Message::gui_settings_language_chinese));
    language_->setCurrentIndex(selected < 0 ? 0 : selected);
    render_model_state();
}

void SettingsPanel::save_custom_model() {
    if (!custom_model_fields_valid()) {
        custom_validation_error_ = true;
        render_model_state();
        return;
    }

    auto api_key = api_key_->password();
    const auto adapter = custom_api_type_->currentData().toString();
    const auto base_url = custom_base_url_->text().trimmed();
    const auto model = custom_model_->text().trimmed();
    pending_custom_submission_ = true;
    custom_validation_error_ = false;
    emit custom_model_save_requested(adapter, base_url, model, api_key, loaded_revision_);
    api_key.fill(QChar{});
}

void SettingsPanel::activate_api_key_action() {
    const auto saved = model_state_.saved_credential == desktop::SavedCredentialState::ready;
    if (saved && api_key_->password().trimmed().isEmpty() && !model_form_dirty()) {
        emit api_key_remove_requested(loaded_revision_);
        return;
    }
    save_custom_model();
}

void SettingsPanel::update_api_key_action() {
    const auto has_value = !api_key_->password().trimmed().isEmpty();
    const auto saved = model_state_.saved_credential == desktop::SavedCredentialState::ready;
    const auto form_dirty = model_form_dirty();
    const auto removes_saved_key = saved && !has_value && !form_dirty;
    const auto storage_available =
        model_state_.saved_credential != desktop::SavedCredentialState::unavailable;

    const auto shows_saved_mask = saved && !has_value &&
                                  custom_api_type_->currentData().toString() == loaded_adapter_ &&
                                  custom_base_url_->text().trimmed() == loaded_base_url_;
    api_key_->setPlaceholderText(
        text(shows_saved_mask ? localization::Message::gui_settings_api_key_saved_mask
                              : localization::Message::gui_settings_api_key_placeholder));
    auto palette = api_key_->palette();
    palette.setColor(QPalette::PlaceholderText,
                     shows_saved_mask ? palette.color(QPalette::Text)
                                      : palette.color(QPalette::Disabled, QPalette::Text));
    api_key_->setPalette(palette);
    api_key_->setAccessibleDescription(
        text(shows_saved_mask ? localization::Message::gui_settings_api_key_saved_hint
                              : localization::Message::gui_settings_api_key_hint));

    api_key_action_->setIconGlyph(removes_saved_key ? Typography::Icons::Delete
                                                    : Typography::Icons::Save,
                                  Typography::IconSize::Compact);
    api_key_action_->setFluentStyle(removes_saved_key ? fluent::basicinput::Button::Subtle
                                                      : fluent::basicinput::Button::Accent);
    api_key_action_->setToolTip(text(removes_saved_key
                                         ? localization::Message::gui_settings_api_key_remove
                                         : localization::Message::gui_settings_custom_model_save));
    api_key_action_->setAccessibleName(api_key_action_->toolTip());
    const auto can_save = custom_model_fields_valid() && (form_dirty || has_value);
    api_key_action_->setEnabled(!model_state_.loading && !model_state_.saving &&
                                !model_state_.legacy_conflict && storage_available &&
                                !pending_custom_submission_ && (removes_saved_key || can_save));
}

bool SettingsPanel::custom_model_fields_valid() const {
    if (custom_model_->text().trimmed().isEmpty() || custom_model_->text().trimmed().size() > 512 ||
        custom_api_type_->currentData().toString().isEmpty()) {
        return false;
    }
    const auto source = custom_base_url_->text().trimmed();
    if (source.isEmpty() || source.size() > 2048) {
        return false;
    }
    const QUrl url(source, QUrl::StrictMode);
    const auto scheme = url.scheme().toLower();
    return url.isValid() && !url.isRelative() &&
           (scheme == QStringLiteral("https") ||
            (scheme == QStringLiteral("http") && is_loopback_host(url.host()))) &&
           !url.host().isEmpty() && url.userName().isEmpty() && url.password().isEmpty() &&
           url.query().isEmpty() && url.fragment().isEmpty();
}

bool SettingsPanel::model_form_dirty() const {
    return custom_api_type_->currentData().toString() != loaded_adapter_ ||
           custom_base_url_->text().trimmed() != loaded_base_url_ ||
           custom_model_->text().trimmed() != loaded_model_;
}

void SettingsPanel::populate_model_editor() {
    if (!model_state_.provider.has_value()) {
        return;
    }

    if (!discard_draft_ && !loaded_provider_fingerprint_.isEmpty() &&
        (model_form_dirty() || !api_key_->password().isEmpty())) {
        return;
    }
    const auto fingerprint = provider_fingerprint(*model_state_.provider);
    const auto revision = QString::fromStdString(model_state_.provider->revision);
    if (!discard_draft_ && fingerprint == loaded_provider_fingerprint_ &&
        revision == loaded_revision_) {
        return;
    }
    discard_draft_ = false;
    loaded_revision_ = revision;
    loaded_provider_fingerprint_ = fingerprint;
    loaded_adapter_ = QString::fromStdString(model_state_.provider->adapter);
    loaded_base_url_ = base_url_for_provider(*model_state_.provider);
    loaded_model_ = QString::fromStdString(model_state_.provider->model);

    const QSignalBlocker api_type_blocker(custom_api_type_);
    const QSignalBlocker base_url_blocker(custom_base_url_);
    const QSignalBlocker model_blocker(custom_model_);
    const auto api_type_index = custom_api_type_->findData(loaded_adapter_);
    custom_api_type_->setCurrentIndex(api_type_index < 0 ? 0 : api_type_index);
    if (api_type_index < 0) {
        loaded_adapter_ = custom_api_type_->currentData().toString();
    }
    custom_base_url_->setText(loaded_base_url_);
    custom_model_->setText(loaded_model_);
    api_key_->clear();
    custom_validation_error_ = false;
}

void SettingsPanel::render_model_state() {
    custom_editor_->setVisible(!model_state_.legacy_conflict);
    api_key_label_->setVisible(!model_state_.legacy_conflict);
    api_key_->setVisible(!model_state_.legacy_conflict);
    const auto editable =
        !model_state_.loading && !model_state_.saving && !model_state_.legacy_conflict;
    custom_api_type_->setEnabled(editable);
    custom_base_url_->setEnabled(editable);
    custom_model_->setEnabled(editable);
    migration_panel_->setVisible(model_state_.legacy_conflict.has_value());
    if (model_state_.legacy_conflict) {
        const auto& conflict = *model_state_.legacy_conflict;
        const auto summary = [](localization::Message id,
                                const desktop::ProviderDescriptor& provider) {
            return text(
                id,
                {localization::arg(localization::Placeholder::model, provider.model),
                 localization::arg(localization::Placeholder::endpoint, provider.endpoint),
                 localization::arg(localization::Placeholder::path, path_text(provider.config))});
        };
        migration_shared_->setText(
            summary(localization::Message::gui_settings_legacy_shared, conflict.shared));
        migration_legacy_->setText(
            conflict.legacy
                ? summary(localization::Message::gui_settings_legacy_previous, *conflict.legacy)
                : text(localization::Message::gui_settings_legacy_unavailable,
                       {localization::arg(localization::Placeholder::path,
                                          path_text(conflict.legacy_config))}));
        migration_import_->setEnabled(!model_state_.loading && !model_state_.saving &&
                                      conflict.legacy.has_value());
        migration_keep_->setEnabled(!model_state_.loading && !model_state_.saving);
    }

    QString status;
    auto tone = QStringLiteral("secondary");
    if (custom_validation_error_) {
        tone = QStringLiteral("critical");
        status = text(localization::Message::gui_settings_custom_model_validation);
    } else if (model_persistence_error_) {
        tone = QStringLiteral("critical");
        status = text(localization::Message::gui_settings_model_save_failed);
    } else if (model_state_.problem == desktop::ModelSettingsProblem::invalid_input) {
        tone = QStringLiteral("critical");
        status = text(localization::Message::gui_settings_custom_model_validation);
    } else if (model_state_.problem == desktop::ModelSettingsProblem::save_failed) {
        tone = QStringLiteral("critical");
        status = text(localization::Message::gui_settings_model_save_failed);
    } else if (model_state_.problem == desktop::ModelSettingsProblem::conflict) {
        tone = QStringLiteral("critical");
        status = text(localization::Message::gui_settings_model_conflict);
    } else if (model_state_.saving) {
        status = text(localization::Message::gui_settings_model_saving);
    } else if (model_state_.loading) {
        status = text(localization::Message::gui_settings_provider_loading);
    } else if (model_state_.problem == desktop::ModelSettingsProblem::inspection_failed) {
        tone = QStringLiteral("critical");
        status = text(localization::Message::gui_settings_provider_error);
    } else if (model_state_.saved_credential == desktop::SavedCredentialState::unavailable) {
        tone = QStringLiteral("attention");
        status = text(localization::Message::gui_settings_api_key_storage_unavailable);
    } else if (model_state_.saved_credential == desktop::SavedCredentialState::error) {
        tone = QStringLiteral("critical");
        status = text(localization::Message::gui_settings_api_key_load_failed);
    }
    api_key_status_->setProperty("tone", tone);
    api_key_status_->setText(status);
    api_key_status_->setVisible(!status.isEmpty());
    model_reload_->setVisible(model_state_.problem == desktop::ModelSettingsProblem::conflict ||
                              model_state_.legacy_conflict.has_value());
    api_key_status_->style()->unpolish(api_key_status_);
    api_key_status_->style()->polish(api_key_status_);

    const auto storage_available =
        model_state_.saved_credential != desktop::SavedCredentialState::unavailable;
    api_key_->setEnabled(editable && storage_available);
    update_api_key_action();
}

} // namespace mint::ui
