#include "desktop_component_test_support.hpp"
#include "ui/components/conversation_transcript.hpp"
#include "ui/components/task_composer.hpp"
#include "ui/components/task_sidebar.hpp"
#include "ui/models/task_list_model.hpp"
#include "ui/pages/task_workspace_page.hpp"
#include "ui/support/localized_text.hpp"

#include <FluentQt/Collections.h>
#include <FluentQt/StatusInfo.h>
#include <FluentQt/TextFields.h>

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QFileDialog>
#include <QListView>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

using mint::test::ComponentRuntime;
using mint::test::MemoryPreferencesRepository;

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

} // namespace
