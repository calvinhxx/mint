#include "task_sidebar.hpp"

#include "support/qt_path.hpp"
#include "ui/delegates/task_item_delegate.hpp"
#include "ui/models/task_list_model.hpp"
#include "ui/support/localized_text.hpp"
#include "ui/theme/layout_metrics.hpp"

#include <FluentQt/BasicInput.h>
#include <FluentQt/Collections.h>
#include <FluentQt/Design.h>
#include <FluentQt/MenusToolbars.h>
#include <FluentQt/StatusInfo.h>

#include <QAbstractItemView>
#include <QAction>
#include <QCursor>
#include <QEvent>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPersistentModelIndex>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

namespace mint::ui {
namespace {

bool changes_current_item(int key) {
    switch (key) {
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        return true;
    default:
        return false;
    }
}

} // namespace

TaskSidebar::TaskSidebar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MintTaskSidebar"));
    setAutoFillBackground(false);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(metrics::space_sm, metrics::space_sm, metrics::space_sm,
                               metrics::space_md);
    layout->setSpacing(metrics::space_xs);

    auto* task_header = new QHBoxLayout;
    task_header->setContentsMargins(metrics::space_xs, 0, 0, 0);
    task_header->setSpacing(0);
    collection_button_ = new fluent::basicinput::DropDownButton(this);
    collection_button_->setObjectName(QStringLiteral("MintTaskCollectionButton"));
    collection_button_->setFluentLayout(fluent::basicinput::Button::TextOnly);
    collection_button_->setFluentStyle(fluent::basicinput::Button::Subtle);
    collection_button_->setFluentSize(fluent::basicinput::Button::Small);
    collection_button_->setChevronGlyph(Typography::Icons::More);
    collection_button_->setChevronSize(12);
    auto* collection_menu = new fluent::menus_toolbars::FluentMenu(QString{}, collection_button_);
    collection_view_action_ =
        new fluent::menus_toolbars::FluentMenuItem(QString{}, collection_menu);
    collection_menu->addAction(collection_view_action_);
    collection_button_->setMenu(collection_menu);
    task_header->addWidget(collection_button_);
    task_header->addStretch(1);
    add_workspace_button_ = new fluent::basicinput::Button(this);
    add_workspace_button_->setObjectName(QStringLiteral("MintProjectButton"));
    add_workspace_button_->setIconGlyph(Typography::Icons::Folder);
    add_workspace_button_->setFluentLayout(fluent::basicinput::Button::IconOnly);
    add_workspace_button_->setFluentStyle(fluent::basicinput::Button::Subtle);
    add_workspace_button_->setFluentSize(fluent::basicinput::Button::Small);
    task_header->addWidget(add_workspace_button_);
    layout->addLayout(task_header);

    task_model_ = new TaskListModel(this);
    task_view_ = new fluent::collections::ListView(this);
    task_view_->setObjectName(QStringLiteral("MintTaskList"));
    task_view_->setModel(task_model_);
    task_delegate_ = new TaskItemDelegate(task_view_);
    task_view_->setItemDelegate(task_delegate_);
    task_view_->setSelectionMode(fluent::collections::SelectionMode::Single);
    task_view_->setSelectionIndicatorVisible(false);
    task_view_->setUniformItemSizes(true);
    task_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    task_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    task_view_->setBorderVisible(false);
    task_view_->setBackgroundVisible(false);
    task_view_->viewport()->setAutoFillBackground(false);
    task_view_->setSpacing(0);
    task_view_->setPlaceholderText(text(localization::Message::gui_task_empty));
    task_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    task_view_->setMouseTracking(true);
    task_view_->installEventFilter(this);
    task_view_->viewport()->installEventFilter(this);
    layout->addWidget(task_view_, 1);

    workspace_action_button_ = new fluent::basicinput::Button(this);
    workspace_action_button_->setObjectName(QStringLiteral("MintWorkspaceRowAddButton"));
    workspace_action_button_->setIconGlyph(Typography::Icons::Add);
    workspace_action_button_->setFluentLayout(fluent::basicinput::Button::IconOnly);
    workspace_action_button_->setFluentStyle(fluent::basicinput::Button::Subtle);
    workspace_action_button_->setFluentSize(fluent::basicinput::Button::Small);
    workspace_action_button_->setFixedSize(28, 28);
    workspace_action_button_->installEventFilter(this);
    workspace_action_button_->hide();

    task_action_button_ = new fluent::basicinput::Button(this);
    task_action_button_->setObjectName(QStringLiteral("MintTaskRowMenuButton"));
    task_action_button_->setIconGlyph(Typography::Icons::More, Typography::IconSize::Compact);
    task_action_button_->setFluentLayout(fluent::basicinput::Button::IconOnly);
    task_action_button_->setFluentStyle(fluent::basicinput::Button::Subtle);
    task_action_button_->setFluentSize(fluent::basicinput::Button::Small);
    task_action_button_->setFixedSize(28, 28);
    task_action_button_->installEventFilter(this);
    task_action_button_->hide();

    running_indicator_ = new fluent::status_info::ProgressRing(this);
    running_indicator_->setObjectName(QStringLiteral("MintSidebarRunningIndicator"));
    running_indicator_->setRingSize(fluent::status_info::ProgressRing::ProgressRingSize::Small);
    running_indicator_->setStrokeWidth(2.0);
    running_indicator_->setBackgroundVisible(false);
    running_indicator_->setFixedSize(metrics::space_lg, metrics::space_lg);
    running_indicator_->setAttribute(Qt::WA_TransparentForMouseEvents);
    running_indicator_->setAccessibleName(text(localization::Message::gui_status_running));
    running_indicator_->hide();

    connect(add_workspace_button_, &QPushButton::clicked, this,
            &TaskSidebar::add_workspace_requested);
    connect(collection_view_action_, &QAction::triggered, this,
            [this] { emit archived_view_requested(!showing_archived_tasks_); });
    connect(workspace_action_button_, &QPushButton::clicked, this, [this] {
        if (!mutations_enabled_ || action_workspace_root_.isEmpty()) {
            return;
        }
        active_workspace_expanded_ = true;
        collapsed_workspaces_.remove(action_workspace_root_);
        emit new_task_requested(action_workspace_root_);
    });
    connect(task_action_button_, &QPushButton::clicked, this, [this] {
        const auto index = task_model_->index(action_row_, 0);
        if (!can_show_item_menu(index)) {
            return;
        }
        show_item_menu(index, task_action_button_->mapToGlobal(QPoint(
                                  task_action_button_->width(), task_action_button_->height())));
    });
    connect(task_view_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& position) { show_item_menu(position); });
    // EN: Selection changes keep pointer and keyboard navigation on one path. FluentQt 1.8
    // restores clicked(), but click-only wiring would drop keyboard navigation.
    // ZH-CN: 选择变化让鼠标与键盘共用同一路径。FluentQt 1.8 虽恢复 clicked()，
    // 但只监听点击会丢失键盘导航。
    connect(task_view_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current) {
                if (rendering_ || !mutations_enabled_ || !current.isValid()) {
                    return;
                }
                if (task_model_->row_kind(current.row()) == TaskListModel::RowKind::task) {
                    activate_row(current.row());
                }
            });
    // EN: Accessibility can select a row without moving Qt's current index. Reconcile after
    // the input event so normal pointer/keyboard navigation still activates exactly once.
    // ZH-CN: 无障碍动作可以只选择行而不移动 Qt 当前项；输入事件后再同步，避免鼠标和键盘重复导航。
    connect(task_view_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection& selected) {
                if (rendering_ || !mutations_enabled_ || selected.isEmpty()) {
                    return;
                }
                const QPersistentModelIndex index(selected.indexes().front());
                QTimer::singleShot(0, this, [this, index] {
                    if (rendering_ || !mutations_enabled_ || !index.isValid() ||
                        !task_view_->selectionModel()->isSelected(index) ||
                        task_view_->currentIndex() == index) {
                        return;
                    }
                    task_view_->setCurrentIndex(index);
                    if (task_model_->row_kind(index.row()) == TaskListModel::RowKind::workspace) {
                        activate_row(index.row());
                    }
                });
            });
    connect(task_view_, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) {
        if (index.isValid() &&
            task_model_->row_kind(index.row()) == TaskListModel::RowKind::workspace) {
            activate_row(index.row());
        }
    });
    connect(task_view_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        position_running_indicator();
        refresh_row_action_at_pointer();
    });
    retranslate();
}

void TaskSidebar::activate_row(int row) {
    const auto current = task_model_->index(row, 0);
    if (!mutations_enabled_ || !current.isValid()) {
        return;
    }
    if (task_model_->row_kind(row) == TaskListModel::RowKind::workspace) {
        const auto root = task_model_->workspace_root_at(row);
        if (current.data(TaskListModel::ActiveWorkspaceRole).toBool() &&
            current.data(TaskListModel::WorkspaceExpandableRole).toBool()) {
            active_workspace_expanded_ = !active_workspace_expanded_;
            if (active_workspace_expanded_) {
                collapsed_workspaces_.remove(root);
            } else {
                collapsed_workspaces_.insert(root);
            }
        } else if (!current.data(TaskListModel::ActiveWorkspaceRole).toBool()) {
            // EN: Selecting a closed workspace is an explicit request to reveal its tasks.
            // ZH-CN: 选择收起的工作区即表示需要展开查看其中的任务。
            collapsed_workspaces_.remove(root);
        }
        const auto was_expanded = current.data(TaskListModel::ExpandedWorkspaceRole).toBool();
        emit workspace_selected(root);
        task_delegate_->animate_disclosure(
            task_model_->index(task_model_->row_for_workspace(mint::gui::path_from_qstring(root)),
                               0),
            was_expanded);
        return;
    }
    const auto task_id = task_model_->task_id_at(row);
    if (!task_id.isEmpty()) {
        emit task_selected(task_id);
    }
}

bool TaskSidebar::eventFilter(QObject* watched, QEvent* event) {
    const auto on_row_action =
        watched == workspace_action_button_ || watched == task_action_button_;
    if (on_row_action) {
        if (event->type() == QEvent::Leave) {
            QTimer::singleShot(0, this, [this] { refresh_row_action_at_pointer(); });
        }
        return QWidget::eventFilter(watched, event);
    }

    const auto on_view = watched == task_view_ || watched == task_view_->viewport();
    if (!on_view) {
        return QWidget::eventFilter(watched, event);
    }

    if (watched == task_view_->viewport() && event->type() == QEvent::Resize) {
        QTimer::singleShot(0, this, [this] {
            position_running_indicator();
            position_row_action();
        });
    }
    if (watched == task_view_->viewport() && event->type() == QEvent::Leave) {
        QTimer::singleShot(0, this, [this] { refresh_row_action_at_pointer(); });
    }

    if (!mutations_enabled_) {
        if (watched == task_view_->viewport() && (event->type() == QEvent::MouseButtonPress ||
                                                  event->type() == QEvent::MouseButtonRelease ||
                                                  event->type() == QEvent::MouseButtonDblClick ||
                                                  event->type() == QEvent::ContextMenu)) {
            return true;
        }
        if (event->type() == QEvent::KeyPress &&
            changes_current_item(static_cast<QKeyEvent*>(event)->key())) {
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const auto index = task_view_->currentIndex();
        const auto opens_menu =
            key->key() == Qt::Key_Menu ||
            (key->key() == Qt::Key_F10 && key->modifiers().testFlag(Qt::ShiftModifier));
        if (opens_menu) {
            if (can_show_item_menu(index)) {
                const auto bounds = static_cast<QListView*>(task_view_)->visualRect(index);
                show_item_menu(index, task_view_->viewport()->mapToGlobal(bounds.center()));
                return true;
            }
        }
        if (index.isValid() && (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right)) {
            const auto workspace =
                task_model_->row_kind(index.row()) == TaskListModel::RowKind::workspace;
            if (!workspace && key->key() == Qt::Key_Left) {
                const auto root = index.data(TaskListModel::WorkspaceRootRole).toString();
                task_view_->setCurrentIndex(task_model_->index(
                    task_model_->row_for_workspace(mint::gui::path_from_qstring(root)), 0));
            } else if (workspace) {
                const auto expanded = index.data(TaskListModel::ExpandedWorkspaceRole).toBool();
                if ((key->key() == Qt::Key_Left && expanded) ||
                    (key->key() == Qt::Key_Right && !expanded)) {
                    const auto root = mint::gui::path_from_qstring(
                        index.data(TaskListModel::WorkspaceRootRole).toString());
                    activate_row(index.row());
                    task_view_->setCurrentIndex(
                        task_model_->index(task_model_->row_for_workspace(root), 0));
                } else if (key->key() == Qt::Key_Right && expanded) {
                    task_view_->setCurrentIndex(task_model_->index(index.row() + 1, 0));
                }
            }
            show_row_action(task_view_->currentIndex());
            return true;
        }
        const auto activates_workspace =
            index.isValid() &&
            task_model_->row_kind(index.row()) == TaskListModel::RowKind::workspace &&
            (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
             key->key() == Qt::Key_Space);
        if (activates_workspace) {
            activate_row(index.row());
            return true;
        }
        if (changes_current_item(key->key())) {
            QTimer::singleShot(0, this, [this] {
                if (task_view_->hasFocus()) {
                    show_row_action(task_view_->currentIndex());
                }
            });
        }
    }

    if (watched != task_view_->viewport()) {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseMove) {
        const auto position = static_cast<QMouseEvent*>(event)->position().toPoint();
        show_row_action(task_view_->indexAt(position));
        return QWidget::eventFilter(watched, event);
    }

    return QWidget::eventFilter(watched, event);
}

void TaskSidebar::render(const desktop::TaskWorkspaceState& state,
                         desktop::WorkspaceChange change) {
    // EN: Suppress only our intent echo; Qt still needs selection notifications for its view
    // and accessibility state.
    // ZH-CN: 只抑制本组件的意图回传，保留 Qt 视图和无障碍状态所需的选择通知。
    const QScopedValueRollback rendering(rendering_, true);
    const auto active_root = mint::gui::qstring_from_path(state.root.lexically_normal());
    const auto workspace_changed = active_workspace_root_ != active_root;
    if (workspace_changed) {
        if (!active_workspace_root_.isEmpty()) {
            workspace_scroll_positions_.insert(active_workspace_root_,
                                               task_view_->verticalScrollBar()->value());
        }
        active_workspace_root_ = active_root;
        active_workspace_expanded_ = !collapsed_workspaces_.contains(active_root);
        pending_scroll_root_ =
            workspace_scroll_positions_.contains(active_root) ? active_root : QString{};
    }
    const auto had_running_row = task_model_->running_row() >= 0;
    const auto needs_navigation =
        change == desktop::WorkspaceChange::all || change == desktop::WorkspaceChange::tasks ||
        change == desktop::WorkspaceChange::run_state || workspace_changed ||
        change == desktop::WorkspaceChange::approval ||
        (change == desktop::WorkspaceChange::selection && state.running) ||
        ((change == desktop::WorkspaceChange::content ||
          change == desktop::WorkspaceChange::error) &&
         had_running_row);
    showing_archived_tasks_ = state.showing_archived_tasks;
    const auto top_index = task_view_->indexAt(QPoint(task_view_->viewport()->width() / 2, 1));
    const auto top_id = top_index.data(TaskListModel::TaskIdRole).toString();
    const auto top_root = top_index.data(TaskListModel::WorkspaceRootRole).toString();
    const auto top_is_workspace = top_index.isValid() && task_model_->row_kind(top_index.row()) ==
                                                             TaskListModel::RowKind::workspace;
    const auto top_offset =
        top_index.isValid() ? static_cast<QListView*>(task_view_)->visualRect(top_index).top() : 0;
    if (needs_navigation) {
        hide_row_actions();
        const auto running_id = state.running_task_id.empty() && state.running_root.empty()
                                    ? state.task_id
                                    : state.running_task_id;
        const auto running_title = state.running_title.empty() && state.running_root.empty()
                                       ? state.title
                                       : state.running_title;
        task_model_->set_navigation(state.workspace_roots, state.root, state.tasks,
                                    state.task_organization, showing_archived_tasks_, state.running,
                                    running_id, running_title, active_workspace_expanded_,
                                    state.workspace_task_counts, state.running_root,
                                    state.running_status);
        update_collection_chrome();
    }

    auto row = task_model_->row_for_task_id(state.task_id);
    if (row < 0 && state.viewing_running_task()) {
        row = task_model_->running_row();
    }
    if (row < 0) {
        row = task_model_->row_for_workspace(state.root);
    }
    const auto selected_task_id = QString::fromStdString(state.task_id);
    const auto selection_changed = workspace_changed || selected_task_id_ != selected_task_id;
    const auto restore_selection = selection_changed || !task_view_->currentIndex().isValid() ||
                                   change == desktop::WorkspaceChange::all ||
                                   change == desktop::WorkspaceChange::selection ||
                                   change == desktop::WorkspaceChange::tasks;
    if (row >= 0 && restore_selection) {
        const auto index = task_model_->index(row, 0);
        if (task_view_->currentIndex() != index) {
            task_view_->setCurrentIndex(index);
        }
    } else if (row < 0) {
        task_view_->clearSelection();
        task_view_->setCurrentIndex({});
    }
    const auto next_running_row = state.running ? task_model_->running_row() : -1;
    if (running_row_ != next_running_row) {
        running_row_ = next_running_row;
        task_delegate_->set_running_row(running_row_);
        task_view_->viewport()->update();
    }
    position_running_indicator();
    if (needs_navigation) {
        task_view_->doItemsLayout();
        if (pending_scroll_root_ == active_root && !state.loading_tasks) {
            task_view_->verticalScrollBar()->setValue(
                workspace_scroll_positions_.value(active_root, 0));
            pending_scroll_root_.clear();
        } else if (!selection_changed && top_index.isValid()) {
            const auto anchor_row =
                top_is_workspace
                    ? task_model_->row_for_workspace(mint::gui::path_from_qstring(top_root))
                    : task_model_->row_for_task_id(top_id.toStdString());
            if (anchor_row >= 0) {
                const auto anchor = task_model_->index(anchor_row, 0);
                const auto anchor_top =
                    static_cast<QListView*>(task_view_)->visualRect(anchor).top();
                task_view_->verticalScrollBar()->setValue(task_view_->verticalScrollBar()->value() +
                                                          anchor_top - top_offset);
            }
        }
        position_running_indicator();
        QTimer::singleShot(0, this, [this] { refresh_row_action_at_pointer(); });
    }
    selected_task_id_ = selected_task_id;
}

void TaskSidebar::set_mutations_enabled(bool enabled) {
    if (mutations_enabled_ == enabled) {
        return;
    }
    mutations_enabled_ = enabled;
    collection_button_->setEnabled(enabled);
    add_workspace_button_->setEnabled(enabled);
    workspace_action_button_->setEnabled(enabled);
    task_action_button_->setEnabled(enabled);
    if (!enabled) {
        hide_row_actions();
    } else {
        refresh_row_action_at_pointer();
    }
    task_view_->viewport()->update();
}

void TaskSidebar::retranslate() {
    add_workspace_button_->setToolTip(text(localization::Message::gui_action_add_workspace));
    add_workspace_button_->setAccessibleName(add_workspace_button_->toolTip());
    workspace_action_button_->setToolTip(text(localization::Message::gui_action_new_task));
    workspace_action_button_->setAccessibleName(workspace_action_button_->toolTip());
    task_action_button_->setToolTip(text(localization::Message::gui_action_task_menu));
    task_action_button_->setAccessibleName(task_action_button_->toolTip());
    running_indicator_->setAccessibleName(text(localization::Message::gui_status_running));
    task_model_->retranslate();
    update_collection_chrome();
}

bool TaskSidebar::can_show_item_menu(const QModelIndex& index) const {
    if (!mutations_enabled_ || !index.isValid()) {
        return false;
    }
    if (task_model_->row_kind(index.row()) == TaskListModel::RowKind::workspace) {
        return true;
    }
    return index.row() != running_row_ && !index.data(TaskListModel::LoadingRole).toBool();
}

void TaskSidebar::show_item_menu(const QPoint& position) {
    if (!mutations_enabled_) {
        return;
    }
    const auto index = task_view_->indexAt(position);
    if (!can_show_item_menu(index)) {
        return;
    }
    show_item_menu(index, task_view_->viewport()->mapToGlobal(position));
}

void TaskSidebar::show_item_menu(const QModelIndex& index, const QPoint& global_position) {
    if (!can_show_item_menu(index)) {
        return;
    }
    if (item_menu_ != nullptr) {
        item_menu_->close();
    }
    auto* menu = new fluent::menus_toolbars::FluentMenu(QString{}, this);
    item_menu_ = menu;
    if (task_model_->row_kind(index.row()) == TaskListModel::RowKind::workspace) {
        append_workspace_actions(*menu, index);
    } else {
        append_task_actions(*menu, index);
    }
    item_menu_open_ = true;
    task_delegate_->set_hovered_index(index);
    connect(menu, &QMenu::aboutToHide, this, [this, menu] {
        if (item_menu_ == menu) {
            item_menu_ = nullptr;
            item_menu_open_ = false;
        }
        menu->deleteLater();
        QTimer::singleShot(0, this, [this] { refresh_row_action_at_pointer(); });
    });
    menu->popup(global_position);
}

void TaskSidebar::append_workspace_actions(fluent::menus_toolbars::FluentMenu& menu,
                                           const QModelIndex& index) {
    const auto root = index.data(TaskListModel::WorkspaceRootRole).toString();
    auto* create = new fluent::menus_toolbars::FluentMenuItem(
        text(localization::Message::gui_action_new_task), &menu);
    connect(create, &QAction::triggered, this, [this, root] {
        active_workspace_expanded_ = true;
        collapsed_workspaces_.remove(root);
        emit new_task_requested(root);
    });
    menu.addAction(create);

    if (!index.data(TaskListModel::WorkspaceRemovableRole).toBool()) {
        return;
    }
    menu.addSeparator();
    auto* remove = new fluent::menus_toolbars::FluentMenuItem(
        text(localization::Message::gui_action_remove_workspace), &menu);
    connect(remove, &QAction::triggered, this,
            [this, root] { emit workspace_remove_requested(root); });
    menu.addAction(remove);
}

void TaskSidebar::append_task_actions(fluent::menus_toolbars::FluentMenu& menu,
                                      const QModelIndex& index) {
    const auto task_id = index.data(TaskListModel::TaskIdRole).toString();
    const auto archived = index.data(TaskListModel::ArchivedRole).toBool();

    if (archived) {
        auto* unarchive = new fluent::menus_toolbars::FluentMenuItem(
            text(localization::Message::gui_action_unarchive_task), &menu);
        connect(unarchive, &QAction::triggered, this,
                [this, task_id] { emit task_unarchive_requested(task_id); });
        menu.addAction(unarchive);
    } else {
        const auto pinned = index.data(TaskListModel::PinnedRole).toBool();
        auto* pin = new fluent::menus_toolbars::FluentMenuItem(
            text(pinned ? localization::Message::gui_action_unpin_task
                        : localization::Message::gui_action_pin_task),
            &menu);
        connect(pin, &QAction::triggered, this,
                [this, task_id] { emit task_pin_requested(task_id); });
        menu.addAction(pin);

        auto* archive = new fluent::menus_toolbars::FluentMenuItem(
            text(localization::Message::gui_action_archive_task), &menu);
        connect(archive, &QAction::triggered, this,
                [this, task_id] { emit task_archive_requested(task_id); });
        menu.addAction(archive);
    }
}

void TaskSidebar::show_row_action(const QModelIndex& index) {
    if (item_menu_open_) {
        return;
    }
    if (!mutations_enabled_ || !index.isValid()) {
        task_delegate_->set_hovered_index({});
        hide_row_actions();
        return;
    }

    task_delegate_->set_hovered_index(index);

    const auto kind = task_model_->row_kind(index.row());
    if (kind == TaskListModel::RowKind::task && !can_show_item_menu(index)) {
        hide_row_actions();
        return;
    }
    if (action_row_ == index.row()) {
        position_row_action();
        return;
    }

    const auto previous_row = action_row_;
    action_row_ = index.row();
    action_workspace_root_.clear();
    task_delegate_->set_action_row(action_row_);
    workspace_action_button_->hide();
    task_action_button_->hide();

    if (kind == TaskListModel::RowKind::workspace) {
        action_workspace_root_ = index.data(TaskListModel::WorkspaceRootRole).toString();
    }

    if (previous_row >= 0) {
        const auto previous_bounds =
            static_cast<QListView*>(task_view_)->visualRect(task_model_->index(previous_row, 0));
        if (previous_bounds.isValid()) {
            task_view_->viewport()->update(previous_bounds);
        }
    }
    position_row_action();
    const auto bounds = static_cast<QListView*>(task_view_)->visualRect(index);
    if (bounds.isValid()) {
        task_view_->viewport()->update(bounds);
    }
}

void TaskSidebar::hide_row_actions() {
    const auto previous_row = action_row_;
    action_row_ = -1;
    action_workspace_root_.clear();
    task_delegate_->set_action_row(-1);
    workspace_action_button_->hide();
    task_action_button_->hide();
    if (previous_row < 0) {
        return;
    }
    const auto bounds =
        static_cast<QListView*>(task_view_)->visualRect(task_model_->index(previous_row, 0));
    if (bounds.isValid()) {
        task_view_->viewport()->update(bounds);
    }
}

void TaskSidebar::refresh_row_action_at_pointer() {
    if (item_menu_open_) {
        return;
    }
    const auto* viewport = task_view_->viewport();
    if (!viewport->underMouse() && !workspace_action_button_->underMouse() &&
        !task_action_button_->underMouse()) {
        task_delegate_->set_hovered_index({});
        hide_row_actions();
        return;
    }
    const auto position = viewport->mapFromGlobal(QCursor::pos());
    show_row_action(viewport->rect().contains(position) ? task_view_->indexAt(position)
                                                        : QModelIndex{});
}

void TaskSidebar::position_row_action() {
    const auto index = task_model_->index(action_row_, 0);
    if (!index.isValid()) {
        hide_row_actions();
        return;
    }
    const auto bounds = static_cast<QListView*>(task_view_)->visualRect(index);
    const auto action_bounds = TaskItemDelegate::trailing_action_rect(bounds);
    if (!bounds.isValid() || !task_view_->viewport()->rect().contains(action_bounds)) {
        hide_row_actions();
        return;
    }

    auto* action = task_model_->row_kind(action_row_) == TaskListModel::RowKind::workspace
                       ? static_cast<QWidget*>(workspace_action_button_)
                       : static_cast<QWidget*>(task_action_button_);
    action->setGeometry(
        QRect(task_view_->viewport()->mapTo(this, action_bounds.topLeft()), action_bounds.size()));
    action->show();
    action->raise();
}

void TaskSidebar::update_collection_chrome() {
    collection_button_->setText(text(showing_archived_tasks_
                                         ? localization::Message::gui_task_archived_section
                                         : localization::Message::gui_workspace_section));
    collection_button_->setToolTip(text(localization::Message::gui_action_task_menu));
    collection_button_->setAccessibleName(collection_button_->text());
    collection_view_action_->setText(text(showing_archived_tasks_
                                              ? localization::Message::gui_action_show_tasks
                                              : localization::Message::gui_action_show_archived));
    task_view_->setPlaceholderText(text(showing_archived_tasks_
                                            ? localization::Message::gui_task_archived_empty
                                            : localization::Message::gui_task_empty));
    task_view_->setAccessibleName(collection_button_->text());
}

void TaskSidebar::position_running_indicator() {
    const auto row = running_row_;
    if (row < 0 || task_model_->index(row, 0).data(TaskListModel::AwaitingApprovalRole).toBool()) {
        running_indicator_->setIsActive(false);
        running_indicator_->hide();
        return;
    }
    const auto bounds = static_cast<QListView*>(task_view_)->visualRect(task_model_->index(row, 0));
    if (!bounds.isValid() || !task_view_->viewport()->rect().intersects(bounds)) {
        running_indicator_->setIsActive(false);
        running_indicator_->hide();
        return;
    }
    const auto slot = task_model_->row_kind(row) == TaskListModel::RowKind::workspace
                          ? TaskItemDelegate::workspace_icon_rect(bounds)
                          : TaskItemDelegate::trailing_action_rect(bounds);
    const auto x = slot.center().x() - running_indicator_->width() / 2;
    const auto y = slot.center().y() - running_indicator_->height() / 2;
    const QRect indicator_bounds(QPoint(x, y), running_indicator_->size());
    if (!task_view_->viewport()->rect().contains(indicator_bounds)) {
        running_indicator_->setIsActive(false);
        running_indicator_->hide();
        return;
    }
    running_indicator_->move(task_view_->viewport()->mapTo(this, indicator_bounds.topLeft()));
    running_indicator_->show();
    running_indicator_->raise();
    running_indicator_->setIsActive(true);
}

} // namespace mint::ui
