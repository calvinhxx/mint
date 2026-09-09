#include "infrastructure/settings/qsettings_preferences_repository.hpp"
#include "infrastructure/settings/shared_model_settings_runtime.hpp"
#include "mint/infrastructure/model_config_service.hpp"
#include "ui/components/conversation_transcript.hpp"
#include "ui/components/run_conversation.hpp"
#include "ui/components/settings_panel.hpp"
#include "ui/components/task_composer.hpp"
#include "ui/components/task_sidebar.hpp"
#include "ui/delegates/run_timeline_delegate.hpp"
#include "ui/delegates/task_item_delegate.hpp"
#include "ui/models/run_timeline_model.hpp"
#include "ui/models/task_list_model.hpp"
#include "ui/pages/task_workspace_page.hpp"
#include "ui/presentation/run_event_presentation.hpp"
#include "ui/shell/mint_window.hpp"
#include "ui/support/localized_text.hpp"
#include "ui/theme/layout_metrics.hpp"
#include "ui/theme/mint_theme.hpp"

#include <FluentQt/BasicInput.h>
#include <FluentQt/Collections.h>
#include <FluentQt/MenusToolbars.h>
#include <FluentQt/Scrolling.h>
#include <FluentQt/StatusInfo.h>
#include <FluentQt/TextFields.h>

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QListView>
#include <QMouseEvent>
#include <QPalette>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

#include <gtest/gtest.h>

#include <algorithm>
#include <unordered_map>

namespace {

class ComponentRuntime final : public mint::desktop::AgentRuntime,
                               public mint::desktop::ModelSettingsRuntime {
  public:
    void set_callbacks(mint::desktop::AgentRuntimeCallbacks value) override {
        callbacks = std::move(value);
    }
    void refresh_tasks(const std::filesystem::path&, const std::string&) override {}
    void load_task(const std::string& task_id) override {
        loaded_task = task_id;
        ++load_count;
    }
    void start_task(mint::desktop::RunRequest request) override {
        last_run_request = std::move(request);
        ++run_count;
    }
    void resume_task(mint::desktop::ResumeRequest request) override {
        last_resume_request = std::move(request);
        ++resume_count;
    }
    [[nodiscard]] bool resolve_approval(bool) override {
        return true;
    }
    void cancel() override {}
    [[nodiscard]] bool running() const noexcept override {
        return false;
    }

    void set_model_settings_callbacks(mint::desktop::ModelSettingsCallbacks value) override {
        model_settings_callbacks = std::move(value);
    }
    void inspect_model(mint::desktop::ModelInspectionRequest request) override {
        ++inspection_count;
        last_inspection = std::move(request);
        if (model_settings_callbacks.inspected) {
            auto provider = mint::desktop::ProviderDescriptor{
                .provider = "deepseek",
                .model = "deepseek-v4-flash",
                .adapter = "chat_completions",
                .credential_scope = credential_scope,
                .config = last_inspection.config,
                .credential_source = configured_credential_source,
                .credential_available = configured_credential_available,
                .revision = "fixture-revision"};
            if (const auto configured = providers.find(last_inspection.config.generic_string());
                configured != providers.end()) {
                provider = configured->second;
                provider.config = last_inspection.config;
            }
            model_settings_callbacks.inspected(std::move(provider));
        }
    }
    void update_model(mint::desktop::ModelUpdateRequest request) override {
        last_update = std::move(request);
        if (defer_model_update)
            return;
        if (next_failure) {
            model_settings_callbacks.error(*next_failure);
            return;
        }
        if (model_settings_callbacks.saved) {
            model_settings_callbacks.saved(
                {.provider = "custom",
                 .model = last_update.model,
                 .adapter =
                     std::string(mint::desktop::custom_model_adapter_code(last_update.adapter)),
                 .endpoint = last_update.base_url,
                 .config = "/state/model.json",
                 .revision = "saved-revision",
                 .saved_api_key = !last_update.remove_api_key});
        }
    }
    void resolve_legacy_conflict(mint::desktop::LegacyModelResolution request) override {
        last_resolution = std::move(request);
    }

    mint::desktop::AgentRuntimeCallbacks callbacks;
    mint::desktop::ModelSettingsCallbacks model_settings_callbacks;
    mint::desktop::ModelInspectionRequest last_inspection;
    int inspection_count = 0;
    mint::desktop::RunRequest last_run_request;
    mint::desktop::ResumeRequest last_resume_request;
    mint::desktop::LegacyModelResolution last_resolution;
    int run_count = 0;
    int resume_count = 0;
    bool defer_model_update = false;
    std::string loaded_task;
    int load_count = 0;
    std::unordered_map<std::string, mint::desktop::ProviderDescriptor> providers;
    mint::desktop::ModelUpdateRequest last_update;
    std::optional<mint::desktop::ModelSettingsFailure> next_failure;
    mint::desktop::CredentialSource configured_credential_source =
        mint::desktop::CredentialSource::inline_key;
    bool configured_credential_available = true;
    std::string credential_scope = "v1:deepseek:https://api.deepseek.com/chat/completions";
};

class MemoryPreferencesRepository final : public mint::desktop::PreferencesRepository {
  public:
    [[nodiscard]] std::optional<mint::desktop::GuiPreferences> load() const override {
        return saved;
    }

    bool save(const mint::desktop::GuiPreferences& preferences) override {
        saved = preferences;
        return save_succeeds;
    }

    std::optional<mint::desktop::GuiPreferences> saved;
    bool save_succeeds = true;
};

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

TEST(DesktopModel, PersistedResumableTaskIsNotPresentedAsLive) {
    mint::ui::TaskListModel model;
    model.set_tasks({mint::desktop::TaskSummary{
        .id = "task-1", .question = "Inspect", .status = "running", .resumable = true}});

    const auto index = model.index(0, 0);
    EXPECT_EQ(model.data(index, mint::ui::TaskListModel::StatusRole).toString(),
              QStringLiteral("resumable"));
}

TEST(DesktopModel, TaskRowsUpdateInPlaceWhenTheirIdentityIsStable) {
    mint::ui::TaskListModel model;
    std::vector<mint::desktop::TaskSummary> tasks;
    tasks.reserve(10'000);
    for (int row = 0; row < 10'000; ++row) {
        tasks.push_back({.id = "task-" + std::to_string(row),
                         .question = "Question " + std::to_string(row),
                         .status = "completed"});
    }
    model.set_tasks(tasks);
    int model_resets = 0;
    int data_changes = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&model_resets] { ++model_resets; });
    QObject::connect(&model, &QAbstractItemModel::dataChanged, [&data_changes] { ++data_changes; });

    model.set_tasks(tasks);
    tasks.back().status = "failed";
    model.set_tasks(tasks);

    EXPECT_EQ(model.rowCount(), 10'000);
    EXPECT_EQ(model_resets, 0);
    EXPECT_EQ(data_changes, 1);
    EXPECT_EQ(model.data(model.index(9'999, 0), mint::ui::TaskListModel::StatusRole).toString(),
              QStringLiteral("failed"));
}

TEST(DesktopModel, WorkspaceRowsGroupOnlyTheActiveProjectsTasks) {
    mint::ui::TaskListModel model;
    const std::vector<mint::desktop::TaskSummary> tasks{
        {.id = "task-1", .question = "First", .status = "completed"},
        {.id = "task-2", .question = "Second", .status = "completed"}};

    model.set_navigation({"/projects/alpha", "/projects/beta"}, "/projects/beta", tasks, {}, false,
                         false, {}, {});

    ASSERT_EQ(model.rowCount(), 4);
    EXPECT_EQ(model.row_kind(0), mint::ui::TaskListModel::RowKind::workspace);
    EXPECT_EQ(model.workspace_root_at(0), QStringLiteral("/projects/alpha"));
    EXPECT_FALSE(
        model.data(model.index(0, 0), mint::ui::TaskListModel::ActiveWorkspaceRole).toBool());
    EXPECT_TRUE(
        model.data(model.index(0, 0), mint::ui::TaskListModel::WorkspaceRemovableRole).toBool());
    EXPECT_EQ(model.row_kind(1), mint::ui::TaskListModel::RowKind::workspace);
    EXPECT_EQ(model.workspace_root_at(1), QStringLiteral("/projects/beta"));
    EXPECT_TRUE(
        model.data(model.index(1, 0), mint::ui::TaskListModel::ActiveWorkspaceRole).toBool());
    EXPECT_TRUE(
        model.data(model.index(1, 0), mint::ui::TaskListModel::ExpandedWorkspaceRole).toBool());
    EXPECT_TRUE(
        model.data(model.index(1, 0), mint::ui::TaskListModel::WorkspaceRemovableRole).toBool());
    EXPECT_EQ(model.task_id_at(2), QStringLiteral("task-1"));
    EXPECT_EQ(model.task_id_at(3), QStringLiteral("task-2"));

    model.set_navigation({"/projects/alpha", "/projects/beta"}, "/projects/beta", tasks, {}, false,
                         false, {}, {}, false);

    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_FALSE(
        model.data(model.index(1, 0), mint::ui::TaskListModel::ExpandedWorkspaceRole).toBool());
    EXPECT_EQ(model.row_for_task_id("task-1"), -1);
}

TEST(DesktopModel, AnUnselectedWorkspaceDoesNotCreateABlankSidebarRow) {
    mint::ui::TaskListModel model;
    model.set_navigation({{}}, {}, {}, {}, false, false, {}, {});
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(model.row_for_workspace({}), -1);

    model.set_navigation({{}, "/projects/alpha"}, {}, {}, {}, false, false, {}, {});
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.workspace_root_at(0), QStringLiteral("/projects/alpha"));
    EXPECT_FALSE(model.index(0, 0).data(mint::ui::TaskListModel::ActiveWorkspaceRole).toBool());
}

TEST(DesktopComponents, EmptyWorkspaceExplainsWhySendingIsDisabledAndKeepsTheFolderAction) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto base = mint::desktop::normalize_workspace_root(
        std::filesystem::path(directory.path().toStdString()));
    const auto project = base / "project";
    std::filesystem::create_directories(project);
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::TaskWorkspaceController controller(runtime, {.state_root = base / "state"});
    mint::desktop::ModelSettingsController model_settings(runtime, {});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(840, 640);
    page.show();
    QApplication::processEvents();

    auto* input = page.findChild<fluent::textfields::TextEdit*>(QStringLiteral("MintTaskInput"));
    auto* action = page.findChild<QAbstractButton*>(QStringLiteral("MintTaskAction"));
    auto* add = page.findChild<QAbstractButton*>(QStringLiteral("MintProjectButton"));
    auto* composer = page.findChild<mint::ui::TaskComposer*>();
    auto* transcript = page.findChild<mint::ui::ConversationTranscript*>();
    ASSERT_NE(input, nullptr);
    ASSERT_NE(action, nullptr);
    ASSERT_NE(add, nullptr);
    ASSERT_NE(composer, nullptr);
    ASSERT_NE(transcript, nullptr);
    EXPECT_FALSE(input->isEnabled());
    EXPECT_FALSE(action->isEnabled());
    EXPECT_EQ(action->toolTip(),
              mint::ui::text(mint::localization::Message::gui_workspace_required));
    EXPECT_TRUE(transcript->toPlainText().contains(
        mint::ui::text(mint::localization::Message::gui_workspace_required)));
    EXPECT_TRUE(add->isEnabled());
    action->click();
    EXPECT_TRUE(runtime.last_run_request.question.empty());

    add->click();
    auto* dialog = page.findChild<QFileDialog*>(QStringLiteral("MintProjectDialog"));
    ASSERT_NE(dialog, nullptr);
    dialog->reject();
    QApplication::processEvents();

    ASSERT_TRUE(controller.set_root(project));
    EXPECT_TRUE(input->isEnabled());
    composer->set_draft(QStringLiteral("Inspect this project"));
    EXPECT_TRUE(action->isEnabled());
    EXPECT_FALSE(transcript->toPlainText().contains(
        mint::ui::text(mint::localization::Message::gui_workspace_required)));
    action->click();
    EXPECT_EQ(runtime.last_run_request.root, project);
    EXPECT_EQ(runtime.last_run_request.question, "Inspect this project");
}

TEST(DesktopComponents, PointerNavigationKeepsSidebarAndWorkspaceStateInSync) {
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::TaskWorkspaceController controller(
        runtime, {.root = "/projects/alpha", .language = "en"});
    ASSERT_TRUE(controller.set_root("/projects/beta"));
    mint::desktop::ModelSettingsController model_settings(
        runtime, {.root = "/projects/beta", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1000, 700);
    page.show();
    QApplication::processEvents();

    runtime.callbacks.tasks_loaded({{.id = "task-1", .question = "First", .status = "completed"},
                                    {.id = "task-2", .question = "Second", .status = "completed"}},
                                   "/state");
    QApplication::processEvents();
    runtime.callbacks.task_loaded("task-1", {.result = mint::desktop::TaskResult{
                                                 .status = "completed", .answer = "First answer"}});
    QApplication::processEvents();

    auto* list = page.findChild<fluent::collections::ListView*>();
    auto* sidebar = page.findChild<mint::ui::TaskSidebar*>();
    auto* transcript = page.findChild<mint::ui::ConversationTranscript*>(
        QStringLiteral("MintConversationTranscript"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(sidebar, nullptr);
    ASSERT_NE(transcript, nullptr);
    auto* model = static_cast<mint::ui::TaskListModel*>(list->model());
    ASSERT_NE(model, nullptr);
    const auto selected_task = model->index(model->row_for_task_id("task-1"), 0);
    const auto workspace = model->index(model->row_for_workspace("/projects/beta"), 0);
    ASSERT_EQ(list->currentIndex(), selected_task);
    ASSERT_TRUE(workspace.isValid());

    QSignalSpy workspace_navigation(sidebar, &mint::ui::TaskSidebar::workspace_selected);
    const auto workspace_bounds = static_cast<QListView*>(list)->visualRect(workspace);
    ASSERT_TRUE(workspace_bounds.isValid());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, workspace_bounds.center());
    QApplication::processEvents();

    EXPECT_EQ(workspace_navigation.count(), 1);
    EXPECT_EQ(controller.state().task_id, "task-1");
    EXPECT_EQ(model->rowCount(), 2);
    EXPECT_EQ(model->row_for_task_id("task-1"), -1);
    EXPECT_EQ(list->currentIndex().row(), model->row_for_workspace("/projects/beta"));

    const auto collapsed_workspace = model->index(model->row_for_workspace("/projects/beta"), 0);
    const auto collapsed_workspace_bounds =
        static_cast<QListView*>(list)->visualRect(collapsed_workspace);
    ASSERT_TRUE(collapsed_workspace_bounds.isValid());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                      collapsed_workspace_bounds.center());
    QApplication::processEvents();

    EXPECT_EQ(workspace_navigation.count(), 2);
    ASSERT_EQ(model->rowCount(), 4);
    auto restored_task = model->index(model->row_for_task_id("task-1"), 0);
    EXPECT_EQ(list->currentIndex(), restored_task);
    EXPECT_TRUE(list->selectionModel()->isSelected(restored_task));
    EXPECT_TRUE(transcript->toPlainText().contains(QStringLiteral("First answer")));

    const auto focused_workspace = model->index(model->row_for_workspace("/projects/beta"), 0);
    list->setCurrentIndex(focused_workspace);
    QApplication::processEvents();

    EXPECT_EQ(workspace_navigation.count(), 2);
    EXPECT_EQ(model->rowCount(), 4);
    EXPECT_EQ(controller.state().task_id, "task-1");

    QTest::keyClick(list, Qt::Key_Return);
    QApplication::processEvents();

    EXPECT_EQ(workspace_navigation.count(), 3);
    EXPECT_EQ(model->rowCount(), 2);

    const auto keyboard_collapsed_workspace =
        model->index(model->row_for_workspace("/projects/beta"), 0);
    list->setCurrentIndex(keyboard_collapsed_workspace);
    QTest::keyClick(list, Qt::Key_Return);
    QApplication::processEvents();

    EXPECT_EQ(workspace_navigation.count(), 4);
    EXPECT_EQ(model->rowCount(), 4);
    restored_task = model->index(model->row_for_task_id("task-1"), 0);
    EXPECT_EQ(list->currentIndex(), restored_task);
    EXPECT_TRUE(list->selectionModel()->isSelected(restored_task));

    QSignalSpy task_navigation(sidebar, &mint::ui::TaskSidebar::task_selected);
    const auto second_task = model->index(model->row_for_task_id("task-2"), 0);
    const auto second_task_bounds = static_cast<QListView*>(list)->visualRect(second_task);
    ASSERT_TRUE(second_task_bounds.isValid());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                      second_task_bounds.center());
    QApplication::processEvents();

    EXPECT_EQ(task_navigation.count(), 1);
    EXPECT_EQ(controller.state().task_id, "task-2");
    EXPECT_EQ(runtime.loaded_task, "task-2");
    EXPECT_EQ(list->currentIndex(), second_task);
    EXPECT_TRUE(list->selectionModel()->isSelected(second_task));
    EXPECT_FALSE(transcript->toPlainText().contains(QStringLiteral("First answer")));

    runtime.callbacks.task_loaded(
        "task-2",
        {.result = mint::desktop::TaskResult{.status = "completed", .answer = "Second answer"}});
    QApplication::processEvents();
    EXPECT_TRUE(transcript->toPlainText().contains(QStringLiteral("Second answer")));
    EXPECT_FALSE(transcript->toPlainText().contains(QStringLiteral("First answer")));

    QTest::keyClick(list, Qt::Key_Up);
    QApplication::processEvents();

    EXPECT_EQ(task_navigation.count(), 2);
    EXPECT_EQ(controller.state().task_id, "task-1");
    EXPECT_EQ(runtime.loaded_task, "task-1");
    EXPECT_EQ(list->currentIndex(), restored_task);
    EXPECT_TRUE(list->selectionModel()->isSelected(restored_task));

    const auto other_workspace = model->index(model->row_for_workspace("/projects/alpha"), 0);
    const auto other_workspace_bounds = static_cast<QListView*>(list)->visualRect(other_workspace);
    ASSERT_TRUE(other_workspace_bounds.isValid());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                      other_workspace_bounds.center());
    QApplication::processEvents();

    EXPECT_EQ(controller.state().root, std::filesystem::path("/projects/alpha"));
    EXPECT_TRUE(controller.state().loading_tasks);
    EXPECT_TRUE(controller.state().task_id.empty());
    EXPECT_EQ(list->currentIndex().row(), model->row_for_workspace("/projects/alpha"));

    runtime.callbacks.tasks_loaded(
        {{.id = "task-alpha", .question = "Alpha task", .status = "completed"}}, "/state-alpha");
    QApplication::processEvents();

    EXPECT_EQ(controller.state().task_id, "task-alpha");
    EXPECT_EQ(list->currentIndex().row(), model->row_for_task_id("task-alpha"));
    EXPECT_TRUE(list->selectionModel()->isSelected(list->currentIndex()));
}

TEST(DesktopComponents, SidebarCollectionHasALocalizedAccessibleName) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    mint::ui::TaskSidebar sidebar;
    auto* list = sidebar.findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));

    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->accessibleName(), QStringLiteral("Workspaces"));
    auto* accessible = QAccessible::queryAccessibleInterface(list);
    ASSERT_NE(accessible, nullptr);
    EXPECT_EQ(accessible->role(), QAccessible::List);
    EXPECT_EQ(accessible->text(QAccessible::Name), QStringLiteral("Workspaces"));

    sidebar.resize(280, 360);
    sidebar.show();
    mint::desktop::TaskWorkspaceState state;
    state.root = "/projects/alpha";
    state.workspace_roots = {"/projects/alpha", "/projects/beta"};
    state.tasks = {{.id = "task-1", .question = "First task", .status = "completed"},
                   {.id = "task-2", .question = "Second task", .status = "completed"}};
    state.task_id = "task-1";
    state.title = "First task";
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();

    ASSERT_EQ(accessible->childCount(), 4);
    const QStringList names{QStringLiteral("alpha"), QStringLiteral("First task"),
                            QStringLiteral("Second task"), QStringLiteral("beta")};
    for (int row = 0; row < names.size(); ++row) {
        auto* child = accessible->child(row);
        ASSERT_NE(child, nullptr);
        EXPECT_EQ(child->role(), QAccessible::ListItem);
        EXPECT_EQ(child->text(QAccessible::Name), names[row]);
        EXPECT_EQ(accessible->indexOfChild(child), row);
        EXPECT_EQ(child->parent(), accessible);
        EXPECT_FALSE(child->rect().isEmpty());
    }
    EXPECT_TRUE(list->focusPolicy() & Qt::TabFocus);
    auto* table = accessible->tableInterface();
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->rowCount(), 4);
    EXPECT_EQ(table->columnCount(), 1);
    QSignalSpy task_navigation(&sidebar, &mint::ui::TaskSidebar::task_selected);
    EXPECT_TRUE(table->selectRow(2));
    QTRY_COMPARE(task_navigation.count(), 1);
    EXPECT_EQ(task_navigation.front().front().toString(), QStringLiteral("task-2"));
    EXPECT_EQ(list->currentIndex().row(), 2);
    EXPECT_TRUE(accessible->child(2)->state().selected);

    auto* first_task_actions = accessible->child(1)->actionInterface();
    ASSERT_NE(first_task_actions, nullptr);
    ASSERT_TRUE(
        first_task_actions->actionNames().contains(QAccessibleActionInterface::toggleAction()));
    first_task_actions->doAction(QAccessibleActionInterface::toggleAction());
    QTRY_COMPARE(task_navigation.count(), 2);
    EXPECT_EQ(task_navigation.back().front().toString(), QStringLiteral("task-1"));
    EXPECT_EQ(list->currentIndex().row(), 1);

    QSignalSpy workspace_navigation(&sidebar, &mint::ui::TaskSidebar::workspace_selected);
    auto* workspace_actions = accessible->child(3)->actionInterface();
    ASSERT_NE(workspace_actions, nullptr);
    workspace_actions->doAction(QAccessibleActionInterface::toggleAction());
    QTRY_COMPARE(workspace_navigation.count(), 1);
    EXPECT_EQ(workspace_navigation.front().front().toString(), QStringLiteral("/projects/beta"));
    EXPECT_EQ(list->currentIndex().row(), 3);
    EXPECT_EQ(task_navigation.count(), 2);
}

TEST(DesktopComponents, WorkspaceRowOwnsNewTaskAndCanCreateItInAnotherWorkspace) {
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::TaskWorkspaceController controller(
        runtime, {.root = "/projects/alpha", .language = "en"});
    ASSERT_TRUE(controller.set_root("/projects/beta"));
    mint::desktop::ModelSettingsController model_settings(
        runtime, {.root = "/projects/beta", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1000, 700);
    page.show();
    QApplication::processEvents();

    runtime.callbacks.tasks_loaded(
        {{.id = "task-beta", .question = "Beta task", .status = "completed"}}, "/state-beta");
    QApplication::processEvents();

    auto* sidebar = page.findChild<mint::ui::TaskSidebar*>();
    auto* list = page.findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));
    ASSERT_NE(sidebar, nullptr);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(page.findChild<QAbstractButton*>(QStringLiteral("MintNewTaskButton")), nullptr);
    auto* model = static_cast<mint::ui::TaskListModel*>(list->model());
    ASSERT_NE(model, nullptr);
    const auto alpha = model->index(model->row_for_workspace("/projects/alpha"), 0);
    ASSERT_TRUE(alpha.isValid());

    QSignalSpy create_requests(sidebar, &mint::ui::TaskSidebar::new_task_requested);
    const auto workspace_bounds = static_cast<QListView*>(list)->visualRect(alpha);
    QTest::mouseMove(list->viewport(), workspace_bounds.center());
    QApplication::processEvents();
    auto* add_task =
        sidebar->findChild<QAbstractButton*>(QStringLiteral("MintWorkspaceRowAddButton"));
    ASSERT_NE(add_task, nullptr);
    ASSERT_TRUE(add_task->isVisible());
    QTest::mouseClick(add_task, Qt::LeftButton);
    QApplication::processEvents();

    ASSERT_EQ(create_requests.count(), 1);
    EXPECT_EQ(create_requests.front().front().toString(), QStringLiteral("/projects/alpha"));
    EXPECT_EQ(controller.state().root, std::filesystem::path("/projects/alpha"));
    EXPECT_TRUE(controller.state().loading_tasks);

    runtime.callbacks.tasks_loaded(
        {{.id = "task-alpha", .question = "Alpha task", .status = "completed"}}, "/state-alpha");
    QApplication::processEvents();
    QApplication::processEvents();

    EXPECT_EQ(controller.state().root, std::filesystem::path("/projects/alpha"));
    EXPECT_TRUE(controller.state().task_id.empty());
    EXPECT_FALSE(controller.state().selected_task.has_value());
    EXPECT_GE(model->row_for_workspace("/projects/alpha"), 0);
    EXPECT_GE(model->row_for_task_id("task-alpha"), 0);
}

TEST(DesktopComponents, SelectingAmongManyTasksDoesNotResetTheSidebarModel) {
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1000, 700);
    page.show();
    QApplication::processEvents();

    std::vector<mint::desktop::TaskSummary> tasks;
    tasks.reserve(10'000);
    for (int row = 0; row < 10'000; ++row) {
        tasks.push_back({.id = "task-" + std::to_string(row),
                         .question = "Question " + std::to_string(row),
                         .status = "completed"});
    }
    runtime.callbacks.tasks_loaded(std::move(tasks), "/state");
    QApplication::processEvents();

    auto* list = page.findChild<fluent::collections::ListView*>();
    ASSERT_NE(list, nullptr);
    auto* model = static_cast<mint::ui::TaskListModel*>(list->model());
    ASSERT_NE(model, nullptr);
    int model_resets = 0;
    int data_changes = 0;
    int rows_inserted = 0;
    int rows_removed = 0;
    int layouts_changed = 0;
    QObject::connect(model, &QAbstractItemModel::modelReset, [&model_resets] { ++model_resets; });
    QObject::connect(model, &QAbstractItemModel::dataChanged, [&data_changes] { ++data_changes; });
    QObject::connect(model, &QAbstractItemModel::rowsInserted,
                     [&rows_inserted] { ++rows_inserted; });
    QObject::connect(model, &QAbstractItemModel::rowsRemoved, [&rows_removed] { ++rows_removed; });
    QObject::connect(model, &QAbstractItemModel::layoutChanged,
                     [&layouts_changed] { ++layouts_changed; });

    const auto last_task = model->index(model->row_for_task_id("task-9999"), 0);
    list->scrollTo(last_task, QAbstractItemView::PositionAtCenter);
    QApplication::processEvents();
    const auto last_task_bounds = static_cast<QListView*>(list)->visualRect(last_task);
    ASSERT_TRUE(last_task_bounds.isValid());
    ASSERT_TRUE(list->viewport()->rect().contains(last_task_bounds.center()));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, last_task_bounds.center());
    QApplication::processEvents();

    EXPECT_TRUE(list->uniformItemSizes());
    EXPECT_EQ(list->currentIndex().row(), model->row_for_task_id("task-9999"));
    EXPECT_EQ(list->currentIndex().row(), 10'000);
    EXPECT_EQ(model_resets, 0);
    EXPECT_EQ(data_changes, 0);
    EXPECT_EQ(rows_inserted, 0);
    EXPECT_EQ(rows_removed, 0);
    EXPECT_EQ(layouts_changed, 0);
}

TEST(DesktopComponents, RunningTaskUsesOneLightweightSidebarIndicator) {
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::TaskWorkspaceController controller(
        runtime, {.root = "/projects/alpha", .language = "en"});
    ASSERT_TRUE(controller.set_root("/projects/beta"));
    mint::desktop::ModelSettingsController model_settings(
        runtime, {.root = "/projects/beta", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1000, 700);
    page.show();
    QApplication::processEvents();

    ASSERT_TRUE(controller.start("Inspect the project"));
    QApplication::processEvents();

    auto* list = page.findChild<fluent::collections::ListView*>();
    auto* sidebar = page.findChild<mint::ui::TaskSidebar*>();
    auto* indicator = page.findChild<fluent::status_info::ProgressRing*>(
        QStringLiteral("MintSidebarRunningIndicator"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(sidebar, nullptr);
    ASSERT_NE(indicator, nullptr);
    auto* model = static_cast<mint::ui::TaskListModel*>(list->model());
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->running_row(), 2);
    EXPECT_EQ(page.findChildren<fluent::status_info::ProgressRing*>().size(), 1);
    EXPECT_TRUE(indicator->isVisible());
    EXPECT_TRUE(indicator->isActive());
    EXPECT_TRUE(indicator->isAnimationRunning());

    QSignalSpy workspace_navigation(sidebar, &mint::ui::TaskSidebar::workspace_selected);
    const auto workspace_bounds = static_cast<QListView*>(list)->visualRect(model->index(0, 0));
    ASSERT_TRUE(workspace_bounds.isValid());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, workspace_bounds.center());
    QApplication::processEvents();
    ASSERT_EQ(workspace_navigation.count(), 1);
    EXPECT_EQ(controller.state().root, std::filesystem::path("/projects/alpha"));
    model_settings.refresh();
    EXPECT_EQ(runtime.last_inspection.root, std::filesystem::path("/projects/alpha"));
    EXPECT_TRUE(controller.state().running);
    EXPECT_FALSE(controller.state().viewing_running_task());
    EXPECT_EQ(controller.state().running_root, std::filesystem::path("/projects/beta"));
    EXPECT_EQ(model->row_kind(model->running_row()), mint::ui::TaskListModel::RowKind::workspace);
    EXPECT_EQ(model->workspace_root_at(model->running_row()), QStringLiteral("/projects/beta"));
    EXPECT_TRUE(indicator->isVisible());
    EXPECT_FALSE(controller.start("A second task"));
}

TEST(DesktopComponents, SidebarRunningIndicatorIgnoresPassiveLoads) {
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1000, 700);
    page.show();
    QApplication::processEvents();

    auto* sidebar = page.findChild<mint::ui::TaskSidebar*>();
    auto* list =
        sidebar == nullptr
            ? nullptr
            : sidebar->findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));
    auto* indicator = page.findChild<fluent::status_info::ProgressRing*>(
        QStringLiteral("MintSidebarRunningIndicator"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(sidebar, nullptr);
    ASSERT_NE(indicator, nullptr);
    auto* model = static_cast<mint::ui::TaskListModel*>(list->model());
    ASSERT_NE(model, nullptr);

    EXPECT_TRUE(controller.state().loading_tasks);
    EXPECT_FALSE(controller.state().loading_history);
    EXPECT_TRUE(indicator->isHidden());
    EXPECT_FALSE(indicator->isActive());

    const auto workspace_index = model->index(model->row_for_workspace("/workspace"), 0);
    ASSERT_TRUE(workspace_index.isValid());
    const auto workspace_bounds = static_cast<QListView*>(list)->visualRect(workspace_index);
    ASSERT_TRUE(workspace_bounds.isValid());
    QSignalSpy new_task_requests(sidebar, &mint::ui::TaskSidebar::new_task_requested);
    QTest::mouseMove(list->viewport(), workspace_bounds.center());
    QApplication::processEvents();
    auto* add_task =
        sidebar->findChild<QAbstractButton*>(QStringLiteral("MintWorkspaceRowAddButton"));
    ASSERT_NE(add_task, nullptr);
    ASSERT_TRUE(add_task->isVisible());
    QTest::mouseClick(add_task, Qt::LeftButton);
    QApplication::processEvents();
    EXPECT_EQ(new_task_requests.count(), 1);

    runtime.callbacks.tasks_loaded({{.id = "task-1", .question = "Inspect", .status = "completed"}},
                                   "/state");
    QApplication::processEvents();

    EXPECT_FALSE(controller.state().loading_tasks);
    EXPECT_FALSE(controller.state().loading_history);
    EXPECT_TRUE(controller.state().task_id.empty());
    EXPECT_TRUE(indicator->isHidden());
    EXPECT_FALSE(indicator->isActive());

    ASSERT_TRUE(controller.select_task("task-1"));
    EXPECT_TRUE(controller.state().loading_history);
    runtime.callbacks.task_loaded("task-1", {});
    QApplication::processEvents();

    EXPECT_FALSE(controller.state().loading_tasks);
    EXPECT_FALSE(controller.state().loading_history);
    EXPECT_TRUE(indicator->isHidden());
    EXPECT_FALSE(indicator->isActive());
}

TEST(DesktopComponents, BrowsingDuringARunPreservesEachConversationsDraft) {
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1000, 700);
    page.show();
    QApplication::processEvents();
    runtime.callbacks.tasks_loaded({{.id = "task-1", .question = "First", .status = "completed"},
                                    {.id = "task-2", .question = "Second", .status = "completed"}},
                                   "/state");
    runtime.callbacks.task_loaded("task-1", {});
    auto* composer = page.findChild<mint::ui::TaskComposer*>();
    auto* input = page.findChild<QWidget*>(QStringLiteral("MintTaskInput"));
    auto* action = page.findChild<QAbstractButton*>(QStringLiteral("MintTaskAction"));
    ASSERT_NE(composer, nullptr);
    ASSERT_NE(input, nullptr);
    ASSERT_NE(action, nullptr);

    composer->set_draft(QStringLiteral("First draft"));
    ASSERT_TRUE(controller.select_task("task-2"));
    runtime.callbacks.task_loaded("task-2", {});
    EXPECT_TRUE(composer->draft().isEmpty());
    composer->set_draft(QStringLiteral("Second draft"));
    ASSERT_TRUE(controller.start("Inspect in the background"));
    runtime.callbacks.task_ready("run-live");

    ASSERT_TRUE(controller.select_task("task-1"));
    runtime.callbacks.task_loaded("task-1", {});
    EXPECT_EQ(composer->draft(), QStringLiteral("First draft"));
    EXPECT_TRUE(input->isEnabled());
    EXPECT_FALSE(action->isEnabled());
    EXPECT_TRUE(controller.state().running);
    composer->set_draft(QStringLiteral("First draft, edited while running"));

    ASSERT_TRUE(controller.select_task("task-2"));
    runtime.callbacks.task_loaded("task-2", {});
    EXPECT_EQ(composer->draft(), QStringLiteral("Second draft"));
    ASSERT_TRUE(controller.select_task("run-live"));
    EXPECT_FALSE(input->isEnabled());
    EXPECT_TRUE(action->isEnabled());
    EXPECT_TRUE(controller.state().viewing_running_task());
    runtime.callbacks.run_finished({.status = "completed", .answer = "Done"});
    ASSERT_TRUE(controller.select_task("task-1"));
    runtime.callbacks.task_loaded("task-1", {});
    EXPECT_EQ(composer->draft(), QStringLiteral("First draft, edited while running"));
    EXPECT_TRUE(input->isEnabled());
    EXPECT_TRUE(action->isEnabled());
}

TEST(DesktopModel, TimelineAppendsRowsWithoutResettingExistingEvents) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    mint::ui::RunTimelineModel model;
    mint::desktop::TaskWorkspaceState state;
    state.title = "Inspect";
    model.set_state(state);
    int rows_inserted = 0;
    int model_reset = 0;
    QObject::connect(&model, &QAbstractItemModel::rowsInserted,
                     [&rows_inserted] { ++rows_inserted; });
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&model_reset] { ++model_reset; });

    state.events.push_back(
        {.sequence = 1, .kind = mint::desktop::RunEventKind::model_requested, .turn = 1});
    EXPECT_TRUE(model.append_latest_event(state));
    state.events.push_back(
        {.sequence = 2, .kind = mint::desktop::RunEventKind::model_completed, .turn = 1});

    EXPECT_TRUE(model.append_latest_event(state));
    EXPECT_EQ(model.rowCount(), 3);
    EXPECT_EQ(rows_inserted, 1);
    EXPECT_EQ(model_reset, 0);
}

TEST(DesktopPresentation, ToolNamesStayOutOfUserFacingCopy) {
    const auto presentation = mint::ui::present_run_event(
        {.kind = mint::desktop::RunEventKind::tool_started, .name = "read_file"});

    EXPECT_FALSE(presentation.title.isEmpty());
    EXPECT_FALSE(presentation.title.contains(QStringLiteral("read_file")));
}

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

TEST(DesktopComponents, WorkspaceUsesResizableMicaNavigationAndEdgeToEdgeOpaqueContent) {
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::MintWindow window(controller, model_settings, preferences);
    window.resize(720, 580);
    window.show();
    QApplication::processEvents();

    auto* sidebar = window.findChild<mint::ui::TaskSidebar*>();
    auto* page = window.findChild<mint::ui::TaskWorkspacePage*>();
    auto* main_surface = window.findChild<QWidget*>(QStringLiteral("MintMainSurface"));
    auto* transcript = window.findChild<mint::ui::ConversationTranscript*>(
        QStringLiteral("MintConversationTranscript"));
    auto* task_title =
        window.findChild<fluent::textfields::Label*>(QStringLiteral("MintTitleBarTaskTitle"));
    auto* title_bar_surface = window.findChild<QWidget*>(QStringLiteral("MintTitleBarSurface"));
    auto* split_view =
        window.findChild<fluent::collections::SplitView*>(QStringLiteral("MintWorkspaceSplitView"));
    ASSERT_NE(sidebar, nullptr);
    ASSERT_NE(page, nullptr);
    ASSERT_NE(main_surface, nullptr);
    ASSERT_NE(transcript, nullptr);
    ASSERT_NE(task_title, nullptr);
    ASSERT_NE(title_bar_surface, nullptr);
    ASSERT_NE(split_view, nullptr);
    ASSERT_EQ(split_view->paneCount(), 2);
    EXPECT_EQ(split_view->paneAt(0), sidebar);
    EXPECT_EQ(split_view->paneAt(1), main_surface);
    EXPECT_FALSE(sidebar->autoFillBackground());
    EXPECT_TRUE(main_surface->autoFillBackground());
    EXPECT_EQ(main_surface->palette().color(QPalette::Window), mint::ui::theme::palette().canvas);
    EXPECT_TRUE(split_view->isPaneFill(main_surface));
    EXPECT_EQ(split_view->paneMinimumSize(sidebar), mint::ui::metrics::task_sidebar_minimum_width);
    EXPECT_EQ(split_view->paneMaximumSize(sidebar), mint::ui::metrics::task_sidebar_maximum_width);
    EXPECT_TRUE(sidebar->isVisible());
    EXPECT_GT(main_surface->width(), 0);
    EXPECT_EQ(window.findChild<QWidget*>(QStringLiteral("MintSidebarToggle")), nullptr);

    const auto handle = split_view->handleGeometry(0);
    ASSERT_TRUE(handle.isValid());
    EXPECT_EQ(handle.width(), mint::ui::metrics::workspace_split_handle_width);
    QImage split_pixels(split_view->size(), QImage::Format_ARGB32_Premultiplied);
    split_pixels.fill(Qt::transparent);
    split_view->render(&split_pixels);
    EXPECT_EQ(split_pixels.pixelColor(handle.center()), mint::ui::theme::palette().canvas);

    QMouseEvent hover_event(QEvent::MouseMove, handle.center(),
                            split_view->mapToGlobal(handle.center()), Qt::NoButton, Qt::NoButton,
                            Qt::NoModifier);
    QApplication::sendEvent(split_view, &hover_event);
    EXPECT_EQ(split_view->cursor().shape(), Qt::SplitHCursor);

    window.resize(1280, 800);
    QApplication::processEvents();

    EXPECT_TRUE(sidebar->isVisible());
    EXPECT_GT(main_surface->width(), 0);
    const auto previous_width = sidebar->width();
    split_view->setPanePreferredSize(sidebar, previous_width + 48);
    QApplication::processEvents();
    EXPECT_GT(sidebar->width(), previous_width);

    const auto expected_title_boundary =
        window.titleBar()
            ->mapFromGlobal(page->mapToGlobal(QPoint(page->navigation_surface_width(), 0)))
            .x();
    EXPECT_EQ(title_bar_surface->x(), expected_title_boundary);
    EXPECT_EQ(title_bar_surface->geometry().right(), window.titleBar()->width() - 1);
    EXPECT_TRUE(window.titleBar()->isAncestorOf(task_title));
    EXPECT_GE(task_title->mapTo(window.titleBar(), QPoint{}).x(),
              expected_title_boundary + mint::ui::metrics::space_md);
    QImage title_pixels(title_bar_surface->size(), QImage::Format_ARGB32_Premultiplied);
    title_pixels.fill(Qt::transparent);
    title_bar_surface->render(&title_pixels);
    const auto sample_x = title_bar_surface->width() / 2;
    EXPECT_EQ(title_pixels.pixelColor(sample_x, 0), mint::ui::theme::palette().canvas);
    EXPECT_EQ(title_pixels.pixelColor(sample_x, title_bar_surface->height() - 1),
              mint::ui::theme::palette().stroke);

    ASSERT_TRUE(controller.start("Inspect timeline"));
    for (std::size_t index = 0; index < 40; ++index) {
        runtime.callbacks.event_received({.sequence = index,
                                          .kind = mint::desktop::RunEventKind::model_completed,
                                          .turn = index});
    }
    QApplication::processEvents();
    const QRect transcript_rect(transcript->mapTo(main_surface, QPoint{}), transcript->size());
    EXPECT_EQ(transcript_rect.left(), 0);
    EXPECT_EQ(transcript_rect.top(), 0);
    EXPECT_EQ(transcript_rect.right(), main_surface->width() - 1);
    auto* scrollbar = transcript->verticalFluentScrollBar();
    ASSERT_NE(scrollbar, nullptr);
    const auto scrollbar_right =
        scrollbar->mapTo(main_surface, QPoint(scrollbar->width() - 1, 0)).x();
    EXPECT_GE(scrollbar_right, main_surface->width() - mint::ui::metrics::space_sm);
    const QRect scrollbar_rect(scrollbar->mapTo(transcript, QPoint{}), scrollbar->size());
    EXPECT_LE(scrollbar_rect.top(), mint::ui::metrics::space_xs);
    EXPECT_GE(scrollbar_rect.bottom(), transcript->height() - mint::ui::metrics::space_xs);
    EXPECT_EQ(transcript_rect.bottom(), main_surface->height() - 1);
    const auto readable_width =
        transcript->viewport()->width() - transcript->content_horizontal_padding() * 2;
    EXPECT_LE(readable_width, mint::ui::metrics::conversation_maximum_width);
    EXPECT_EQ(transcript->header(), nullptr);
    EXPECT_EQ(transcript->footer(), nullptr);
}

TEST(DesktopComponents, ComposerFloatsAboveTheConversationCanvas) {
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1200, 760);
    page.show();
    QApplication::processEvents();

    auto* canvas = page.findChild<QWidget*>(QStringLiteral("MintConversationCanvas"));
    auto* conversation = page.findChild<mint::ui::RunConversation*>();
    auto* composer = page.findChild<mint::ui::TaskComposer*>();
    auto* composer_layer = page.findChild<QWidget*>(QStringLiteral("MintComposerLayer"));
    auto* transcript = page.findChild<mint::ui::ConversationTranscript*>(
        QStringLiteral("MintConversationTranscript"));
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(conversation, nullptr);
    ASSERT_NE(composer, nullptr);
    ASSERT_NE(composer_layer, nullptr);
    ASSERT_NE(transcript, nullptr);

    const QRect conversation_rect(conversation->mapTo(canvas, QPoint{}), conversation->size());
    const QRect composer_rect(composer->mapTo(canvas, QPoint{}), composer->size());
    EXPECT_TRUE(conversation_rect.intersects(composer_rect));
    const QRect transcript_rect(transcript->mapTo(canvas, QPoint{}), transcript->size());
    EXPECT_EQ(transcript_rect, canvas->rect());
    EXPECT_EQ(composer_rect.bottom(), canvas->height() - mint::ui::metrics::space_lg - 1);
    EXPECT_LE(composer->width(), mint::ui::metrics::conversation_maximum_width);
    EXPECT_LE(qAbs(composer_rect.center().x() - canvas->rect().center().x()), 1);
    const auto spacer = transcript->model()->index(transcript->model()->rowCount() - 1, 0);
    EXPECT_EQ(spacer.data(mint::ui::RunTimelineModel::ItemKindRole).toInt(),
              static_cast<int>(mint::ui::RunTimelineModel::ItemKind::spacer));
    EXPECT_GE(spacer.data(mint::ui::RunTimelineModel::BottomInsetRole).toInt(),
              composer->height() + mint::ui::metrics::space_lg + mint::ui::metrics::space_md);

    ASSERT_TRUE(controller.start("Inspect the complete timeline"));
    for (std::size_t index = 0; index < 20; ++index) {
        runtime.callbacks.event_received({.sequence = index,
                                          .kind = mint::desktop::RunEventKind::model_completed,
                                          .turn = index});
    }
    runtime.callbacks.run_finished({.status = "completed", .answer = "Finished"});
    QApplication::processEvents();
    transcript->verticalScrollBar()->setValue(transcript->verticalScrollBar()->maximum());
    QApplication::processEvents();

    const auto result = transcript->model()->index(transcript->model()->rowCount() - 2, 0);
    const auto result_rect = static_cast<QAbstractItemView*>(transcript)->visualRect(result);
    const QRect composer_in_transcript(transcript->mapFromGlobal(composer->mapToGlobal(QPoint{})),
                                       composer->size());
    EXPECT_LE(result_rect.bottom(), composer_in_transcript.top() - mint::ui::metrics::space_md);
}

TEST(DesktopComponents, JumpToLatestTracksTheTranscriptTailAndComposer) {
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1200, 760);
    page.show();
    QApplication::processEvents();

    ASSERT_TRUE(controller.start("Inspect the timeline"));
    for (std::size_t index = 0; index < 40; ++index) {
        runtime.callbacks.event_received({.sequence = index,
                                          .kind = mint::desktop::RunEventKind::model_completed,
                                          .turn = index});
    }
    QApplication::processEvents();

    auto* canvas = page.findChild<QWidget*>(QStringLiteral("MintConversationCanvas"));
    auto* composer = page.findChild<mint::ui::TaskComposer*>();
    auto* transcript = page.findChild<mint::ui::ConversationTranscript*>(
        QStringLiteral("MintConversationTranscript"));
    auto* jump =
        page.findChild<fluent::basicinput::Button*>(QStringLiteral("MintJumpToLatestButton"));
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(composer, nullptr);
    ASSERT_NE(transcript, nullptr);
    ASSERT_NE(jump, nullptr);
    auto* scrollbar = transcript->verticalScrollBar();
    ASSERT_GT(scrollbar->maximum(), 0);
    EXPECT_TRUE(jump->isHidden());

    scrollbar->setValue(scrollbar->maximum() / 3);
    QApplication::processEvents();

    ASSERT_TRUE(jump->isVisible());
    const QRect composer_rect(composer->mapTo(canvas, QPoint{}), composer->size());
    EXPECT_LE(qAbs(jump->geometry().center().x() - canvas->rect().center().x()), 1);
    EXPECT_LE(qAbs(jump->geometry().center().y() - composer_rect.top()), 1);

    jump->click();
    QApplication::processEvents();

    EXPECT_EQ(scrollbar->value(), scrollbar->maximum());
    EXPECT_TRUE(jump->isHidden());
}

TEST(DesktopComponents, ProjectPickerOpensAsynchronouslyAndOwnsOneDialog) {
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1000, 700);
    page.show();
    QApplication::processEvents();

    auto* project = page.findChild<QAbstractButton*>(QStringLiteral("MintProjectButton"));
    ASSERT_NE(project, nullptr);

    project->click();
    QApplication::processEvents();

    const auto dialogs = page.findChildren<QFileDialog*>(QStringLiteral("MintProjectDialog"));
    ASSERT_EQ(dialogs.size(), 1);
    EXPECT_TRUE(dialogs.front()->isVisible());
    EXPECT_EQ(dialogs.front()->fileMode(), QFileDialog::Directory);
    EXPECT_TRUE(dialogs.front()->testOption(QFileDialog::ShowDirsOnly));
    dialogs.front()->reject();
    QApplication::processEvents();
}

TEST(DesktopComponents, TranscriptKeepsEventsAndAnswerInOneScrollOwner) {
    mint::ui::ConversationTranscript transcript;
    mint::desktop::TaskWorkspaceState state;
    state.title = "Inspect";
    state.events.push_back({.kind = mint::desktop::RunEventKind::tool_started,
                            .name = "read_file",
                            .evidence = "event evidence"});
    state.result = mint::desktop::TaskResult{.status = "completed", .answer = "Done"};
    QString selected_evidence;
    QObject::connect(
        &transcript, &mint::ui::ConversationTranscript::evidence_selected,
        [&selected_evidence](QString evidence) { selected_evidence = std::move(evidence); });

    transcript.render(state, mint::desktop::WorkspaceChange::all);
    transcript.select_first_evidence();

    EXPECT_TRUE(transcript.findChildren<QAbstractScrollArea*>().isEmpty());
    EXPECT_EQ(transcript.model()->rowCount(), 3);
    EXPECT_EQ(transcript.model()
                  ->index(transcript.model()->rowCount() - 1, 0)
                  .data(mint::ui::RunTimelineModel::ItemKindRole)
                  .toInt(),
              static_cast<int>(mint::ui::RunTimelineModel::ItemKind::spacer));
    EXPECT_TRUE(transcript.toPlainText().contains(QStringLiteral("Done")));
    EXPECT_EQ(selected_evidence, QStringLiteral("event evidence"));
}

class DesktopTranscriptPointer : public ::testing::Test {
  protected:
    void SetUp() override {
        state.title = "Inspect";
        state.events = {{.kind = mint::desktop::RunEventKind::tool_completed,
                         .name = "read_file",
                         .evidence = "first evidence"},
                        {.kind = mint::desktop::RunEventKind::tool_completed,
                         .name = "search_files",
                         .evidence = "second evidence"}};
        transcript.resize(800, 420);
        transcript.show();
        transcript.render(state, mint::desktop::WorkspaceChange::all);
        QApplication::processEvents();
    }

    [[nodiscard]] QRect evidence_rect(int row) const {
        const auto index = transcript.model()->index(row, 0);
        QStyleOptionViewItem option;
        option.initFrom(&transcript);
        option.widget = &transcript;
        option.rect = static_cast<const QListView*>(&transcript)->visualRect(index);
        const auto* delegate =
            static_cast<mint::ui::RunTimelineDelegate*>(transcript.itemDelegate());
        return delegate->evidence_action_rect(option, index);
    }

    mint::ui::ConversationTranscript transcript;
    mint::desktop::TaskWorkspaceState state;
};

TEST_F(DesktopTranscriptPointer, RightClickDoesNotOpenEvidence) {
    QSignalSpy evidence(&transcript, &mint::ui::ConversationTranscript::evidence_selected);
    ASSERT_TRUE(evidence_rect(0).isValid());

    QTest::mouseClick(transcript.viewport(), Qt::RightButton, Qt::NoModifier,
                      evidence_rect(0).center());
    QApplication::processEvents();

    EXPECT_TRUE(evidence.isEmpty());
}

TEST_F(DesktopTranscriptPointer, ReleaseOverEvidenceRequiresPressOnTheSameAction) {
    QSignalSpy evidence(&transcript, &mint::ui::ConversationTranscript::evidence_selected);
    const auto first_action = evidence_rect(0);
    const auto second_action = evidence_rect(1);
    ASSERT_TRUE(first_action.isValid());
    ASSERT_TRUE(second_action.isValid());

    QTest::mousePress(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(32, first_action.center().y()));
    QTest::mouseRelease(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                        first_action.center());
    QApplication::processEvents();
    EXPECT_TRUE(evidence.isEmpty());

    QTest::mousePress(transcript.viewport(), Qt::LeftButton, Qt::NoModifier, first_action.center());
    QTest::mouseRelease(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                        second_action.center());
    QApplication::processEvents();
    EXPECT_TRUE(evidence.isEmpty());
}

TEST_F(DesktopTranscriptPointer, EachEvidenceClickPreservesTheInheritedClickContract) {
    QSignalSpy evidence(&transcript, &mint::ui::ConversationTranscript::evidence_selected);
    QSignalSpy clicked(&transcript, &QAbstractItemView::clicked);
    QSignalSpy row_clicked(&transcript, &fluent::collections::ListView::itemClicked);

    QTest::mouseClick(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                      evidence_rect(0).center());
    QApplication::processEvents();

    ASSERT_EQ(evidence.count(), 1);
    EXPECT_EQ(evidence.front().front().toString(), QStringLiteral("first evidence"));
    EXPECT_EQ(clicked.count(), 1);
    EXPECT_EQ(row_clicked.count(), 1);

    QTest::mouseClick(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                      evidence_rect(1).center());
    QApplication::processEvents();

    ASSERT_EQ(evidence.count(), 2);
    EXPECT_EQ(evidence.back().front().toString(), QStringLiteral("second evidence"));
    EXPECT_EQ(clicked.count(), 2);
    EXPECT_EQ(row_clicked.count(), 2);
}

TEST_F(DesktopTranscriptPointer, ModelResetCancelsThePendingEvidenceAction) {
    QSignalSpy evidence(&transcript, &mint::ui::ConversationTranscript::evidence_selected);
    QSignalSpy reset(transcript.model(), &QAbstractItemModel::modelReset);
    QTest::mousePress(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                      evidence_rect(0).center());
    state.events.front().evidence = "replacement evidence";
    transcript.render(state, mint::desktop::WorkspaceChange::selection);
    QApplication::processEvents();
    ASSERT_EQ(reset.count(), 1);

    QTest::mouseRelease(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                        evidence_rect(0).center());
    QApplication::processEvents();

    EXPECT_TRUE(evidence.isEmpty());
}

TEST(DesktopComponents, ConversationKeepsOneScrollViewportAndOverlaysConditionalActions) {
    mint::ui::RunConversation conversation;
    conversation.resize(900, 700);
    conversation.show();
    mint::desktop::TaskWorkspaceState state;
    state.title = "Inspect";
    conversation.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();

    auto* transcript = conversation.findChild<mint::ui::ConversationTranscript*>(
        QStringLiteral("MintConversationTranscript"));
    auto* action_layer =
        conversation.findChild<QWidget*>(QStringLiteral("MintConversationActionLayer"));
    ASSERT_NE(transcript, nullptr);
    ASSERT_NE(action_layer, nullptr);
    EXPECT_EQ(transcript->header(), nullptr);
    EXPECT_EQ(transcript->footer(), nullptr);
    EXPECT_TRUE(action_layer->isHidden());
    const auto transcript_height = transcript->height();

    state.running = true;
    state.selected_task_running = true;
    state.approval = mint::desktop::ApprovalRequest{.kind = mint::desktop::ApprovalKind::command,
                                                    .program = "cmake",
                                                    .working_directory = "/workspace"};
    conversation.render(state, mint::desktop::WorkspaceChange::approval);
    QApplication::processEvents();
    EXPECT_FALSE(action_layer->isHidden());
    EXPECT_EQ(transcript->height(), transcript_height);
    EXPECT_EQ(transcript->footer(), nullptr);
    EXPECT_TRUE(transcript->geometry().intersects(action_layer->geometry()));

    state.approval.reset();
    conversation.render(state, mint::desktop::WorkspaceChange::approval);
    QApplication::processEvents();
    EXPECT_TRUE(action_layer->isHidden());
    EXPECT_EQ(transcript->footer(), nullptr);

    state.approval = mint::desktop::ApprovalRequest{.kind = mint::desktop::ApprovalKind::command,
                                                    .program = "cmake",
                                                    .working_directory = "/workspace"};
    state.selected_task_running = false;
    conversation.render(state, mint::desktop::WorkspaceChange::selection);
    QApplication::processEvents();
    EXPECT_TRUE(action_layer->isHidden());
    EXPECT_EQ(transcript->height(), transcript_height);
}

TEST(DesktopComponents, AppendedEventKeepsTheReadersScrollPosition) {
    mint::ui::ConversationTranscript transcript;
    transcript.resize(540, 220);
    transcript.show();
    mint::desktop::TaskWorkspaceState state;
    state.title = "Inspect";
    for (std::size_t index = 0; index < 40; ++index) {
        state.events.push_back({.sequence = index,
                                .kind = mint::desktop::RunEventKind::model_completed,
                                .turn = index});
    }
    transcript.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();
    auto* scrollbar = transcript.verticalScrollBar();
    ASSERT_GT(scrollbar->maximum(), 0);
    scrollbar->setValue(scrollbar->maximum() / 3);
    const auto previous_value = scrollbar->value();
    const auto previous_text_size = transcript.toPlainText().size();

    state.events.push_back({.sequence = state.events.size(),
                            .kind = mint::desktop::RunEventKind::tool_completed,
                            .name = "read_file",
                            .evidence = "new evidence"});
    transcript.render(state, mint::desktop::WorkspaceChange::event_appended);
    QApplication::processEvents();

    EXPECT_EQ(scrollbar->value(), previous_value);
    EXPECT_GT(transcript.toPlainText().size(), previous_text_size);
}

TEST(DesktopComponents, EvidenceLinkOpensAsASameWindowDrawer) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1200, 760);
    page.show();
    QApplication::processEvents();

    ASSERT_TRUE(controller.start("Inspect"));
    runtime.callbacks.event_received({.kind = mint::desktop::RunEventKind::tool_completed,
                                      .name = "read_file",
                                      .evidence = "event evidence"});
    QApplication::processEvents();

    auto* transcript = page.findChild<mint::ui::ConversationTranscript*>(
        QStringLiteral("MintConversationTranscript"));
    auto* drawer =
        page.findChild<fluent::collections::DrawerView*>(QStringLiteral("MintEvidenceDrawer"));
    ASSERT_NE(transcript, nullptr);
    ASSERT_NE(drawer, nullptr);
    drawer->setAnimationEnabled(false);

    transcript->select_first_evidence();
    QApplication::processEvents();

    EXPECT_TRUE(drawer->isOpen());
}

TEST(DesktopComponents, ResultEvidenceRemainsProgressivelyDisclosed) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1200, 760);
    page.show();
    QApplication::processEvents();

    ASSERT_TRUE(controller.start("Inspect"));
    runtime.callbacks.run_finished(
        {.status = "completed", .answer = "Done", .unified_diff = "diff --git a/a b/a"});
    QApplication::processEvents();

    auto* drawer =
        page.findChild<fluent::collections::DrawerView*>(QStringLiteral("MintEvidenceDrawer"));
    auto* transcript = page.findChild<mint::ui::ConversationTranscript*>(
        QStringLiteral("MintConversationTranscript"));
    ASSERT_NE(drawer, nullptr);
    ASSERT_NE(transcript, nullptr);
    EXPECT_EQ(page.findChild<QWidget*>(QStringLiteral("MintEvidenceButton")), nullptr);
    EXPECT_FALSE(drawer->isOpen());
    EXPECT_TRUE(transcript->toPlainText().contains(QStringLiteral("View changes")));

    drawer->setAnimationEnabled(false);
    transcript->select_first_evidence();
    QApplication::processEvents();
    EXPECT_TRUE(drawer->isOpen());
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
