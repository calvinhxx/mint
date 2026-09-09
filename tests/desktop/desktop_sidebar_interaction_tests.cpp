#include "ui/components/task_sidebar.hpp"
#include "ui/components/workspace_split_view.hpp"
#include "ui/delegates/task_item_delegate.hpp"
#include "ui/models/task_list_model.hpp"
#include "ui/support/localized_text.hpp"

#include <FluentQt/BasicInput.h>
#include <FluentQt/Collections.h>
#include <FluentQt/Foundation.h>
#include <FluentQt/StatusInfo.h>

#include <QAccessible>
#include <QApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QVariantAnimation>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

TEST(DesktopSidebarModel, ActiveWorkspaceDisclosureRequiresAKnownVisibleTask) {
    mint::ui::TaskListModel model;

    model.set_navigation({"/projects/alpha", "/projects/beta"}, "/projects/beta", {}, {}, false,
                         false, {}, {}, true);

    const auto alpha = model.index(model.row_for_workspace("/projects/alpha"), 0);
    const auto beta = model.index(model.row_for_workspace("/projects/beta"), 0);
    ASSERT_TRUE(alpha.isValid());
    ASSERT_TRUE(beta.isValid());
    EXPECT_TRUE(alpha.data(mint::ui::TaskListModel::WorkspaceExpandableRole).toBool());
    EXPECT_FALSE(beta.data(mint::ui::TaskListModel::WorkspaceExpandableRole).toBool());
    EXPECT_FALSE(beta.data(mint::ui::TaskListModel::ExpandedWorkspaceRole).toBool());

    const std::vector<mint::desktop::WorkspaceTaskCounts> cached_empty_counts{
        {.root = "/projects/alpha"}};
    model.set_navigation({"/projects/alpha", "/projects/beta"}, "/projects/beta", {}, {}, false,
                         false, {}, {}, true, cached_empty_counts);
    const auto empty_alpha = model.index(model.row_for_workspace("/projects/alpha"), 0);
    ASSERT_TRUE(empty_alpha.isValid());
    EXPECT_FALSE(empty_alpha.data(mint::ui::TaskListModel::WorkspaceExpandableRole).toBool());

    const std::vector<mint::desktop::WorkspaceTaskCounts> cached_counts{
        {.root = "/projects/alpha", .unarchived = 1}};
    model.set_navigation({"/projects/alpha", "/projects/beta"}, "/projects/beta", {}, {}, false,
                         false, {}, {}, true, cached_counts);
    const auto populated_alpha = model.index(model.row_for_workspace("/projects/alpha"), 0);
    ASSERT_TRUE(populated_alpha.isValid());
    EXPECT_TRUE(populated_alpha.data(mint::ui::TaskListModel::WorkspaceExpandableRole).toBool());

    const std::vector<mint::desktop::TaskSummary> tasks{
        {.id = "task-1", .question = "Inspect", .status = "completed"}};
    model.set_navigation({"/projects/alpha", "/projects/beta"}, "/projects/beta", tasks, {}, false,
                         false, {}, {}, true);

    const auto populated_beta = model.index(model.row_for_workspace("/projects/beta"), 0);
    ASSERT_TRUE(populated_beta.isValid());
    EXPECT_TRUE(populated_beta.data(mint::ui::TaskListModel::WorkspaceExpandableRole).toBool());
    EXPECT_TRUE(populated_beta.data(mint::ui::TaskListModel::ExpandedWorkspaceRole).toBool());

    model.set_navigation({"/projects/alpha", "/projects/beta"}, "/projects/beta", tasks, {}, true,
                         false, {}, {}, true);

    const auto archived_beta = model.index(model.row_for_workspace("/projects/beta"), 0);
    ASSERT_TRUE(archived_beta.isValid());
    EXPECT_FALSE(archived_beta.data(mint::ui::TaskListModel::WorkspaceExpandableRole).toBool());
    EXPECT_FALSE(archived_beta.data(mint::ui::TaskListModel::ExpandedWorkspaceRole).toBool());
}

TEST(DesktopSidebarInteractions, HeaderAndRowsUseBoundedNativeFluentActions) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    mint::ui::TaskSidebar sidebar;
    sidebar.resize(280, 480);
    sidebar.show();

    mint::desktop::TaskWorkspaceState state;
    state.root = "/projects/beta";
    state.workspace_roots = {"/projects/alpha", "/projects/beta"};
    state.tasks.reserve(10'000);
    for (int row = 0; row < 10'000; ++row) {
        state.tasks.push_back({.id = "task-" + std::to_string(row),
                               .question = "Question " + std::to_string(row),
                               .status = "completed"});
    }
    state.task_id = "task-0";
    state.title = "Question 0";
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();

    auto* collection = sidebar.findChild<fluent::basicinput::DropDownButton*>(
        QStringLiteral("MintTaskCollectionButton"));
    auto* add_workspace =
        sidebar.findChild<fluent::basicinput::Button*>(QStringLiteral("MintProjectButton"));
    auto* add_task =
        sidebar.findChild<fluent::basicinput::Button*>(QStringLiteral("MintWorkspaceRowAddButton"));
    auto* task_menu =
        sidebar.findChild<fluent::basicinput::Button*>(QStringLiteral("MintTaskRowMenuButton"));
    auto* list = sidebar.findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));
    ASSERT_NE(collection, nullptr);
    ASSERT_NE(add_workspace, nullptr);
    ASSERT_NE(add_task, nullptr);
    ASSERT_NE(task_menu, nullptr);
    ASSERT_NE(list, nullptr);
    ASSERT_NE(collection->menu(), nullptr);
    EXPECT_EQ(collection->text(), QStringLiteral("Workspaces"));
    EXPECT_EQ(collection->menu()->actions().size(), 1);
    EXPECT_EQ(collection->menu()->actions().front()->text(), QStringLiteral("Archived"));
    EXPECT_NE(collection, task_menu);
    EXPECT_NE(add_workspace, add_task);
    QSignalSpy archived_view(&sidebar, &mint::ui::TaskSidebar::archived_view_requested);
    collection->menu()->actions().front()->trigger();
    ASSERT_EQ(archived_view.count(), 1);
    EXPECT_TRUE(archived_view.front().front().toBool());

    auto* model = static_cast<mint::ui::TaskListModel*>(list->model());
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->rowCount(), 10'002);
    EXPECT_EQ(sidebar.findChildren<fluent::basicinput::DropDownButton*>().size(), 1);
    EXPECT_EQ(sidebar.findChildren<fluent::status_info::ProgressRing*>().size(), 1);

    const auto alpha = model->index(model->row_for_workspace("/projects/alpha"), 0);
    const auto alpha_bounds = static_cast<QListView*>(list)->visualRect(alpha);
    ASSERT_TRUE(alpha_bounds.isValid());
    EXPECT_EQ(list->indexWidget(alpha), nullptr);
    QTest::mouseMove(list->viewport(), alpha_bounds.center());
    QTRY_VERIFY(add_task->isVisible());
    EXPECT_EQ(add_task->parentWidget(), &sidebar);
    EXPECT_TRUE(alpha_bounds.contains(
        list->viewport()->mapFromGlobal(add_task->mapToGlobal(add_task->rect().center()))));
    EXPECT_TRUE(task_menu->isHidden());
    auto* add_task_accessible = QAccessible::queryAccessibleInterface(add_task);
    ASSERT_NE(add_task_accessible, nullptr);
    EXPECT_EQ(add_task_accessible->role(), QAccessible::Button);
    EXPECT_FALSE(add_task_accessible->text(QAccessible::Name).isEmpty());

    QSignalSpy create_task(&sidebar, &mint::ui::TaskSidebar::new_task_requested);
    QSignalSpy workspace_navigation(&sidebar, &mint::ui::TaskSidebar::workspace_selected);
    QSignalSpy list_click(list, &QAbstractItemView::clicked);
    QTest::mouseClick(add_task, Qt::LeftButton);
    ASSERT_EQ(create_task.count(), 1);
    EXPECT_EQ(create_task.front().front().toString(), QStringLiteral("/projects/alpha"));
    EXPECT_TRUE(workspace_navigation.isEmpty());
    EXPECT_TRUE(list_click.isEmpty());

    const auto first_task = model->index(model->row_for_task_id("task-0"), 0);
    const auto task_bounds = static_cast<QListView*>(list)->visualRect(first_task);
    ASSERT_TRUE(task_bounds.isValid());
    EXPECT_EQ(list->indexWidget(first_task), nullptr);
    QTest::mouseMove(list->viewport(), task_bounds.center());
    QTRY_VERIFY(task_menu->isVisible());
    EXPECT_EQ(task_menu->parentWidget(), &sidebar);
    EXPECT_TRUE(task_bounds.contains(
        list->viewport()->mapFromGlobal(task_menu->mapToGlobal(task_menu->rect().center()))));
    EXPECT_TRUE(add_task->isHidden());
    auto* task_menu_accessible = QAccessible::queryAccessibleInterface(task_menu);
    ASSERT_NE(task_menu_accessible, nullptr);
    EXPECT_EQ(task_menu_accessible->role(), QAccessible::Button);
    EXPECT_FALSE(task_menu_accessible->text(QAccessible::Name).isEmpty());
    QSignalSpy task_navigation(&sidebar, &mint::ui::TaskSidebar::task_selected);
    QSignalSpy pin_task(&sidebar, &mint::ui::TaskSidebar::task_pin_requested);
    auto popup_observed = false;
    QTimer::singleShot(0, &sidebar, [&] {
        auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (popup == nullptr || popup->actions().size() != 2) {
            if (popup != nullptr) {
                popup->close();
            }
            return;
        }
        popup_observed = true;
        popup->actions().front()->trigger();
        popup->close();
    });
    QTest::mouseClick(task_menu, Qt::LeftButton);
    QTRY_VERIFY(popup_observed);
    EXPECT_TRUE(task_navigation.isEmpty());
    ASSERT_EQ(pin_task.count(), 1);
    EXPECT_EQ(pin_task.front().front().toString(), QStringLiteral("task-0"));
}

TEST(DesktopSidebarInteractions, WorkspaceDisclosureAndTaskNavigationHaveSeparateHitTargets) {
    mint::ui::TaskSidebar sidebar;
    sidebar.resize(280, 360);
    sidebar.show();

    mint::desktop::TaskWorkspaceState state;
    state.root = "/workspace";
    state.workspace_roots = {"/workspace"};
    state.tasks = {{.id = "task-1", .question = "First", .status = "completed"},
                   {.id = "task-2", .question = "Second", .status = "completed"}};
    state.task_id = "task-1";
    state.title = "First";
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();

    auto* list = sidebar.findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));
    auto* model = list == nullptr ? nullptr : static_cast<mint::ui::TaskListModel*>(list->model());
    ASSERT_NE(list, nullptr);
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->rowCount(), 3);

    QSignalSpy workspace_navigation(&sidebar, &mint::ui::TaskSidebar::workspace_selected);
    const auto workspace = model->index(model->row_for_workspace("/workspace"), 0);
    const auto workspace_bounds = static_cast<QListView*>(list)->visualRect(workspace);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, workspace_bounds.center());
    ASSERT_EQ(workspace_navigation.count(), 1);
    sidebar.render(state, mint::desktop::WorkspaceChange::tasks);
    QApplication::processEvents();
    EXPECT_EQ(model->rowCount(), 1);

    const auto collapsed_workspace = model->index(0, 0);
    const auto collapsed_bounds = static_cast<QListView*>(list)->visualRect(collapsed_workspace);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, collapsed_bounds.center());
    sidebar.render(state, mint::desktop::WorkspaceChange::tasks);
    QApplication::processEvents();
    ASSERT_EQ(model->rowCount(), 3);

    QSignalSpy task_navigation(&sidebar, &mint::ui::TaskSidebar::task_selected);
    const auto second_task = model->index(model->row_for_task_id("task-2"), 0);
    const auto task_bounds = static_cast<QListView*>(list)->visualRect(second_task);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, task_bounds.center());
    ASSERT_EQ(task_navigation.count(), 1);
    EXPECT_EQ(task_navigation.front().front().toString(), QStringLiteral("task-2"));
}

TEST(DesktopSidebarInteractions, RenderingKeepsQtSelectionNotificationsWithoutEchoingIntent) {
    mint::ui::TaskSidebar sidebar;
    sidebar.resize(280, 360);
    sidebar.show();
    auto* list = sidebar.findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));
    ASSERT_NE(list, nullptr);
    QSignalSpy current_changed(list->selectionModel(), &QItemSelectionModel::currentChanged);
    QSignalSpy task_selected(&sidebar, &mint::ui::TaskSidebar::task_selected);

    mint::desktop::TaskWorkspaceState state;
    state.root = "/workspace";
    state.workspace_roots = {"/workspace"};
    state.tasks = {{.id = "task-1", .question = "First", .status = "completed"},
                   {.id = "task-2", .question = "Second", .status = "completed"}};
    state.task_id = "task-1";
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();
    EXPECT_FALSE(current_changed.isEmpty());
    EXPECT_TRUE(task_selected.isEmpty());

    current_changed.clear();
    state.task_id = "task-2";
    sidebar.render(state, mint::desktop::WorkspaceChange::selection);
    QApplication::processEvents();
    ASSERT_EQ(current_changed.count(), 1);
    EXPECT_EQ(list->currentIndex().data(mint::ui::TaskListModel::TaskIdRole).toString(),
              QStringLiteral("task-2"));
    EXPECT_TRUE(task_selected.isEmpty());

    QTest::keyClick(list, Qt::Key_Up);
    ASSERT_EQ(task_selected.count(), 1);
    EXPECT_EQ(task_selected.front().front().toString(), QStringLiteral("task-1"));
}

TEST(DesktopSidebarInteractions, WorkspaceDisclosureRequiresAMatchingPointerPressAndRelease) {
    mint::ui::TaskSidebar sidebar;
    sidebar.resize(280, 360);
    sidebar.show();

    mint::desktop::TaskWorkspaceState state;
    state.root = "/workspace";
    state.workspace_roots = {"/workspace"};
    state.tasks = {{.id = "task-1", .question = "First", .status = "completed"},
                   {.id = "task-2", .question = "Second", .status = "completed"}};
    state.task_id = "task-1";
    state.title = "First";
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();

    auto* list = sidebar.findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));
    auto* model = list == nullptr ? nullptr : static_cast<mint::ui::TaskListModel*>(list->model());
    ASSERT_NE(list, nullptr);
    ASSERT_NE(model, nullptr);
    QSignalSpy workspace_navigation(&sidebar, &mint::ui::TaskSidebar::workspace_selected);
    QSignalSpy task_navigation(&sidebar, &mint::ui::TaskSidebar::task_selected);
    QSignalSpy list_click(list, &QAbstractItemView::clicked);
    QSignalSpy row_click(list, &fluent::collections::ListView::itemClicked);
    QObject::connect(&sidebar, &mint::ui::TaskSidebar::workspace_selected, &sidebar,
                     [&] { sidebar.render(state, mint::desktop::WorkspaceChange::tasks); });

    const auto workspace = model->index(model->row_for_workspace("/workspace"), 0);
    const auto second_task = model->index(model->row_for_task_id("task-2"), 0);
    const auto workspace_point = static_cast<QListView*>(list)->visualRect(workspace).center();
    const auto task_point = static_cast<QListView*>(list)->visualRect(second_task).center();

    QTest::mousePress(list->viewport(), Qt::LeftButton, Qt::NoModifier, task_point);
    QTest::mouseRelease(list->viewport(), Qt::LeftButton, Qt::NoModifier, workspace_point);
    QApplication::processEvents();

    EXPECT_TRUE(workspace_navigation.isEmpty());
    EXPECT_TRUE(task_navigation.isEmpty());
    EXPECT_TRUE(list_click.isEmpty());
    EXPECT_TRUE(row_click.isEmpty());
    EXPECT_EQ(model->rowCount(), 3);
    EXPECT_EQ(list->currentIndex().data(mint::ui::TaskListModel::TaskIdRole).toString(),
              QStringLiteral("task-1"));

    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, workspace_point);
    QApplication::processEvents();

    EXPECT_EQ(workspace_navigation.count(), 1);
    EXPECT_EQ(list_click.count(), 1);
    EXPECT_EQ(row_click.count(), 1);
    EXPECT_EQ(model->rowCount(), 1);

    const auto collapsed_point =
        static_cast<QListView*>(list)->visualRect(model->index(0, 0)).center();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, collapsed_point);
    QApplication::processEvents();

    EXPECT_EQ(workspace_navigation.count(), 2);
    EXPECT_EQ(list_click.count(), 2);
    EXPECT_EQ(row_click.count(), 2);
    EXPECT_EQ(model->rowCount(), 3);
    EXPECT_TRUE(task_navigation.isEmpty());
}

TEST(DesktopSidebarInteractions, RunningRowKeepsOneProgressRingAndNoMenuButton) {
    mint::ui::TaskSidebar sidebar;
    sidebar.resize(280, 320);
    sidebar.show();

    mint::desktop::TaskWorkspaceState state;
    state.root = "/workspace";
    state.workspace_roots = {"/workspace"};
    state.tasks = {{.id = "task-1", .question = "Inspect", .status = "completed"}};
    state.task_id = "task-1";
    state.title = "Inspect";
    state.running = true;
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();

    auto* list = sidebar.findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));
    auto* model = list == nullptr ? nullptr : static_cast<mint::ui::TaskListModel*>(list->model());
    auto* task_menu =
        sidebar.findChild<fluent::basicinput::Button*>(QStringLiteral("MintTaskRowMenuButton"));
    auto* indicator = sidebar.findChild<fluent::status_info::ProgressRing*>(
        QStringLiteral("MintSidebarRunningIndicator"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(model, nullptr);
    ASSERT_NE(task_menu, nullptr);
    ASSERT_NE(indicator, nullptr);
    const auto running = model->index(model->running_row(), 0);
    const auto bounds = static_cast<QListView*>(list)->visualRect(running);
    ASSERT_TRUE(bounds.isValid());

    QTest::mouseMove(list->viewport(), bounds.center());
    QApplication::processEvents();

    EXPECT_TRUE(task_menu->isHidden());
    EXPECT_TRUE(indicator->isVisible());
    EXPECT_TRUE(indicator->isActive());
    EXPECT_EQ(sidebar.findChildren<fluent::status_info::ProgressRing*>().size(), 1);
}

TEST(DesktopSidebarInteractions, RecycledRowControlsStayInsideTheScrollingViewport) {
    mint::ui::TaskSidebar sidebar;
    sidebar.resize(280, 240);
    sidebar.show();

    mint::desktop::TaskWorkspaceState state;
    state.root = "/workspace";
    state.workspace_roots = {"/workspace"};
    for (int row = 0; row < 20; ++row) {
        state.tasks.push_back({.id = "task-" + std::to_string(row),
                               .question = "Question " + std::to_string(row),
                               .status = "completed"});
    }
    state.task_id = "task-0";
    state.title = "Question 0";
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();

    auto* list = sidebar.findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));
    auto* model = list == nullptr ? nullptr : static_cast<mint::ui::TaskListModel*>(list->model());
    auto* add_task =
        sidebar.findChild<fluent::basicinput::Button*>(QStringLiteral("MintWorkspaceRowAddButton"));
    auto* indicator = sidebar.findChild<fluent::status_info::ProgressRing*>(
        QStringLiteral("MintSidebarRunningIndicator"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(model, nullptr);
    ASSERT_NE(add_task, nullptr);
    ASSERT_NE(indicator, nullptr);

    const auto workspace = model->index(model->row_for_workspace("/workspace"), 0);
    QTest::mouseMove(list->viewport(),
                     static_cast<QListView*>(list)->visualRect(workspace).center());
    QTRY_VERIFY(add_task->isVisible());

    list->verticalScrollBar()->setValue(20);
    QApplication::processEvents();
    const auto clipped_workspace = static_cast<QListView*>(list)->visualRect(workspace);
    ASSERT_LT(clipped_workspace.top(), 0);
    ASSERT_GT(clipped_workspace.bottom(), 0);
    QTest::mouseMove(list->viewport(),
                     clipped_workspace.intersected(list->viewport()->rect()).center());
    QApplication::processEvents();
    EXPECT_TRUE(add_task->isHidden());

    state.running = true;
    sidebar.render(state, mint::desktop::WorkspaceChange::run_state);
    list->verticalScrollBar()->setValue(0);
    QApplication::processEvents();
    EXPECT_TRUE(indicator->isVisible());

    const auto running = model->index(model->running_row(), 0);
    const auto running_bounds = static_cast<QListView*>(list)->visualRect(running);
    list->verticalScrollBar()->setValue(running_bounds.top() + 20);
    QApplication::processEvents();
    const auto clipped_running = static_cast<QListView*>(list)->visualRect(running);
    ASSERT_LT(clipped_running.top(), 0);
    ASSERT_GT(clipped_running.bottom(), 0);
    EXPECT_TRUE(indicator->isHidden());
    EXPECT_FALSE(indicator->isActive());

    list->verticalScrollBar()->setValue(0);
    QApplication::processEvents();
    EXPECT_TRUE(indicator->isVisible());
    EXPECT_TRUE(indicator->isActive());
}

TEST(DesktopSidebarModel, AppendsAndDisclosurePreservePersistentWorkspaceIndexes) {
    mint::ui::TaskListModel model;
    std::vector<mint::desktop::TaskSummary> tasks{
        {.id = "task-1", .question = "First", .status = "completed"}};
    model.set_navigation({"/projects/alpha/", "/projects/alpha", "/projects/beta/"},
                         "/projects/alpha/", tasks, {}, false, false, {}, {});
    ASSERT_EQ(model.rowCount(), 3);
    EXPECT_EQ(model.index(0, 0).data(mint::ui::TaskListModel::WorkspaceNameRole).toString(),
              QStringLiteral("alpha"));
    QPersistentModelIndex alpha(model.index(model.row_for_workspace("/projects/alpha"), 0));
    QPersistentModelIndex beta(model.index(model.row_for_workspace("/projects/beta/"), 0));
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);

    tasks.push_back({.id = "task-2", .question = "Second", .status = "completed"});
    model.set_navigation({"/projects/alpha", "/projects/beta"}, "/projects/alpha", tasks, {}, false,
                         false, {}, {});
    EXPECT_TRUE(resets.isEmpty());
    EXPECT_EQ(inserted.count(), 1);
    EXPECT_TRUE(removed.isEmpty());
    EXPECT_TRUE(alpha.isValid());
    EXPECT_TRUE(beta.isValid());
    EXPECT_EQ(beta.row(), 3);

    model.set_navigation({"/projects/alpha", "/projects/beta"}, "/projects/alpha", tasks, {}, false,
                         false, {}, {}, false);
    EXPECT_TRUE(resets.isEmpty());
    EXPECT_EQ(removed.count(), 1);
    EXPECT_TRUE(alpha.isValid());
    EXPECT_TRUE(beta.isValid());
    EXPECT_EQ(beta.row(), 1);
}

TEST(DesktopSidebarModel, BackgroundRunBelongsToItsWorkspaceAndCannotBeRemoved) {
    mint::ui::TaskListModel model;
    model.set_navigation({"/projects/alpha", "/projects/beta"}, "/projects/beta",
                         {{.id = "history", .question = "Past answer", .status = "completed"}}, {},
                         false, true, "run-1", "Inspect alpha", true, {}, "/projects/alpha");
    const auto running = model.index(model.running_row(), 0);
    ASSERT_TRUE(running.isValid());
    EXPECT_EQ(model.row_kind(running.row()), mint::ui::TaskListModel::RowKind::workspace);
    EXPECT_EQ(running.data(mint::ui::TaskListModel::WorkspaceRootRole).toString(),
              QStringLiteral("/projects/alpha"));
    EXPECT_FALSE(running.data(mint::ui::TaskListModel::WorkspaceRemovableRole).toBool());
    EXPECT_FALSE(model.index(model.row_for_task_id("history"), 0)
                     .data(mint::ui::TaskListModel::LoadingRole)
                     .toBool());
    EXPECT_EQ(model.row_for_task_id("run-1"), -1);
}

TEST(DesktopSidebarInteractions, BackgroundApprovalHasAnAttentionStateAndStopsTheSpinner) {
    mint::ui::TaskSidebar sidebar;
    sidebar.resize(280, 360);
    sidebar.show();
    mint::desktop::TaskWorkspaceState state;
    state.root = "/projects/beta";
    state.workspace_roots = {"/projects/alpha", "/projects/beta"};
    state.running = true;
    state.running_root = "/projects/alpha";
    state.running_task_id = "run-1";
    state.running_title = "Inspect alpha";
    state.running_status = "waiting";
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();
    auto* list = sidebar.findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));
    auto* indicator = sidebar.findChild<fluent::status_info::ProgressRing*>(
        QStringLiteral("MintSidebarRunningIndicator"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(indicator, nullptr);
    auto* model = static_cast<mint::ui::TaskListModel*>(list->model());
    const auto running = model->index(model->running_row(), 0);
    EXPECT_TRUE(running.data(mint::ui::TaskListModel::AwaitingApprovalRole).toBool());
    EXPECT_TRUE(indicator->isHidden());
    EXPECT_FALSE(indicator->isActive());

    state.running_status = "running";
    sidebar.render(state, mint::desktop::WorkspaceChange::approval);
    EXPECT_FALSE(running.data(mint::ui::TaskListModel::AwaitingApprovalRole).toBool());
    EXPECT_TRUE(indicator->isVisible());
    EXPECT_TRUE(indicator->isActive());
}

TEST(DesktopSidebarInteractions, MotionPreferencesKeepFeedbackBoundedAndCanDisableIt) {
    auto& policy = fluent::MotionPolicy::instance();
    struct RestoreMotion {
        fluent::MotionPolicy::Mode mode;
        ~RestoreMotion() {
            fluent::MotionPolicy::instance().setMode(mode);
        }
    } restore{policy.mode()};
    mint::ui::TaskSidebar sidebar;
    sidebar.resize(280, 360);
    sidebar.show();
    mint::desktop::TaskWorkspaceState state;
    state.root = "/workspace";
    state.workspace_roots = {"/workspace"};
    state.tasks = {{.id = "task-1",
                    .question = "A long title that should not move on hover",
                    .status = "completed"}};
    state.task_id = "task-1";
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();
    auto* list = sidebar.findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));
    ASSERT_NE(list, nullptr);
    auto* delegate = dynamic_cast<mint::ui::TaskItemDelegate*>(list->itemDelegate());
    ASSERT_NE(delegate, nullptr);
    const auto task = list->model()->index(1, 0);
    const auto bounds = static_cast<QListView*>(list)->visualRect(task);

    policy.setMode(fluent::MotionPolicy::Mode::Reduced);
    QTest::mouseMove(list->viewport(), bounds.center());
    const auto animations = delegate->findChildren<QVariantAnimation*>();
    ASSERT_EQ(animations.size(), 2);
    for (const auto* animation : animations) {
        if (animation->state() == QAbstractAnimation::Running) {
            EXPECT_LE(animation->duration(), 50);
            EXPECT_EQ(animation->loopCount(), 1);
        }
    }
    policy.setMode(fluent::MotionPolicy::Mode::Disabled);
    for (const auto* animation : animations) {
        EXPECT_EQ(animation->state(), QAbstractAnimation::Stopped);
    }
    QTest::mouseMove(
        list->viewport(),
        static_cast<QListView*>(list)->visualRect(list->model()->index(0, 0)).center());
    EXPECT_EQ(static_cast<QListView*>(list)->visualRect(task), bounds);
    EXPECT_EQ(mint::ui::TaskItemDelegate::trailing_action_rect(bounds).width(), 28);
}

TEST(DesktopSidebarInteractions, ArrowKeysDiscloseWorkspacesWithoutResettingTheModel) {
    mint::ui::TaskSidebar sidebar;
    sidebar.resize(280, 360);
    sidebar.show();
    mint::desktop::TaskWorkspaceState state;
    state.root = "/workspace";
    state.workspace_roots = {"/workspace"};
    state.tasks = {{.id = "task-1", .question = "First", .status = "completed"}};
    state.task_id = "task-1";
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    auto* list = sidebar.findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));
    ASSERT_NE(list, nullptr);
    auto* model = static_cast<mint::ui::TaskListModel*>(list->model());
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    QObject::connect(&sidebar, &mint::ui::TaskSidebar::workspace_selected, &sidebar,
                     [&] { sidebar.render(state, mint::desktop::WorkspaceChange::tasks); });
    QApplication::processEvents();

    QTest::keyClick(list, Qt::Key_Left);
    EXPECT_EQ(model->row_kind(list->currentIndex().row()),
              mint::ui::TaskListModel::RowKind::workspace);
    QTest::keyClick(list, Qt::Key_Left);
    EXPECT_EQ(model->rowCount(), 1);
    QTest::keyClick(list, Qt::Key_Right);
    EXPECT_EQ(model->rowCount(), 2);
    EXPECT_EQ(model->row_kind(list->currentIndex().row()),
              mint::ui::TaskListModel::RowKind::workspace);
    QTest::keyClick(list, Qt::Key_Right);
    EXPECT_EQ(list->currentIndex().data(mint::ui::TaskListModel::TaskIdRole).toString(),
              QStringLiteral("task-1"));
    EXPECT_TRUE(resets.isEmpty());
}

TEST(DesktopSidebarInteractions, ContextMenusAreNonBlockingAndReleasedOnDismissal) {
    mint::ui::TaskSidebar sidebar;
    sidebar.resize(280, 360);
    sidebar.show();
    mint::desktop::TaskWorkspaceState state;
    state.root = "/workspace";
    state.workspace_roots = {"/workspace"};
    state.tasks = {{.id = "task-1", .question = "First", .status = "completed"}};
    state.task_id = "task-1";
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    auto* list = sidebar.findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));
    ASSERT_NE(list, nullptr);
    QApplication::processEvents();
    const auto baseline = sidebar.findChildren<QMenu*>().size();

    for (int repetition = 0; repetition < 3; ++repetition) {
        QTest::keyClick(list, Qt::Key_F10, Qt::ShiftModifier);
        QPointer<QMenu> popup(qobject_cast<QMenu*>(QApplication::activePopupWidget()));
        ASSERT_NE(popup, nullptr);
        EXPECT_EQ(sidebar.findChildren<QMenu*>().size(), baseline + 1);
        popup->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        EXPECT_EQ(popup, nullptr);
        EXPECT_EQ(sidebar.findChildren<QMenu*>().size(), baseline);
    }
}

TEST(DesktopSidebarInteractions, WorkspaceScrollRestoresAfterItsAsyncTaskListArrives) {
    mint::ui::TaskSidebar sidebar;
    sidebar.resize(280, 300);
    sidebar.show();
    mint::desktop::TaskWorkspaceState state;
    state.root = "/projects/alpha";
    state.workspace_roots = {"/projects/alpha", "/projects/beta"};
    std::vector<mint::desktop::TaskSummary> alpha_tasks;
    for (int row = 0; row < 80; ++row) {
        alpha_tasks.push_back({.id = "task-" + std::to_string(row),
                               .question = "Question " + std::to_string(row),
                               .status = "completed"});
    }
    state.tasks = alpha_tasks;
    state.task_id = "task-0";
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();
    auto* list = sidebar.findChild<fluent::collections::ListView*>(QStringLiteral("MintTaskList"));
    ASSERT_NE(list, nullptr);
    list->verticalScrollBar()->setValue(900);
    const auto reading_position = list->verticalScrollBar()->value();
    ASSERT_GT(reading_position, 0);

    state.root = "/projects/beta";
    state.tasks.clear();
    state.task_id.clear();
    state.loading_tasks = true;
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    state.loading_tasks = false;
    sidebar.render(state, mint::desktop::WorkspaceChange::tasks);
    state.root = "/projects/alpha";
    state.loading_tasks = true;
    sidebar.render(state, mint::desktop::WorkspaceChange::all);
    state.tasks = alpha_tasks;
    state.task_id = "task-0";
    state.loading_tasks = false;
    sidebar.render(state, mint::desktop::WorkspaceChange::tasks);
    QApplication::processEvents();
    EXPECT_EQ(list->verticalScrollBar()->value(), reading_position);
}

TEST(DesktopSidebarInteractions, ResizingCursorIsConfinedToTheSplitHandle) {
    mint::ui::WorkspaceSplitView split;
    split.resize(900, 400);
    auto* navigation = new QWidget;
    auto* content = new QWidget;
    auto* editor = new QLineEdit(content);
    editor->setGeometry(20, 20, 240, 32);
    fluent::collections::SplitViewPaneOptions navigation_options;
    navigation_options.minimumSize = 180;
    navigation_options.preferredSize = 240;
    navigation_options.maximumSize = 400;
    split.addPane(navigation, navigation_options);
    fluent::collections::SplitViewPaneOptions content_options;
    content_options.minimumSize = 300;
    content_options.fill = true;
    split.addPane(content, content_options);
    split.show();
    QApplication::processEvents();
    ASSERT_EQ(split.handleCount(), 1);

    for (int repetition = 0; repetition < 3; ++repetition) {
        const auto handle = split.handleGeometry(0);
        QTest::mouseMove(&split, handle.center());
        EXPECT_EQ(split.cursor().shape(), Qt::SplitHCursor);
        QTest::mouseMove(navigation, QPoint(navigation->width() - 1, 80));
        EXPECT_EQ(split.cursor().shape(), Qt::ArrowCursor);
        EXPECT_EQ(navigation->cursor().shape(), Qt::ArrowCursor);
        QTest::mouseMove(&split, split.handleGeometry(0).center());
        EXPECT_EQ(split.cursor().shape(), Qt::SplitHCursor);
        QTest::mouseMove(editor, editor->rect().center());
        EXPECT_EQ(split.cursor().shape(), Qt::ArrowCursor);
        EXPECT_EQ(editor->cursor().shape(), Qt::IBeamCursor);
    }

    const auto handle = split.handleGeometry(0);
    const auto original_width = navigation->width();
    QTest::mousePress(&split, Qt::LeftButton, Qt::NoModifier, handle.center());
    EXPECT_TRUE(split.resizing());
    QTest::mouseMove(&split, handle.center() + QPoint(30, 0));
    EXPECT_GT(navigation->width(), original_width);
    EXPECT_EQ(split.cursor().shape(), Qt::SplitHCursor);
    QTest::mouseRelease(&split, Qt::LeftButton, Qt::NoModifier, QPoint(split.width() - 10, 80));
    EXPECT_FALSE(split.resizing());
    EXPECT_EQ(split.cursor().shape(), Qt::ArrowCursor);
    QTest::mouseMove(&split, split.handleGeometry(0).center());
    split.setEnabled(false);
    EXPECT_EQ(split.cursor().shape(), Qt::ArrowCursor);
}

} // namespace
