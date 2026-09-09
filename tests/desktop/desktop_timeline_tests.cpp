#include "ui/models/run_timeline_model.hpp"

#include <QAbstractItemModelTester>
#include <QSignalSpy>

#include <gtest/gtest.h>

namespace {

using mint::desktop::RunEventKind;
using mint::desktop::TaskWorkspaceState;
using mint::ui::RunTimelineModel;

void append_events(TaskWorkspaceState& state, std::size_t first, std::size_t last) {
    for (auto sequence = first; sequence <= last; ++sequence) {
        state.events.push_back({.sequence = sequence,
                                .kind = RunEventKind::tool_completed,
                                .raw_type = "tool_completed",
                                .name = "read_file",
                                .target = "file-" + std::to_string(sequence)});
    }
}

TEST(DesktopTimeline, EventBatchesInsertRangesWithoutResettingTheTranscript) {
    RunTimelineModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
    TaskWorkspaceState state;
    append_events(state, 1, 100);
    ASSERT_TRUE(model.append_latest_event(state));
    EXPECT_EQ(model.rowCount(), 101);
    EXPECT_EQ(resets.count(), 0);
    ASSERT_EQ(inserts.count(), 1);
    EXPECT_EQ(inserts.front().at(1).toInt(), 1);
    EXPECT_EQ(inserts.front().at(2).toInt(), 99);

    append_events(state, 101, 200);
    ASSERT_TRUE(model.append_latest_event(state));
    EXPECT_EQ(model.rowCount(), 201);
    EXPECT_EQ(resets.count(), 0);
    ASSERT_EQ(inserts.count(), 2);
    EXPECT_EQ(inserts.back().at(1).toInt(), 100);
    EXPECT_EQ(inserts.back().at(2).toInt(), 199);
}

TEST(DesktopTimeline, RetainedEventWindowRemovesAndInsertsWholeBatches) {
    RunTimelineModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    TaskWorkspaceState state;
    append_events(state, 1, mint::desktop::maximum_retained_run_events);
    model.set_state(state);
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy removals(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
    state.events.erase(state.events.begin(), state.events.begin() + 100);
    append_events(state, 5'001, 5'100);

    ASSERT_TRUE(model.append_latest_event(state));
    EXPECT_EQ(model.rowCount(), 5'001);
    EXPECT_EQ(resets.count(), 0);
    ASSERT_EQ(removals.count(), 1);
    EXPECT_EQ(removals.front().at(1).toInt(), 0);
    EXPECT_EQ(removals.front().at(2).toInt(), 99);
    ASSERT_EQ(inserts.count(), 1);
    EXPECT_EQ(inserts.front().at(1).toInt(), 4'900);
    EXPECT_EQ(inserts.front().at(2).toInt(), 4'999);
}

} // namespace
