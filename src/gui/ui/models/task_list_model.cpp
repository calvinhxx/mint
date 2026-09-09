#include "task_list_model.hpp"

#include "ui/support/status_text.hpp"

#include <algorithm>
#include <iterator>

#include <QDir>
#include <QFileInfo>

namespace mint::ui {
namespace {

std::filesystem::path normalized_root(const std::filesystem::path& root) {
    auto normalized = root.lexically_normal();
    if (normalized != normalized.root_path() && normalized.filename().empty()) {
        normalized = normalized.parent_path();
    }
    return normalized;
}

} // namespace

TaskListModel::TaskListModel(QObject* parent) : QAbstractListModel(parent) {}

int TaskListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant TaskListModel::data(const QModelIndex& index, int role) const {
    if (index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    const auto& task = row.task;
    switch (role) {
    case Qt::DisplayRole:
        return row.kind == RowKind::workspace ? data(index, WorkspaceNameRole)
                                              : QString::fromStdString(task.question);
    case Qt::ToolTipRole:
        return row.kind == RowKind::workspace ? data(index, WorkspaceRootRole)
                                              : QString::fromStdString(task.question);
    case Qt::AccessibleTextRole:
        return data(index, Qt::DisplayRole);
    case Qt::AccessibleDescriptionRole:
        return row.kind == RowKind::workspace
                   ? (row.loading
                          ? data(index, WorkspaceRootRole).toString() + QStringLiteral(" · ") +
                                status_text(row.awaiting_approval ? QStringLiteral("waiting")
                                                                  : QStringLiteral("running"))
                          : data(index, WorkspaceRootRole))
                   : status_text(persisted_status_code(QString::fromStdString(task.status),
                                                       task.resumable));
    case QuestionRole:
        return row.kind == RowKind::task ? QString::fromStdString(task.question) : QString{};
    case TaskIdRole:
        return row.kind == RowKind::task ? QString::fromStdString(task.id) : QString{};
    case StatusRole:
        return row.kind == RowKind::task
                   ? persisted_status_code(QString::fromStdString(task.status), task.resumable)
                   : QString{};
    case StatusTextRole:
        return row.kind == RowKind::task ? status_text(persisted_status_code(
                                               QString::fromStdString(task.status), task.resumable))
                                         : QString{};
    case CreatedAtRole:
        return QString::fromStdString(task.created_at);
    case ResumableRole:
        return task.resumable;
    case PinnedRole:
        return row.pinned;
    case ArchivedRole:
        return row.archived;
    case RowKindRole:
        return static_cast<int>(row.kind);
    case WorkspaceRootRole: {
#if defined(_WIN32)
        return QString::fromStdWString(row.workspace_root.wstring());
#else
        const auto value = row.workspace_root.native();
        return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
#endif
    }
    case WorkspaceNameRole: {
        const auto path = QDir::cleanPath(data(index, WorkspaceRootRole).toString());
        const auto name = QFileInfo(path).fileName();
        return name.isEmpty() ? path : name;
    }
    case ActiveWorkspaceRole:
        return row.active_workspace;
    case ExpandedWorkspaceRole:
        return row.expanded_workspace;
    case WorkspaceExpandableRole:
        return row.kind == RowKind::workspace && row.workspace_expandable;
    case WorkspaceRemovableRole:
        return row.kind == RowKind::workspace && row.workspace_removable;
    case LoadingRole:
        return row.loading;
    case AwaitingApprovalRole:
        return row.awaiting_approval;
    default:
        return {};
    }
}

QHash<int, QByteArray> TaskListModel::roleNames() const {
    return {{TaskIdRole, "taskId"},
            {QuestionRole, "question"},
            {StatusRole, "status"},
            {StatusTextRole, "statusText"},
            {CreatedAtRole, "createdAt"},
            {ResumableRole, "resumable"},
            {PinnedRole, "pinned"},
            {ArchivedRole, "archived"},
            {RowKindRole, "rowKind"},
            {WorkspaceRootRole, "workspaceRoot"},
            {WorkspaceNameRole, "workspaceName"},
            {ActiveWorkspaceRole, "activeWorkspace"},
            {ExpandedWorkspaceRole, "expandedWorkspace"},
            {WorkspaceExpandableRole, "workspaceExpandable"},
            {WorkspaceRemovableRole, "workspaceRemovable"},
            {LoadingRole, "loading"},
            {AwaitingApprovalRole, "awaitingApproval"}};
}

void TaskListModel::set_tasks(const std::vector<desktop::TaskSummary>& tasks) {
    set_tasks(tasks, {}, false);
}

void TaskListModel::set_tasks(const std::vector<desktop::TaskSummary>& tasks,
                              const desktop::TaskOrganization& organization, bool show_archived) {
    std::vector<Row> rows;
    rows.reserve(tasks.size());
    for (const auto& task : tasks) {
        const auto archived = organization.archived_task_ids.contains(task.id);
        if (archived != show_archived) {
            continue;
        }
        rows.push_back({.kind = RowKind::task,
                        .task = task,
                        .pinned = !archived && organization.pinned_task_ids.contains(task.id),
                        .archived = archived});
    }
    if (!show_archived) {
        std::stable_partition(rows.begin(), rows.end(), [](const auto& row) { return row.pinned; });
    }

    replace_rows(std::move(rows));
}

void TaskListModel::set_navigation(
    const std::vector<std::filesystem::path>& workspace_roots,
    const std::filesystem::path& active_root, const std::vector<desktop::TaskSummary>& tasks,
    const desktop::TaskOrganization& organization, bool show_archived, bool running,
    std::string_view active_task_id, std::string_view active_task_title,
    bool active_workspace_expanded,
    const std::vector<desktop::WorkspaceTaskCounts>& workspace_task_counts,
    const std::filesystem::path& running_root, std::string_view running_status) {
    std::vector<Row> rows;
    rows.reserve(workspace_roots.size() + tasks.size() + 1);
    const auto normalized_active = normalized_root(active_root);
    const auto normalized_running =
        running_root.empty() ? normalized_active : normalized_root(running_root);
    std::vector<std::filesystem::path> roots;
    roots.reserve(workspace_roots.size() + 1);
    for (const auto& root : workspace_roots) {
        const auto normalized = normalized_root(root);
        if (!normalized.empty() &&
            std::find(roots.begin(), roots.end(), normalized) == roots.end()) {
            roots.push_back(normalized);
        }
    }
    if (!normalized_active.empty() &&
        std::find(roots.begin(), roots.end(), normalized_active) == roots.end()) {
        roots.push_back(normalized_active);
    }

    for (const auto& root : roots) {
        const auto normalized_workspace = normalized_root(root);
        const auto active = normalized_workspace == normalized_active;
        const auto workspace_running = running && normalized_workspace == normalized_running;
        const auto awaiting_approval = workspace_running && running_status == "waiting";
        const auto cached_counts = std::find_if(
            workspace_task_counts.begin(), workspace_task_counts.end(), [&](const auto& counts) {
                return normalized_root(counts.root) == normalized_workspace;
            });
        const auto cached_visible_tasks =
            cached_counts == workspace_task_counts.end() ||
            (show_archived ? cached_counts->archived : cached_counts->unarchived) > 0;
        const auto has_visible_tasks =
            active ? std::any_of(tasks.begin(), tasks.end(),
                                 [&](const auto& task) {
                                     return organization.archived_task_ids.contains(task.id) ==
                                            show_archived;
                                 }) ||
                         (workspace_running && !show_archived && !active_task_title.empty())
                   : cached_visible_tasks;
        rows.push_back(
            {.kind = RowKind::workspace,
             .workspace_root = normalized_workspace,
             .active_workspace = active,
             .expanded_workspace = active && active_workspace_expanded && has_visible_tasks,
             .workspace_expandable = has_visible_tasks,
             .workspace_removable = roots.size() > 1 && !workspace_running,
             .loading =
                 workspace_running && (!active || !active_workspace_expanded || show_archived),
             .awaiting_approval =
                 awaiting_approval && (!active || !active_workspace_expanded || show_archived)});
        if (!active || !active_workspace_expanded) {
            continue;
        }

        std::vector<Row> task_rows;
        task_rows.reserve(tasks.size() + 1);
        auto running_task_found = false;
        for (const auto& task : tasks) {
            const auto archived = organization.archived_task_ids.contains(task.id);
            if (archived != show_archived) {
                continue;
            }
            auto visible_task = task;
            const auto live_task = workspace_running && !active_task_id.empty() &&
                                   std::string_view(task.id) == active_task_id;
            if (live_task) {
                visible_task.status = awaiting_approval ? "waiting" : "running";
                visible_task.resumable = false;
                running_task_found = true;
            }
            task_rows.push_back(
                {.kind = RowKind::task,
                 .workspace_root = normalized_workspace,
                 .task = std::move(visible_task),
                 .pinned = !archived && organization.pinned_task_ids.contains(task.id),
                 .archived = archived,
                 .loading = live_task,
                 .awaiting_approval = live_task && awaiting_approval});
        }
        if (!show_archived) {
            std::stable_partition(task_rows.begin(), task_rows.end(),
                                  [](const auto& row) { return row.pinned; });
        }
        if (workspace_running && !running_task_found && !active_task_title.empty() &&
            !show_archived) {
            desktop::TaskSummary task{.id = std::string(active_task_id),
                                      .question = std::string(active_task_title),
                                      .status = awaiting_approval ? "waiting" : "running"};
            task_rows.insert(task_rows.begin(), {.kind = RowKind::task,
                                                 .workspace_root = normalized_workspace,
                                                 .task = std::move(task),
                                                 .loading = true,
                                                 .awaiting_approval = awaiting_approval});
        }
        rows.insert(rows.end(), std::make_move_iterator(task_rows.begin()),
                    std::make_move_iterator(task_rows.end()));
    }
    replace_rows(std::move(rows));
}

void TaskListModel::replace_rows(std::vector<Row> rows) {
    const auto same_identity = [](const auto& current, const auto& next) {
        return current.kind == next.kind && current.workspace_root == next.workspace_root &&
               current.task.id == next.task.id;
    };
    const auto same_order = rows_.size() == rows.size() &&
                            std::equal(rows_.begin(), rows_.end(), rows.begin(), same_identity);
    if (!same_order) {
        // EN: Preserve unaffected rows and Qt's persistent selection/scroll anchors. A task
        // append or workspace disclosure is an insertion/removal, never a model reset.
        // ZH-CN: 保留未变行与 Qt 的持久选择、滚动锚点；任务追加及工作区展开收起只增删对应行。
        auto first = std::size_t{0};
        while (first < rows_.size() && first < rows.size() &&
               same_identity(rows_[first], rows[first])) {
            ++first;
        }
        auto suffix = std::size_t{0};
        while (suffix < rows_.size() - first && suffix < rows.size() - first &&
               same_identity(rows_[rows_.size() - suffix - 1], rows[rows.size() - suffix - 1])) {
            ++suffix;
        }
        const auto removed_end = rows_.size() - suffix;
        if (removed_end > first) {
            beginRemoveRows({}, static_cast<int>(first), static_cast<int>(removed_end - 1));
            rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(first),
                        rows_.begin() + static_cast<std::ptrdiff_t>(removed_end));
            rebuild_row_indexes();
            endRemoveRows();
        }
        const auto inserted_end = rows.size() - suffix;
        if (inserted_end > first) {
            beginInsertRows({}, static_cast<int>(first), static_cast<int>(inserted_end - 1));
            rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(first),
                         rows.begin() + static_cast<std::ptrdiff_t>(first),
                         rows.begin() + static_cast<std::ptrdiff_t>(inserted_end));
            rebuild_row_indexes();
            endInsertRows();
        }
    }

    int changed_first = -1;
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const auto position = static_cast<std::size_t>(row);
        const auto& current = rows_[position];
        const auto& next = rows[position];
        const auto changed =
            current.kind != next.kind || current.workspace_root != next.workspace_root ||
            current.active_workspace != next.active_workspace ||
            current.expanded_workspace != next.expanded_workspace ||
            current.workspace_expandable != next.workspace_expandable ||
            current.loading != next.loading ||
            current.awaiting_approval != next.awaiting_approval ||
            current.workspace_removable != next.workspace_removable ||
            current.task.id != next.task.id || current.task.question != next.task.question ||
            current.task.status != next.task.status ||
            current.task.created_at != next.task.created_at ||
            current.task.verification_status != next.task.verification_status ||
            current.task.completed != next.task.completed ||
            current.task.resumable != next.task.resumable || current.pinned != next.pinned ||
            current.archived != next.archived;
        if (changed) {
            rows_[position] = std::move(rows[position]);
            if (changed_first < 0) {
                changed_first = row;
            }
        } else if (changed_first >= 0) {
            emit dataChanged(index(changed_first, 0), index(row - 1, 0));
            changed_first = -1;
        }
    }
    if (changed_first >= 0) {
        emit dataChanged(index(changed_first, 0), index(static_cast<int>(rows.size()) - 1, 0));
    }
    rebuild_row_indexes();
}

void TaskListModel::rebuild_row_indexes() {
    task_rows_by_id_.clear();
    workspace_rows_by_root_.clear();
    running_row_ = -1;
    task_rows_by_id_.reserve(rows_.size());
    workspace_rows_by_root_.reserve(rows_.size());
    for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
        const auto& item = rows_[static_cast<std::size_t>(row)];
        if (item.kind == RowKind::workspace) {
            workspace_rows_by_root_.insert_or_assign(item.workspace_root.generic_string(), row);
            if (item.loading) {
                running_row_ = row;
            }
            continue;
        }
        task_rows_by_id_.insert_or_assign(item.task.id, row);
        if (item.loading) {
            running_row_ = row;
        }
    }
}

void TaskListModel::retranslate() {
    if (rows_.empty()) {
        return;
    }
    emit dataChanged(index(0, 0), index(static_cast<int>(rows_.size()) - 1, 0),
                     {StatusTextRole, Qt::AccessibleDescriptionRole});
}

TaskListModel::RowKind TaskListModel::row_kind(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return RowKind::task;
    }
    return rows_[static_cast<std::size_t>(row)].kind;
}

const desktop::TaskSummary* TaskListModel::task_at(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return nullptr;
    }
    const auto& item = rows_[static_cast<std::size_t>(row)];
    return item.kind == RowKind::task ? &item.task : nullptr;
}

QString TaskListModel::workspace_root_at(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return {};
    }
    return data(index(row, 0), WorkspaceRootRole).toString();
}

QString TaskListModel::task_id_at(int row) const {
    const auto* task = task_at(row);
    return task == nullptr ? QString{} : QString::fromStdString(task->id);
}

int TaskListModel::row_for_workspace(const std::filesystem::path& root) const {
    const auto found = workspace_rows_by_root_.find(normalized_root(root).generic_string());
    return found == workspace_rows_by_root_.end() ? -1 : found->second;
}

int TaskListModel::running_row() const {
    return running_row_;
}

int TaskListModel::row_for_task_id(std::string_view task_id) const {
    if (task_id.empty()) {
        return -1;
    }
    const auto found = task_rows_by_id_.find(std::string(task_id));
    return found == task_rows_by_id_.end() ? -1 : found->second;
}

} // namespace mint::ui
