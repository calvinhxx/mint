#include "application/task_organization.hpp"
#include "infrastructure/settings/qsettings_task_organization_repository.hpp"
#include "ui/models/task_list_model.hpp"

#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <unordered_map>

namespace {

class MemoryTaskOrganizationRepository final : public mint::desktop::TaskOrganizationRepository {
  public:
    [[nodiscard]] std::optional<mint::desktop::TaskOrganization>
    load(std::string_view project_scope) const override {
        if (!load_succeeds) {
            return std::nullopt;
        }
        const auto found = values.find(std::string(project_scope));
        if (found == values.end()) {
            return mint::desktop::TaskOrganization{};
        }
        return found->second;
    }

    bool save(std::string_view project_scope,
              const mint::desktop::TaskOrganization& organization) override {
        if (!save_succeeds) {
            return false;
        }
        values[std::string(project_scope)] = organization;
        return true;
    }

    std::unordered_map<std::string, mint::desktop::TaskOrganization> values;
    bool load_succeeds = true;
    bool save_succeeds = true;
};

TEST(DesktopTaskOrganization, PinArchiveAndUnarchiveArePersistentAndReversible) {
    MemoryTaskOrganizationRepository repository;
    mint::desktop::TaskOrganizer organizer(&repository);
    ASSERT_TRUE(organizer.open_project("/workspace"));

    ASSERT_TRUE(organizer.toggle_pin("task-1"));
    EXPECT_TRUE(organizer.is_pinned("task-1"));

    ASSERT_TRUE(organizer.archive("task-1"));
    EXPECT_FALSE(organizer.is_pinned("task-1"));
    EXPECT_TRUE(organizer.is_archived("task-1"));

    ASSERT_TRUE(organizer.unarchive("task-1"));
    EXPECT_FALSE(organizer.is_archived("task-1"));

    mint::desktop::TaskOrganizer restored(&repository);
    ASSERT_TRUE(restored.open_project("/workspace"));
    EXPECT_FALSE(restored.is_pinned("task-1"));
    EXPECT_FALSE(restored.is_archived("task-1"));
}

TEST(DesktopTaskOrganization, FailedPersistenceLeavesTheVisibleStateUnchanged) {
    MemoryTaskOrganizationRepository repository;
    mint::desktop::TaskOrganizer organizer(&repository);
    ASSERT_TRUE(organizer.open_project("/workspace"));
    repository.save_succeeds = false;

    EXPECT_FALSE(organizer.toggle_pin("task-1"));
    EXPECT_FALSE(organizer.is_pinned("task-1"));
}

TEST(DesktopTaskOrganization, FailedProjectLoadCanBeRetriedWithoutLosingCurrentState) {
    MemoryTaskOrganizationRepository repository;
    repository.values["/workspace/a"].pinned_task_ids.insert("task-1");
    repository.values["/workspace/b"].pinned_task_ids.insert("task-2");
    mint::desktop::TaskOrganizer organizer(&repository);
    ASSERT_TRUE(organizer.open_project("/workspace/a"));
    ASSERT_TRUE(organizer.is_pinned("task-1"));

    repository.load_succeeds = false;
    EXPECT_FALSE(organizer.open_project("/workspace/b"));
    EXPECT_TRUE(organizer.is_pinned("task-1"));

    repository.load_succeeds = true;
    ASSERT_TRUE(organizer.open_project("/workspace/b"));
    EXPECT_FALSE(organizer.is_pinned("task-1"));
    EXPECT_TRUE(organizer.is_pinned("task-2"));
}

TEST(DesktopTaskOrganization, QSettingsKeepsEachProjectScopeSeparate) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto settings_path = directory.filePath(QStringLiteral("organization.ini"));

    {
        mint::infrastructure::desktop::QSettingsTaskOrganizationRepository repository(
            settings_path);
        ASSERT_TRUE(repository.save(
            "/workspace/a", {.pinned_task_ids = {"task-a"}, .archived_task_ids = {"task-old"}}));
        ASSERT_TRUE(repository.save("/workspace/b", {.pinned_task_ids = {"task-b"}}));
    }

    mint::infrastructure::desktop::QSettingsTaskOrganizationRepository repository(settings_path);
    const auto first = repository.load("/workspace/a");
    const auto second = repository.load("/workspace/b");
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(first->pinned_task_ids.contains("task-a"));
    EXPECT_TRUE(first->archived_task_ids.contains("task-old"));
    EXPECT_FALSE(first->pinned_task_ids.contains("task-b"));
    EXPECT_TRUE(second->pinned_task_ids.contains("task-b"));
    EXPECT_TRUE(second->archived_task_ids.empty());
}

TEST(DesktopTaskOrganization, TaskModelPinsFirstAndFiltersArchivedRows) {
    const std::vector<mint::desktop::TaskSummary> tasks = {
        {.id = "task-1", .question = "First", .status = "completed"},
        {.id = "task-2", .question = "Second", .status = "completed"},
        {.id = "task-3", .question = "Third", .status = "completed"},
    };
    const mint::desktop::TaskOrganization organization{.pinned_task_ids = {"task-3"},
                                                       .archived_task_ids = {"task-2"}};
    mint::ui::TaskListModel model;

    model.set_tasks(tasks, organization, false);
    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.task_id_at(0), QStringLiteral("task-3"));
    EXPECT_TRUE(model.data(model.index(0, 0), mint::ui::TaskListModel::PinnedRole).toBool());
    EXPECT_EQ(model.task_id_at(1), QStringLiteral("task-1"));
    EXPECT_EQ(model.row_for_task_id("task-2"), -1);

    model.set_tasks(tasks, organization, true);
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.task_id_at(0), QStringLiteral("task-2"));
    EXPECT_TRUE(model.data(model.index(0, 0), mint::ui::TaskListModel::ArchivedRole).toBool());
}

} // namespace
