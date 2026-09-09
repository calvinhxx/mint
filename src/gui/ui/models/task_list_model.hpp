#pragma once

#include "application/desktop_types.hpp"
#include "application/task_organization.hpp"

#include <QAbstractListModel>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mint::ui {

class TaskListModel final : public QAbstractListModel {
  public:
    enum class RowKind { workspace, task };

    enum Role {
        TaskIdRole = Qt::UserRole + 1,
        QuestionRole,
        StatusRole,
        StatusTextRole,
        CreatedAtRole,
        ResumableRole,
        PinnedRole,
        ArchivedRole,
        RowKindRole,
        WorkspaceRootRole,
        WorkspaceNameRole,
        ActiveWorkspaceRole,
        ExpandedWorkspaceRole,
        WorkspaceExpandableRole,
        WorkspaceRemovableRole,
        LoadingRole,
        AwaitingApprovalRole
    };

    explicit TaskListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_tasks(const std::vector<desktop::TaskSummary>& tasks);
    void set_tasks(const std::vector<desktop::TaskSummary>& tasks,
                   const desktop::TaskOrganization& organization, bool show_archived);
    void set_navigation(const std::vector<std::filesystem::path>& workspace_roots,
                        const std::filesystem::path& active_root,
                        const std::vector<desktop::TaskSummary>& tasks,
                        const desktop::TaskOrganization& organization, bool show_archived,
                        bool running, std::string_view active_task_id,
                        std::string_view active_task_title, bool active_workspace_expanded = true,
                        const std::vector<desktop::WorkspaceTaskCounts>& workspace_task_counts = {},
                        const std::filesystem::path& running_root = {},
                        std::string_view running_status = {});
    void retranslate();
    [[nodiscard]] RowKind row_kind(int row) const;
    [[nodiscard]] const desktop::TaskSummary* task_at(int row) const;
    [[nodiscard]] QString task_id_at(int row) const;
    [[nodiscard]] QString workspace_root_at(int row) const;
    [[nodiscard]] int row_for_task_id(std::string_view task_id) const;
    [[nodiscard]] int row_for_workspace(const std::filesystem::path& root) const;
    [[nodiscard]] int running_row() const;

  private:
    struct Row {
        RowKind kind = RowKind::task;
        std::filesystem::path workspace_root;
        desktop::TaskSummary task;
        bool active_workspace = false;
        bool expanded_workspace = false;
        bool workspace_expandable = false;
        bool workspace_removable = false;
        bool pinned = false;
        bool archived = false;
        bool loading = false;
        bool awaiting_approval = false;
    };

    void replace_rows(std::vector<Row> rows);
    void rebuild_row_indexes();

    std::vector<Row> rows_;
    std::unordered_map<std::string, int> task_rows_by_id_;
    std::unordered_map<std::string, int> workspace_rows_by_root_;
    int running_row_ = -1;
};

} // namespace mint::ui
