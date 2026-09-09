#pragma once

#include "application/task_workspace_controller.hpp"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QWidget>

class QPoint;
class QModelIndex;
class QEvent;

namespace fluent::collections {
class ListView;
}
namespace fluent::basicinput {
class Button;
class DropDownButton;
} // namespace fluent::basicinput
namespace fluent::status_info {
class ProgressRing;
}
namespace fluent::menus_toolbars {
class FluentMenu;
class FluentMenuItem;
} // namespace fluent::menus_toolbars

namespace mint::ui {

class TaskItemDelegate;
class TaskListModel;

class TaskSidebar final : public QWidget {
    Q_OBJECT

  public:
    explicit TaskSidebar(QWidget* parent = nullptr);

    void render(const desktop::TaskWorkspaceState& state, desktop::WorkspaceChange change);
    void set_mutations_enabled(bool enabled);
    void retranslate();

  signals:
    void add_workspace_requested();
    void new_task_requested(QString root);
    void workspace_selected(QString root);
    void workspace_remove_requested(QString root);
    void task_selected(QString task_id);
    void task_pin_requested(QString task_id);
    void task_archive_requested(QString task_id);
    void task_unarchive_requested(QString task_id);
    void archived_view_requested(bool show);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void activate_row(int row);
    [[nodiscard]] bool can_show_item_menu(const QModelIndex& index) const;
    void show_item_menu(const QPoint& position);
    void show_item_menu(const QModelIndex& index, const QPoint& global_position);
    void append_workspace_actions(fluent::menus_toolbars::FluentMenu& menu,
                                  const QModelIndex& index);
    void append_task_actions(fluent::menus_toolbars::FluentMenu& menu, const QModelIndex& index);
    void update_collection_chrome();
    void show_row_action(const QModelIndex& index);
    void hide_row_actions();
    void refresh_row_action_at_pointer();
    void position_row_action();
    void position_running_indicator();

    fluent::basicinput::DropDownButton* collection_button_ = nullptr;
    fluent::menus_toolbars::FluentMenuItem* collection_view_action_ = nullptr;
    fluent::basicinput::Button* add_workspace_button_ = nullptr;
    fluent::collections::ListView* task_view_ = nullptr;
    TaskListModel* task_model_ = nullptr;
    TaskItemDelegate* task_delegate_ = nullptr;
    fluent::basicinput::Button* workspace_action_button_ = nullptr;
    fluent::basicinput::Button* task_action_button_ = nullptr;
    fluent::status_info::ProgressRing* running_indicator_ = nullptr;
    int action_row_ = -1;
    QString action_workspace_root_;
    int running_row_ = -1;
    QString active_workspace_root_;
    bool active_workspace_expanded_ = true;
    QSet<QString> collapsed_workspaces_;
    QHash<QString, int> workspace_scroll_positions_;
    QString pending_scroll_root_;
    QString selected_task_id_;
    bool showing_archived_tasks_ = false;
    bool mutations_enabled_ = true;
    bool rendering_ = false;
    bool item_menu_open_ = false;
    QPointer<fluent::menus_toolbars::FluentMenu> item_menu_;
};

} // namespace mint::ui
