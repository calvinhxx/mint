#include "desktop_component_test_support.hpp"
#include "infrastructure/settings/qsettings_preferences_repository.hpp"
#include "ui/components/settings_panel.hpp"
#include "ui/components/task_sidebar.hpp"
#include "ui/pages/task_workspace_page.hpp"
#include "ui/shell/mint_window.hpp"
#include "ui/support/localized_text.hpp"
#include "ui/theme/mint_theme.hpp"

#include <FluentQt/BasicInput.h>
#include <FluentQt/Collections.h>
#include <FluentQt/TextFields.h>

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QListView>
#include <QPalette>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

#include <algorithm>

namespace {

using mint::test::ComponentRuntime;
using mint::test::MemoryPreferencesRepository;

class ScopedAppearanceRestore final {
  public:
    ScopedAppearanceRestore()
        : mode_(fluent::FluentElement::currentTheme() == fluent::FluentElement::Light
                    ? mint::desktop::ThemeMode::light
                    : mint::desktop::ThemeMode::dark) {}
    ~ScopedAppearanceRestore() {
        mint::ui::theme::apply(mode_);
    }

  private:
    mint::desktop::ThemeMode mode_;
};

TEST(DesktopTheme, UsesCodexNeutralCanvasesAndReadableForegrounds) {
    const auto& light = mint::ui::theme::palette(mint::ui::theme::Scheme::light);
    const auto& dark = mint::ui::theme::palette(mint::ui::theme::Scheme::dark);
    fluent::basicinput::ComboBox control;

    EXPECT_EQ(light.canvas, QColor(QStringLiteral("#FFFFFF")));
    EXPECT_EQ(light.text_primary, QColor(QStringLiteral("#0D0D0D")));
    EXPECT_EQ(dark.canvas, QColor(QStringLiteral("#181818")));
    EXPECT_EQ(dark.text_primary, QColor(QStringLiteral("#FFFFFF")));
    EXPECT_NE(light.canvas, dark.canvas);
    EXPECT_EQ(control.themeColors().strokeDefault, dark.stroke);
    EXPECT_EQ(control.themeColors().strokeSecondary, dark.stroke_strong);
}

TEST(DesktopTheme, InputPalettesSurviveGlobalStyleChangesAndSystemResolution) {
    ScopedAppearanceRestore restore;
    mint::ui::theme::install(mint::desktop::ThemeMode::light);
    mint::ui::SettingsPanel panel;
    panel.resize(440, 860);
    panel.show();
    QApplication::processEvents();
    auto* base_url =
        panel.findChild<fluent::textfields::LineEdit*>(QStringLiteral("MintCustomModelBaseUrl"));
    auto* model =
        panel.findChild<fluent::textfields::LineEdit*>(QStringLiteral("MintCustomModelName"));
    auto* api_key =
        panel.findChild<fluent::textfields::LineEdit*>(QStringLiteral("MintApiKeyEditor"));
    ASSERT_NE(base_url, nullptr);
    ASSERT_NE(model, nullptr);
    ASSERT_NE(api_key, nullptr);
    EXPECT_EQ(model->palette().color(QPalette::Text),
              mint::ui::theme::palette(mint::ui::theme::Scheme::light).text_primary);
    model->setText(QStringLiteral("fixture-model"));
    model->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    ASSERT_TRUE(model->hasFocus());
    model->setSelection(0, 7);
    ASSERT_EQ(model->selectedText(), QStringLiteral("fixture"));

    for (const auto mode : {mint::desktop::ThemeMode::dark, mint::desktop::ThemeMode::light,
                            mint::desktop::ThemeMode::system, mint::desktop::ThemeMode::dark,
                            mint::desktop::ThemeMode::dark}) {
        const auto scheme = mint::ui::theme::resolved_scheme(mode);
        const auto& expected = mint::ui::theme::palette(scheme);
        mint::ui::theme::apply(mode);
        QApplication::processEvents();
        EXPECT_EQ(QApplication::palette().color(QPalette::Window), expected.canvas);
        for (const auto* field : {base_url, model, api_key}) {
            EXPECT_EQ(field->palette().color(QPalette::Active, QPalette::Text),
                      expected.text_primary);
            EXPECT_EQ(field->palette().color(QPalette::Inactive, QPalette::Text),
                      expected.text_primary);
            EXPECT_EQ(field->palette().color(QPalette::Disabled, QPalette::Text),
                      expected.text_disabled);
        }
        EXPECT_EQ(model->text(), QStringLiteral("fixture-model"));
        EXPECT_EQ(model->selectedText(), QStringLiteral("fixture"));
    }
}

TEST(DesktopComponents, LanguageSettingUsesASameWindowDrawerAndPersists) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::MintWindow window(controller, model_settings, preferences);
    window.resize(1200, 760);
    window.show();
    QApplication::processEvents();

    auto* button = window.findChild<QAbstractButton*>(QStringLiteral("MintSettingsButton"));
    auto* drawer =
        window.findChild<fluent::collections::DrawerView*>(QStringLiteral("MintSettingsDrawer"));
    auto* language_picker =
        window.findChild<fluent::basicinput::ComboBox*>(QStringLiteral("MintLanguagePicker"));
    auto* language_hint =
        window.findChild<fluent::textfields::Label*>(QStringLiteral("MintSettingsLanguageHint"));
    ASSERT_NE(button, nullptr);
    ASSERT_NE(drawer, nullptr);
    ASSERT_NE(language_picker, nullptr);
    ASSERT_NE(language_hint, nullptr);
    ASSERT_TRUE(window.titleBar()->isAncestorOf(button));
    EXPECT_EQ(window.findChildren<QAbstractButton*>(QStringLiteral("MintSettingsButton")).size(),
              1);
    EXPECT_NE(window.findChild<QWidget*>(QStringLiteral("MintAppMark")), nullptr);
    EXPECT_EQ(window.findChild<QWidget*>(QStringLiteral("MintSidebarToggle")), nullptr);
    drawer->setAnimationEnabled(false);

    runtime.callbacks.tasks_loaded({{.id = "task-1", .question = "First", .status = "completed"},
                                    {.id = "task-2", .question = "Second", .status = "completed"}},
                                   "/state");
    QApplication::processEvents();
    int chrome_publications = 0;
    QObject::connect(window.titleBar(), &fluent::windowing::TitleBar::chromeGeometryChanged,
                     [&chrome_publications] { ++chrome_publications; });
    ASSERT_TRUE(controller.select_task(1));
    QApplication::processEvents();
    EXPECT_GT(chrome_publications, 0);
    const auto selected_before_settings = controller.state().selected_task;
    const auto button_center = button->mapTo(window.titleBar(), button->rect().center());
    const auto exclusions = window.titleBar()->dragExclusionRects();
    EXPECT_TRUE(
        std::any_of(exclusions.begin(), exclusions.end(),
                    [button_center](const QRect& area) { return area.contains(button_center); }));

    button->click();
    EXPECT_TRUE(language_hint->isHidden());
    language_picker->setCurrentIndex(1);
    QApplication::processEvents();

    EXPECT_TRUE(drawer->isOpen());
    EXPECT_EQ(controller.state().selected_task, selected_before_settings);
    ASSERT_TRUE(repository.saved.has_value());
    EXPECT_EQ(repository.saved->language, mint::localization::Language::simplified_chinese);
    EXPECT_EQ(button->toolTip(), QStringLiteral("设置"));

    repository.save_succeeds = false;
    language_picker->setCurrentIndex(0);
    QApplication::processEvents();
    EXPECT_FALSE(language_hint->isHidden());
    EXPECT_EQ(language_hint->text(),
              mint::ui::text(mint::localization::Message::gui_settings_language_save_failed));
}

TEST(DesktopComponents, SettingsDrawerDoesNotLeakClicksIntoTaskNavigation) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::MintWindow window(controller, model_settings, preferences);
    window.resize(1200, 760);
    window.show();
    QApplication::processEvents();

    runtime.callbacks.tasks_loaded({{.id = "task-1", .question = "First", .status = "completed"},
                                    {.id = "task-2", .question = "Second", .status = "completed"}},
                                   "/state");
    ASSERT_TRUE(controller.select_task(1));
    QApplication::processEvents();

    auto* settings = window.findChild<QAbstractButton*>(QStringLiteral("MintSettingsButton"));
    auto* close = window.findChild<QAbstractButton*>(QStringLiteral("MintSettingsCloseButton"));
    auto* drawer =
        window.findChild<fluent::collections::DrawerView*>(QStringLiteral("MintSettingsDrawer"));
    auto* evidence =
        window.findChild<fluent::collections::DrawerView*>(QStringLiteral("MintEvidenceDrawer"));
    auto* sidebar = window.findChild<mint::ui::TaskSidebar*>(QStringLiteral("MintTaskSidebar"));
    ASSERT_NE(settings, nullptr);
    ASSERT_NE(close, nullptr);
    ASSERT_NE(drawer, nullptr);
    ASSERT_NE(evidence, nullptr);
    ASSERT_NE(sidebar, nullptr);
    EXPECT_FALSE(drawer->isInteractive());
    EXPECT_FALSE(evidence->isInteractive());

    const auto selected_task = controller.state().selected_task;
    QSignalSpy navigation(sidebar, &mint::ui::TaskSidebar::task_selected);
    ASSERT_TRUE(navigation.isValid());

    QTest::mouseClick(settings, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(drawer->isOpen(), 1000);
    EXPECT_EQ(controller.state().selected_task, selected_task);
    EXPECT_TRUE(navigation.isEmpty());

    QTest::mouseClick(close, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!drawer->isVisible(), 1000);
    EXPECT_EQ(controller.state().selected_task, selected_task);
    EXPECT_TRUE(navigation.isEmpty());
}

TEST(DesktopComponents, AppearanceSettingAppliesAndPersistsAcrossTheApplication) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english, mint::desktop::ThemeMode::dark);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::MintWindow window(controller, model_settings, preferences);
    window.resize(1200, 760);
    window.show();
    QApplication::processEvents();

    auto* settings = window.findChild<QAbstractButton*>(QStringLiteral("MintSettingsButton"));
    auto* light = window.findChild<QAbstractButton*>(QStringLiteral("MintThemeLightButton"));
    auto* dark = window.findChild<QAbstractButton*>(QStringLiteral("MintThemeDarkButton"));
    auto* system = window.findChild<QAbstractButton*>(QStringLiteral("MintThemeSystemButton"));
    auto* hint =
        window.findChild<fluent::textfields::Label*>(QStringLiteral("MintSettingsThemeHint"));
    auto* main_surface = window.findChild<QWidget*>(QStringLiteral("MintMainSurface"));
    ASSERT_NE(settings, nullptr);
    ASSERT_NE(light, nullptr);
    ASSERT_NE(dark, nullptr);
    ASSERT_NE(system, nullptr);
    ASSERT_NE(hint, nullptr);
    ASSERT_NE(main_surface, nullptr);

    settings->click();
    QApplication::processEvents();
    EXPECT_TRUE(hint->isHidden());
    EXPECT_TRUE(dark->isChecked());
    EXPECT_FALSE(light->isChecked());
    EXPECT_FALSE(system->isChecked());

    light->click();
    QApplication::processEvents();
    EXPECT_EQ(preferences.preferences().theme, mint::desktop::ThemeMode::light);
    ASSERT_TRUE(repository.saved.has_value());
    EXPECT_EQ(repository.saved->theme, mint::desktop::ThemeMode::light);
    EXPECT_EQ(fluent::FluentElement::currentTheme(), fluent::FluentElement::Light);
    EXPECT_EQ(main_surface->palette().color(QPalette::Window),
              mint::ui::theme::palette(mint::ui::theme::Scheme::light).canvas);

    repository.save_succeeds = false;
    dark->click();
    QApplication::processEvents();
    EXPECT_EQ(fluent::FluentElement::currentTheme(), fluent::FluentElement::Dark);
    EXPECT_FALSE(hint->isHidden());
    EXPECT_EQ(hint->text(),
              mint::ui::text(mint::localization::Message::gui_settings_theme_save_failed));
}

TEST(DesktopComponents, PointerAppearanceSurvivesDrawerReopenAndPreferencesReload) {
    ScopedAppearanceRestore restore;
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto settings_path = directory.filePath(QStringLiteral("preferences.ini"));
    {
        ComponentRuntime runtime;
        mint::infrastructure::desktop::QSettingsPreferencesRepository repository(settings_path);
        mint::desktop::PreferencesController preferences(repository);
        preferences.initialize(mint::localization::Language::english,
                               mint::desktop::ThemeMode::light);
        mint::ui::theme::apply(preferences.preferences().theme);
        mint::desktop::ModelSettingsController model_settings(
            runtime, {.root = "/workspace", .language = "en"});
        mint::desktop::TaskWorkspaceController controller(runtime,
                                                          {.root = "/workspace", .language = "en"});
        mint::ui::MintWindow window(controller, model_settings, preferences);
        window.resize(1200, 900);
        window.show();
        QApplication::processEvents();

        runtime.callbacks.tasks_loaded(
            {{.id = "task-1", .question = "First", .status = "completed"},
             {.id = "task-2", .question = "Second", .status = "completed"}},
            "/state");
        ASSERT_TRUE(controller.select_task(1));
        QApplication::processEvents();

        auto* settings = window.findChild<QAbstractButton*>(QStringLiteral("MintSettingsButton"));
        auto* close = window.findChild<QAbstractButton*>(QStringLiteral("MintSettingsCloseButton"));
        auto* dark = window.findChild<QAbstractButton*>(QStringLiteral("MintThemeDarkButton"));
        auto* drawer = window.findChild<fluent::collections::DrawerView*>(
            QStringLiteral("MintSettingsDrawer"));
        auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("MintSettingsScroll"));
        auto* sidebar = window.findChild<mint::ui::TaskSidebar*>(QStringLiteral("MintTaskSidebar"));
        auto* main_surface = window.findChild<QWidget*>(QStringLiteral("MintMainSurface"));
        ASSERT_NE(settings, nullptr);
        ASSERT_NE(close, nullptr);
        ASSERT_NE(dark, nullptr);
        ASSERT_NE(drawer, nullptr);
        ASSERT_NE(scroll, nullptr);
        ASSERT_NE(sidebar, nullptr);
        ASSERT_NE(main_surface, nullptr);

        const auto selected_task = controller.state().selected_task;
        QSignalSpy navigation(sidebar, &mint::ui::TaskSidebar::task_selected);
        QTest::mouseClick(settings, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(drawer->isOpen(), 1000);
        scroll->ensureWidgetVisible(dark);
        QApplication::processEvents();
        const auto dark_center = dark->mapTo(scroll->viewport(), dark->rect().center());
        ASSERT_TRUE(scroll->viewport()->rect().contains(dark_center));
        QTest::mouseClick(dark, Qt::LeftButton);
        QTRY_COMPARE(preferences.preferences().theme, mint::desktop::ThemeMode::dark);
        EXPECT_EQ(fluent::FluentElement::currentTheme(), fluent::FluentElement::Dark);
        EXPECT_EQ(main_surface->palette().color(QPalette::Window),
                  mint::ui::theme::palette(mint::ui::theme::Scheme::dark).canvas);

        for (int cycle = 0; cycle < 3; ++cycle) {
            QTest::mouseClick(close, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(!drawer->isVisible(), 1000);
            QTest::mouseClick(settings, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(drawer->isOpen(), 1000);
            EXPECT_TRUE(dark->isChecked());
            EXPECT_EQ(controller.state().selected_task, selected_task);
            EXPECT_TRUE(navigation.isEmpty());
        }
    }

    mint::infrastructure::desktop::QSettingsPreferencesRepository repository(settings_path);
    mint::desktop::PreferencesController reloaded(repository);
    reloaded.initialize();
    EXPECT_EQ(reloaded.preferences().theme, mint::desktop::ThemeMode::dark);
    EXPECT_EQ(reloaded.preferences().language, mint::localization::Language::english);
}

TEST(DesktopComponents, AccessibleThemeToggleAndRapidPointerChangesSharePreferenceState) {
    ScopedAppearanceRestore restore;
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english, mint::desktop::ThemeMode::light);
    mint::ui::theme::apply(preferences.preferences().theme);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::MintWindow window(controller, model_settings, preferences);
    window.resize(1200, 900);
    window.show();
    QApplication::processEvents();

    auto* settings = window.findChild<QAbstractButton*>(QStringLiteral("MintSettingsButton"));
    auto* light = window.findChild<QAbstractButton*>(QStringLiteral("MintThemeLightButton"));
    auto* dark = window.findChild<QAbstractButton*>(QStringLiteral("MintThemeDarkButton"));
    auto* panel = window.findChild<mint::ui::SettingsPanel*>(QStringLiteral("MintSettingsPanel"));
    auto* drawer =
        window.findChild<fluent::collections::DrawerView*>(QStringLiteral("MintSettingsDrawer"));
    auto* model =
        window.findChild<fluent::textfields::LineEdit*>(QStringLiteral("MintCustomModelName"));
    auto* main_surface = window.findChild<QWidget*>(QStringLiteral("MintMainSurface"));
    ASSERT_NE(settings, nullptr);
    ASSERT_NE(light, nullptr);
    ASSERT_NE(dark, nullptr);
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(drawer, nullptr);
    ASSERT_NE(model, nullptr);
    ASSERT_NE(main_surface, nullptr);
    QTest::mouseClick(settings, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(drawer->isOpen(), 1000);

    auto* accessible = QAccessible::queryAccessibleInterface(dark);
    ASSERT_NE(accessible, nullptr);
    auto* actions = accessible->actionInterface();
    ASSERT_NE(actions, nullptr);
    ASSERT_TRUE(actions->actionNames().contains(QAccessibleActionInterface::toggleAction()));
    QSignalSpy clicks(dark, &QAbstractButton::clicked);
    QSignalSpy selections(panel, &mint::ui::SettingsPanel::theme_selected);
    actions->doAction(QAccessibleActionInterface::toggleAction());
    EXPECT_TRUE(dark->isChecked());
    EXPECT_TRUE(clicks.isEmpty());
    EXPECT_EQ(selections.size(), 1);
    EXPECT_EQ(preferences.preferences().theme, mint::desktop::ThemeMode::dark);
    EXPECT_EQ(fluent::FluentElement::currentTheme(), fluent::FluentElement::Dark);

    for (int cycle = 0; cycle < 4; ++cycle) {
        QTest::mouseClick(light, Qt::LeftButton);
        QTest::mouseClick(dark, Qt::LeftButton);
    }
    QApplication::processEvents();
    EXPECT_TRUE(dark->isChecked());
    EXPECT_FALSE(light->isChecked());
    EXPECT_EQ(preferences.preferences().theme, mint::desktop::ThemeMode::dark);
    ASSERT_TRUE(repository.saved.has_value());
    EXPECT_EQ(repository.saved->theme, mint::desktop::ThemeMode::dark);
    EXPECT_EQ(fluent::FluentElement::currentTheme(), fluent::FluentElement::Dark);
    EXPECT_EQ(main_surface->palette().color(QPalette::Window),
              mint::ui::theme::palette(mint::ui::theme::Scheme::dark).canvas);
    EXPECT_EQ(model->palette().color(QPalette::Text),
              mint::ui::theme::palette(mint::ui::theme::Scheme::dark).text_primary);

    selections.clear();
    panel->set_preferences(preferences.preferences());
    EXPECT_TRUE(selections.isEmpty());
}

TEST(DesktopComponents, SettingsCanReverseCloseWithoutLosingSelectionOrFocus) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::MintWindow window(controller, model_settings, preferences);
    window.resize(1200, 760);
    window.show();
    QApplication::processEvents();
    runtime.callbacks.tasks_loaded({{.id = "task-1", .question = "First", .status = "completed"},
                                    {.id = "task-2", .question = "Second", .status = "completed"}},
                                   "/state");
    ASSERT_TRUE(controller.select_task(1));
    QApplication::processEvents();

    auto* page = window.findChild<mint::ui::TaskWorkspacePage*>(QStringLiteral("MintRoot"));
    auto* sidebar = window.findChild<mint::ui::TaskSidebar*>(QStringLiteral("MintTaskSidebar"));
    auto* tasks = window.findChild<QListView*>(QStringLiteral("MintTaskList"));
    auto* drawer =
        window.findChild<fluent::collections::DrawerView*>(QStringLiteral("MintSettingsDrawer"));
    auto* close = window.findChild<QAbstractButton*>(QStringLiteral("MintSettingsCloseButton"));
    ASSERT_NE(page, nullptr);
    ASSERT_NE(sidebar, nullptr);
    ASSERT_NE(tasks, nullptr);
    ASSERT_NE(drawer, nullptr);
    ASSERT_NE(close, nullptr);
    tasks->setFocus(Qt::TabFocusReason);
    QApplication::processEvents();
    ASSERT_EQ(QApplication::focusWidget(), tasks);
    const auto selected_task = controller.state().selected_task;
    QSignalSpy navigation(sidebar, &mint::ui::TaskSidebar::task_selected);

    page->open_settings();
    QTRY_VERIFY_WITH_TIMEOUT(drawer->isOpen(), 1000);
    const auto inspection_count = runtime.inspection_count;
    QSignalSpy reopened(drawer, &fluent::collections::DrawerView::opened);
    QTest::mouseClick(close, Qt::LeftButton);
    ASSERT_TRUE(drawer->isVisible());
    page->open_settings();
    QTRY_COMPARE_WITH_TIMEOUT(reopened.size(), 1, 1000);
    EXPECT_TRUE(drawer->isVisible());
    EXPECT_TRUE(drawer->isOpen());
    EXPECT_EQ(runtime.inspection_count, inspection_count);
    EXPECT_EQ(controller.state().selected_task, selected_task);
    EXPECT_TRUE(navigation.isEmpty());

    QTest::mouseClick(close, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!drawer->isVisible(), 1000);
    QTRY_COMPARE(QApplication::focusWidget(), tasks);
    EXPECT_EQ(controller.state().selected_task, selected_task);
    EXPECT_TRUE(navigation.isEmpty());
}

TEST(DesktopComponents, SettingsUsesThreeOrderedSectionsInANarrowEnglishWindow) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english, mint::desktop::ThemeMode::light);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::MintWindow window(controller, model_settings, preferences);
    window.resize(720, 580);
    window.show();
    QApplication::processEvents();

    auto* settings = window.findChild<QAbstractButton*>(QStringLiteral("MintSettingsButton"));
    auto* light = window.findChild<QAbstractButton*>(QStringLiteral("MintThemeLightButton"));
    auto* hint =
        window.findChild<fluent::textfields::Label*>(QStringLiteral("MintSettingsThemeHint"));
    auto* model_card = window.findChild<QWidget*>(QStringLiteral("MintModelCard"));
    auto* api_key =
        window.findChild<fluent::textfields::PasswordBox*>(QStringLiteral("MintApiKeyEditor"));
    auto* api_key_status =
        window.findChild<fluent::textfields::Label*>(QStringLiteral("MintApiKeyStatus"));
    auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("MintSettingsScroll"));
    ASSERT_NE(settings, nullptr);
    ASSERT_NE(light, nullptr);
    ASSERT_NE(hint, nullptr);
    ASSERT_NE(model_card, nullptr);
    ASSERT_NE(api_key, nullptr);
    ASSERT_NE(api_key_status, nullptr);
    ASSERT_NE(scroll, nullptr);

    settings->click();
    QApplication::processEvents();

    auto* language_picker = window.findChild<QWidget*>(QStringLiteral("MintLanguagePicker"));
    ASSERT_NE(language_picker, nullptr);
    const auto model_bottom = model_card->mapTo(&window, model_card->rect().bottomLeft()).y();
    const auto language_top = language_picker->mapTo(&window, QPoint{}).y();
    const auto language_bottom =
        language_picker->mapTo(&window, language_picker->rect().bottomLeft()).y();
    const auto appearance_top = light->mapTo(&window, QPoint{}).y();
    EXPECT_TRUE(hint->isHidden());
    EXPECT_TRUE(model_card->isAncestorOf(api_key));
    EXPECT_TRUE(model_card->isAncestorOf(api_key_status));
    EXPECT_TRUE(api_key_status->isHidden());
    EXPECT_LT(model_bottom, language_top);
    EXPECT_LT(language_bottom, appearance_top);
    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
}

} // namespace
