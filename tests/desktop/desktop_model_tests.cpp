#include "mint/localization/localization.hpp"

#include "ui/models/run_timeline_model.hpp"
#include "ui/models/task_list_model.hpp"
#include "ui/presentation/run_event_presentation.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

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

} // namespace
