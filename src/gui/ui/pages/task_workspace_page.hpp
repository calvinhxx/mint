#pragma once

#include "application/model_settings_controller.hpp"
#include "application/preferences/preferences_controller.hpp"
#include "application/task_workspace_controller.hpp"

#include <QHash>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <filesystem>
#include <optional>

class QFileDialog;

namespace mint::ui {

class EvidencePanel;
class RunConversation;
class SettingsPanel;
class TaskComposer;
class TaskSidebar;
class WorkspaceSplitView;

} // namespace mint::ui

namespace fluent::collections {
class DrawerView;
}
namespace fluent::basicinput {
class Button;
}

namespace mint::ui {

class TaskWorkspacePage final : public QWidget {
    Q_OBJECT

  public:
    explicit TaskWorkspacePage(desktop::TaskWorkspaceController& controller,
                               desktop::ModelSettingsController& model_settings,
                               desktop::PreferencesController& preferences,
                               QWidget* parent = nullptr);
    ~TaskWorkspacePage() override;

    void retranslate();
    void refresh_theme();
    void open_settings();
    [[nodiscard]] int navigation_surface_width() const;

  signals:
    void navigation_surface_width_changed(int width);
    void task_header_changed(QString title, bool can_resume);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    QWidget* build_main_surface();
    void connect_actions();
    void render(desktop::WorkspaceChange change);
    void choose_project();
    void show_evidence(const QString& content);
    void update_composer_clearance(int composer_height);
    void position_jump_to_latest();
    void render_model_gate();

    desktop::TaskWorkspaceController& controller_;
    desktop::ModelSettingsController& model_settings_;
    desktop::PreferencesController& preferences_;
    TaskSidebar* sidebar_ = nullptr;
    RunConversation* conversation_ = nullptr;
    TaskComposer* composer_ = nullptr;
    WorkspaceSplitView* split_view_ = nullptr;
    QWidget* main_surface_ = nullptr;
    QWidget* conversation_canvas_ = nullptr;
    fluent::basicinput::Button* jump_to_latest_ = nullptr;
    EvidencePanel* evidence_panel_ = nullptr;
    fluent::collections::DrawerView* evidence_drawer_ = nullptr;
    SettingsPanel* settings_panel_ = nullptr;
    fluent::collections::DrawerView* settings_drawer_ = nullptr;
    QPointer<QFileDialog> project_dialog_;
    std::optional<std::filesystem::path> pending_new_task_root_;
    QHash<QString, QString> drafts_;
    QString draft_key_;
    QString header_title_;
    bool header_can_resume_ = false;
    bool migration_prompt_shown_ = false;
};

} // namespace mint::ui
