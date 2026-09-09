#pragma once

#include "application/task_workspace_controller.hpp"

#include <QWidget>

namespace fluent::textfields {
class Label;
}

namespace mint::ui {

class ApprovalPanel;
class ConversationTranscript;

class RunConversation final : public QWidget {
    Q_OBJECT

  public:
    explicit RunConversation(QWidget* parent = nullptr);

    void render(const desktop::TaskWorkspaceState& state, desktop::WorkspaceChange change);
    void retranslate();
    void set_bottom_inset(int height);
    void scroll_to_latest();

  signals:
    void approval_resolved(bool approved);
    void evidence_selected(QString content);
    void jump_to_latest_availability_changed(bool available);

  private:
    void update_scroll_clearance();
    void update_jump_to_latest_availability();

    fluent::textfields::Label* notice_ = nullptr;
    ConversationTranscript* transcript_ = nullptr;
    ApprovalPanel* approval_ = nullptr;
    QWidget* action_layer_ = nullptr;
    QWidget* action_content_ = nullptr;
    int composer_clearance_ = 0;
    bool transcript_jump_available_ = false;
    bool jump_available_ = false;
};

} // namespace mint::ui
