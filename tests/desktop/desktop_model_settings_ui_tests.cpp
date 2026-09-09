#include "desktop_component_test_support.hpp"
#include "infrastructure/settings/shared_model_settings_runtime.hpp"
#include "mint/infrastructure/model_config_service.hpp"
#include "ui/components/settings_panel.hpp"
#include "ui/components/task_composer.hpp"
#include "ui/pages/task_workspace_page.hpp"
#include "ui/shell/mint_window.hpp"
#include "ui/support/localized_text.hpp"
#include "ui/theme/layout_metrics.hpp"

#include <FluentQt/BasicInput.h>
#include <FluentQt/TextFields.h>

#include <QAbstractButton>
#include <QApplication>
#include <QLineEdit>
#include <QPalette>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>

namespace {

using mint::test::ComponentRuntime;
using mint::test::MemoryPreferencesRepository;

TEST(DesktopComponents, UniversalModelEditorShowsCurrentConfigAndInlineCredentialAction) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    mint::ui::SettingsPanel settings;
    settings.resize(520, 680);
    settings.show();
    settings.set_model_state(
        {.provider =
             mint::desktop::ProviderDescriptor{
                 .provider = "deepseek",
                 .model = "deepseek-v4-flash",
                 .adapter = "chat_completions",
                 .endpoint = "https://api.deepseek.com/chat/completions",
                 .credential_scope = "v1:deepseek:https://api.deepseek.com/chat/completions"},
         .saved_credential = mint::desktop::SavedCredentialState::missing});
    QApplication::processEvents();

    auto* api_type =
        settings.findChild<fluent::basicinput::ComboBox*>(QStringLiteral("MintCustomModelApiType"));
    auto* base_url = settings.findChild<QLineEdit*>(QStringLiteral("MintCustomModelBaseUrl"));
    auto* model = settings.findChild<QLineEdit*>(QStringLiteral("MintCustomModelName"));
    auto* api_key =
        settings.findChild<fluent::textfields::PasswordBox*>(QStringLiteral("MintApiKeyEditor"));
    auto* action = settings.findChild<QAbstractButton*>(QStringLiteral("MintApiKeyActionButton"));
    auto* status =
        settings.findChild<fluent::textfields::Label*>(QStringLiteral("MintApiKeyStatus"));
    auto* model_card = settings.findChild<QWidget*>(QStringLiteral("MintModelCard"));
    ASSERT_NE(api_type, nullptr);
    ASSERT_NE(base_url, nullptr);
    ASSERT_NE(model, nullptr);
    ASSERT_NE(api_key, nullptr);
    ASSERT_NE(action, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(model_card, nullptr);

    EXPECT_EQ(settings.findChild<QWidget*>(QStringLiteral("MintModelProfilePicker")), nullptr);
    EXPECT_EQ(api_type->currentData().toString(), QStringLiteral("chat_completions"));
    EXPECT_EQ(base_url->text(), QStringLiteral("https://api.deepseek.com"));
    EXPECT_EQ(model->text(), QStringLiteral("deepseek-v4-flash"));
    EXPECT_EQ(api_key->echoMode(), QLineEdit::Password);
    EXPECT_EQ(api_key->passwordRevealMode(),
              fluent::textfields::PasswordBox::PasswordRevealMode::Hidden);
    EXPECT_TRUE(model_card->isAncestorOf(api_type));
    EXPECT_TRUE(model_card->isAncestorOf(base_url));
    EXPECT_TRUE(model_card->isAncestorOf(model));
    EXPECT_TRUE(model_card->isAncestorOf(api_key));
    EXPECT_TRUE(model_card->isAncestorOf(status));
    EXPECT_TRUE(api_key->isAncestorOf(action));
    const QRect action_rect(action->mapTo(api_key, QPoint{}), action->size());
    EXPECT_TRUE(api_key->rect().contains(action_rect));
    EXPECT_GT(action_rect.center().x(), api_key->rect().center().x());
    EXPECT_GE(api_key->contentMargins().right(), action->width() + mint::ui::metrics::space_sm);
    EXPECT_TRUE(action->text().isEmpty());
    EXPECT_EQ(action->toolTip(),
              mint::ui::text(mint::localization::Message::gui_settings_custom_model_save));
    EXPECT_EQ(settings.findChild<QAbstractButton*>(QStringLiteral("MintApiKeyRemoveButton")),
              nullptr);

    QSignalSpy save_model(&settings, &mint::ui::SettingsPanel::custom_model_save_requested);
    api_key->setPassword(QStringLiteral("fixture-device-key"));
    action->click();
    QApplication::processEvents();
    ASSERT_EQ(save_model.count(), 1);
    const auto saved = save_model.takeFirst();
    EXPECT_EQ(saved.at(0).toString(), QStringLiteral("chat_completions"));
    EXPECT_EQ(saved.at(1).toString(), QStringLiteral("https://api.deepseek.com"));
    EXPECT_EQ(saved.at(2).toString(), QStringLiteral("deepseek-v4-flash"));
    EXPECT_EQ(saved.at(3).toString(), QStringLiteral("fixture-device-key"));
    EXPECT_EQ(api_key->password(), QStringLiteral("fixture-device-key"));

    settings.set_model_state(
        {.provider =
             mint::desktop::ProviderDescriptor{
                 .provider = "custom",
                 .model = "deepseek-v4-flash",
                 .adapter = "chat_completions",
                 .endpoint = "https://api.deepseek.com/chat/completions",
                 .credential_scope = "v1:custom:https://api.deepseek.com/chat/completions"},
         .saved_credential = mint::desktop::SavedCredentialState::ready,
         .save_generation = 1});
    QApplication::processEvents();
    EXPECT_TRUE(status->isHidden());
    EXPECT_EQ(action->toolTip(),
              mint::ui::text(mint::localization::Message::gui_settings_api_key_remove));
    EXPECT_EQ(api_key->placeholderText(),
              mint::ui::text(mint::localization::Message::gui_settings_api_key_saved_mask));
    EXPECT_EQ(api_key->palette().color(QPalette::PlaceholderText),
              api_key->palette().color(QPalette::Text));
    api_key->setPassword(QStringLiteral("replacement-key"));
    EXPECT_EQ(action->toolTip(),
              mint::ui::text(mint::localization::Message::gui_settings_custom_model_save));
    api_key->clear();
    EXPECT_EQ(action->toolTip(),
              mint::ui::text(mint::localization::Message::gui_settings_api_key_remove));

    QSignalSpy remove_key(&settings, &mint::ui::SettingsPanel::api_key_remove_requested);
    action->click();
    QApplication::processEvents();
    EXPECT_EQ(remove_key.count(), 1);
}

TEST(DesktopComponents, SettingsDoesNotExposeTheDeferredModelCatalog) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    mint::ui::SettingsPanel settings;
    settings.resize(520, 680);
    settings.show();
    settings.set_model_state(
        {.provider =
             mint::desktop::ProviderDescriptor{.provider = "openai",
                                               .model = "gpt-5",
                                               .adapter = "responses",
                                               .endpoint = "https://api.openai.com/v1/responses",
                                               .credential_scope =
                                                   "v1:openai:https://api.openai.com/v1/responses"},
         .saved_credential = mint::desktop::SavedCredentialState::missing});
    QApplication::processEvents();

    EXPECT_EQ(settings.findChild<QWidget*>(QStringLiteral("MintModelProfilePicker")), nullptr);
    EXPECT_EQ(settings.findChild<QWidget*>(QStringLiteral("MintModelProfileMenu")), nullptr);
    auto* editor = settings.findChild<QWidget*>(QStringLiteral("MintCustomModelEditor"));
    auto* api_type =
        settings.findChild<fluent::basicinput::ComboBox*>(QStringLiteral("MintCustomModelApiType"));
    auto* base_url = settings.findChild<QLineEdit*>(QStringLiteral("MintCustomModelBaseUrl"));
    auto* model = settings.findChild<QLineEdit*>(QStringLiteral("MintCustomModelName"));
    ASSERT_NE(editor, nullptr);
    ASSERT_NE(api_type, nullptr);
    ASSERT_NE(base_url, nullptr);
    ASSERT_NE(model, nullptr);
    EXPECT_TRUE(editor->isVisible());
    EXPECT_EQ(api_type->currentData().toString(), QStringLiteral("responses"));
    EXPECT_EQ(base_url->text(), QStringLiteral("https://api.openai.com/v1"));
    EXPECT_EQ(model->text(), QStringLiteral("gpt-5"));
}

TEST(DesktopComponents, SharedModelEditorSavesWithoutPersistingAProfilePointer) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    QTemporaryDir state;
    ASSERT_TRUE(state.isValid());
    mint::infrastructure::desktop::SharedModelSettingsRuntime settings_runtime(
        std::filesystem::path(state.path().toStdString()));
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(settings_runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::MintWindow window(controller, model_settings, preferences);
    window.resize(1200, 760);
    window.show();
    ASSERT_TRUE(QTest::qWaitFor([&] { return model_settings.state().provider.has_value(); }));
    window.findChild<QAbstractButton*>(QStringLiteral("MintSettingsButton"))->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !model_settings.state().loading; }));
    auto* model = window.findChild<QLineEdit*>(QStringLiteral("MintCustomModelName"));
    auto* base = window.findChild<QLineEdit*>(QStringLiteral("MintCustomModelBaseUrl"));
    auto* key =
        window.findChild<fluent::textfields::PasswordBox*>(QStringLiteral("MintApiKeyEditor"));
    auto* save = window.findChild<QAbstractButton*>(QStringLiteral("MintApiKeyActionButton"));
    ASSERT_NE(model, nullptr);
    ASSERT_NE(base, nullptr);
    ASSERT_NE(key, nullptr);
    ASSERT_NE(save, nullptr);
    base->setText(QStringLiteral("https://models.example.test/v1"));
    model->setText(QStringLiteral("fixture-model"));
    key->setPassword(QStringLiteral("fixture-custom-key"));
    ASSERT_TRUE(save->isEnabled());
    save->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return model_settings.state().save_generation == 1; }));
    mint::ModelConfigService service(std::filesystem::path(state.path().toStdString()));
    EXPECT_EQ(service.inspect().config.model, "fixture-model");
    EXPECT_TRUE(service.inspect().saved_api_key);
    EXPECT_TRUE(model_settings.state().selected_config.empty());
    EXPECT_FALSE(repository.saved.has_value());
    EXPECT_TRUE(key->password().isEmpty());
    EXPECT_EQ(key->placeholderText(),
              mint::ui::text(mint::localization::Message::gui_settings_api_key_saved_mask));
    ASSERT_TRUE(controller.start("Use the shared model"));
    EXPECT_TRUE(runtime.last_run_request.config.empty());
}

TEST(DesktopComponents, ExternalModelChangesPreserveDirtyDraftAndOldRevisionUntilReload) {
    mint::ui::SettingsPanel settings;
    const auto snapshot = [](std::string model, std::string revision) {
        return mint::desktop::ModelSettingsState{
            .provider =
                mint::desktop::ProviderDescriptor{
                    .provider = "custom",
                    .model = std::move(model),
                    .adapter = "chat_completions",
                    .endpoint = "https://models.example.test/v1/chat/completions",
                    .revision = std::move(revision)},
            .saved_credential = mint::desktop::SavedCredentialState::missing};
    };
    settings.set_model_state(snapshot("original-model", "original-revision"));
    auto* model = settings.findChild<QLineEdit*>(QStringLiteral("MintCustomModelName"));
    auto* key =
        settings.findChild<fluent::textfields::PasswordBox*>(QStringLiteral("MintApiKeyEditor"));
    auto* save = settings.findChild<QAbstractButton*>(QStringLiteral("MintApiKeyActionButton"));
    ASSERT_NE(model, nullptr);
    ASSERT_NE(key, nullptr);
    ASSERT_NE(save, nullptr);
    model->setText(QStringLiteral("my-draft"));
    key->setPassword(QStringLiteral("draft-secret"));
    settings.set_model_state(snapshot("external-model", "external-revision"));
    EXPECT_EQ(model->text(), QStringLiteral("my-draft"));
    EXPECT_EQ(key->password(), QStringLiteral("draft-secret"));
    QSignalSpy submitted(&settings, &mint::ui::SettingsPanel::custom_model_save_requested);
    save->click();
    ASSERT_EQ(submitted.count(), 1);
    EXPECT_EQ(submitted.at(0).at(4).toString(), QStringLiteral("original-revision"));
    auto conflict = snapshot("external-model", "external-revision");
    conflict.problem = mint::desktop::ModelSettingsProblem::conflict;
    settings.set_model_state(conflict);
    EXPECT_EQ(model->text(), QStringLiteral("my-draft"));
    EXPECT_EQ(key->password(), QStringLiteral("draft-secret"));
    auto* reload = settings.findChild<QAbstractButton*>(QStringLiteral("MintModelReloadButton"));
    ASSERT_NE(reload, nullptr);
    EXPECT_FALSE(reload->isHidden());
    QObject::connect(&settings, &mint::ui::SettingsPanel::model_reload_requested, &settings, [&] {
        settings.set_model_state(snapshot("external-model", "external-revision"));
    });
    reload->click();
    EXPECT_EQ(model->text(), QStringLiteral("external-model"));
    EXPECT_TRUE(key->password().isEmpty());
    EXPECT_TRUE(reload->isHidden());
}

TEST(DesktopComponents, ExternalModelChangesRefreshACleanEditor) {
    mint::ui::SettingsPanel settings;
    mint::desktop::ModelSettingsState snapshot{
        .provider =
            mint::desktop::ProviderDescriptor{.model = "old",
                                              .adapter = "responses",
                                              .endpoint = "https://example.test/v1/responses",
                                              .revision = "old-revision"},
        .saved_credential = mint::desktop::SavedCredentialState::missing};
    settings.set_model_state(snapshot);
    snapshot.provider->model = "new";
    snapshot.provider->revision = "new-revision";
    snapshot.saved_credential = mint::desktop::SavedCredentialState::ready;
    settings.set_model_state(snapshot);
    EXPECT_EQ(settings.findChild<QLineEdit*>(QStringLiteral("MintCustomModelName"))->text(),
              QStringLiteral("new"));
    EXPECT_EQ(
        settings.findChild<fluent::textfields::PasswordBox*>(QStringLiteral("MintApiKeyEditor"))
            ->placeholderText(),
        mint::ui::text(mint::localization::Message::gui_settings_api_key_saved_mask));
}

TEST(DesktopModel, FailedUpdateKeepsTheSnapshotAndDoesNotSelectANewConfig) {
    ComponentRuntime runtime;
    mint::desktop::ModelSettingsController controller(
        runtime, {.root = "/workspace", .config = "/override.json"});
    controller.activate();
    runtime.next_failure = mint::desktop::ModelSettingsFailure::conflict;
    EXPECT_TRUE(controller.save_custom_profile(
        {.expected_revision = "stale", .base_url = "https://example.test", .model = "draft"}));
    EXPECT_EQ(controller.state().problem, mint::desktop::ModelSettingsProblem::conflict);
    EXPECT_EQ(controller.state().selected_config, "/override.json");
    EXPECT_EQ(controller.state().provider->model, "deepseek-v4-flash");
    EXPECT_FALSE(controller.state().saving);
    EXPECT_EQ(controller.state().save_generation, 0U);
    EXPECT_FALSE(controller.state().can_start_task());
}

TEST(DesktopComponents, SavingModelSettingsBlocksStartAndResumeUntilTheNewSelectionCommits) {
    ComponentRuntime runtime;
    runtime.defer_model_update = true;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(
        runtime, {.root = "/workspace", .config = "/old-override.json", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(
        runtime, {.root = "/workspace", .config = "/old-override.json", .language = "en"});
    mint::ui::MintWindow window(controller, model_settings, preferences);
    window.show();
    ASSERT_TRUE(QTest::qWaitFor([&] { return model_settings.state().can_start_task(); }));
    runtime.callbacks.tasks_loaded({{.id = "resume-me",
                                     .question = "A paused task",
                                     .status = "cancelled",
                                     .resumable = true}},
                                   "/state");
    runtime.callbacks.task_loaded("resume-me", {});
    ASSERT_TRUE(controller.state().can_resume());
    auto* composer = window.findChild<mint::ui::TaskComposer*>(QStringLiteral("MintComposer"));
    auto* action = window.findChild<QAbstractButton*>(QStringLiteral("MintTaskAction"));
    ASSERT_NE(composer, nullptr);
    ASSERT_NE(action, nullptr);
    composer->set_draft(QStringLiteral("Keep this draft while saving"));
    ASSERT_TRUE(model_settings.save_custom_profile({.expected_revision = "fixture-revision",
                                                    .base_url = "https://next.example.test/v1",
                                                    .model = "next-model",
                                                    .api_key = "fixture-next-key"}));
    EXPECT_FALSE(action->isEnabled());
    EXPECT_FALSE(controller.start("Must not run before save"));
    EXPECT_FALSE(controller.resume());
    EXPECT_EQ(runtime.run_count, 0);
    EXPECT_EQ(runtime.resume_count, 0);
    EXPECT_EQ(composer->draft(), QStringLiteral("Keep this draft while saving"));
    runtime.model_settings_callbacks.saved(
        {.provider = "custom",
         .model = "next-model",
         .adapter = "chat_completions",
         .endpoint = "https://next.example.test/v1/chat/completions",
         .config = "/state/model.json",
         .revision = "next-revision",
         .saved_api_key = true});
    ASSERT_TRUE(controller.resume());
    EXPECT_EQ(runtime.resume_count, 1);
    EXPECT_TRUE(runtime.last_resume_request.config.empty());
    ASSERT_TRUE(model_settings.save_custom_profile({.expected_revision = "next-revision",
                                                    .base_url = "https://later.example.test/v1",
                                                    .model = "later-model"}));
    EXPECT_TRUE(controller.state().running);
    EXPECT_TRUE(action->isEnabled());
    EXPECT_EQ(action->toolTip(), mint::ui::text(mint::localization::Message::gui_action_stop));
    runtime.model_settings_callbacks.error(mint::desktop::ModelSettingsFailure::unavailable);
    runtime.callbacks.run_finished({.status = "cancelled"});
    EXPECT_FALSE(controller.start("Must not fall back after failed save"));
    EXPECT_EQ(runtime.run_count, 0);
}

TEST(DesktopComponents, LegacyModelConflictShowsBothSourcesAndRequiresAnExplicitChoice) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    mint::ui::SettingsPanel settings;
    mint::desktop::ProviderDescriptor shared{.provider = "custom",
                                             .model = "shared-model",
                                             .adapter = "chat_completions",
                                             .endpoint =
                                                 "https://shared.example.test/v1/chat/completions",
                                             .config = "/state/model.json",
                                             .revision = "shared-revision",
                                             .saved_api_key = true};
    mint::desktop::ModelSettingsState snapshot{
        .provider = shared,
        .saved_credential = mint::desktop::SavedCredentialState::ready,
        .legacy_conflict = mint::desktop::LegacyModelConflict{
            .shared = shared,
            .legacy =
                mint::desktop::ProviderDescriptor{.provider = "custom",
                                                  .model = "previous-gui-model",
                                                  .adapter = "responses",
                                                  .endpoint =
                                                      "https://previous.example.test/v1/responses",
                                                  .config = "/state/previous.json",
                                                  .saved_api_key = false,
                                                  .is_override = true},
            .legacy_config = "/state/previous.json"}};
    settings.set_model_state(snapshot);
    settings.resize(380, 640);
    settings.show();
    auto* conflict = settings.findChild<QWidget*>(QStringLiteral("MintLegacyModelConflict"));
    auto* old =
        settings.findChild<fluent::textfields::Label*>(QStringLiteral("MintLegacyModelPrevious"));
    auto* use_previous =
        settings.findChild<QAbstractButton*>(QStringLiteral("MintLegacyModelImport"));
    auto* keep_shared = settings.findChild<QAbstractButton*>(QStringLiteral("MintLegacyModelKeep"));
    auto* save = settings.findChild<QAbstractButton*>(QStringLiteral("MintApiKeyActionButton"));
    auto* editor = settings.findChild<QWidget*>(QStringLiteral("MintCustomModelEditor"));
    auto* key = settings.findChild<QWidget*>(QStringLiteral("MintApiKeyEditor"));
    ASSERT_NE(conflict, nullptr);
    ASSERT_NE(old, nullptr);
    ASSERT_NE(use_previous, nullptr);
    ASSERT_NE(keep_shared, nullptr);
    ASSERT_NE(save, nullptr);
    ASSERT_NE(editor, nullptr);
    ASSERT_NE(key, nullptr);
    EXPECT_TRUE(conflict->isVisible());
    EXPECT_TRUE(editor->isHidden());
    EXPECT_TRUE(key->isHidden());
    EXPECT_TRUE(old->text().contains(QStringLiteral("previous-gui-model")));
    EXPECT_TRUE(old->text().contains(QStringLiteral("/state/previous.json")));
    EXPECT_FALSE(save->isEnabled());
    EXPECT_TRUE(use_previous->isEnabled());
    EXPECT_TRUE(keep_shared->isEnabled());
    QSignalSpy imported(&settings, &mint::ui::SettingsPanel::legacy_model_import_requested);
    QSignalSpy kept(&settings, &mint::ui::SettingsPanel::legacy_model_keep_requested);
    use_previous->click();
    keep_shared->click();
    EXPECT_EQ(imported.count(), 1);
    EXPECT_EQ(kept.count(), 1);
    snapshot.saving = true;
    settings.set_model_state(snapshot);
    EXPECT_FALSE(use_previous->isEnabled());
    EXPECT_FALSE(keep_shared->isEnabled());
    snapshot.saving = false;
    snapshot.legacy_conflict->legacy.reset();
    settings.set_model_state(snapshot);
    EXPECT_FALSE(use_previous->isEnabled());
    EXPECT_TRUE(keep_shared->isEnabled());
    snapshot.legacy_conflict.reset();
    settings.set_model_state(snapshot);
    EXPECT_FALSE(conflict->isVisible());
    EXPECT_FALSE(editor->isHidden());
    EXPECT_FALSE(key->isHidden());
}

} // namespace
