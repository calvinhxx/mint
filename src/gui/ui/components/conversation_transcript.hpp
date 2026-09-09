#pragma once

#include "application/task_workspace_controller.hpp"

#include <FluentQt/Collections.h>

#include <QPersistentModelIndex>

namespace mint::ui {

class RunTimelineDelegate;
class RunTimelineModel;

class ConversationTranscript final : public fluent::collections::ListView {
    Q_OBJECT

  public:
    explicit ConversationTranscript(QWidget* parent = nullptr);

    void render(const desktop::TaskWorkspaceState& state, desktop::WorkspaceChange change);
    void retranslate();
    void set_bottom_inset(int height);
    void scroll_to_latest();
    void select_evidence(std::size_t index);
    void select_first_evidence();

    [[nodiscard]] QString toPlainText() const;
    [[nodiscard]] int maximum_content_width() const noexcept;
    [[nodiscard]] int content_horizontal_padding() const noexcept;

  signals:
    void evidence_selected(QString content);
    void jump_to_latest_availability_changed(bool available);

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    [[nodiscard]] QStyleOptionViewItem option_for(const QModelIndex& index) const;
    void update_follow_state();
    void update_tail_navigation();

    RunTimelineModel* timeline_model_ = nullptr;
    RunTimelineDelegate* timeline_delegate_ = nullptr;
    QPersistentModelIndex pressed_evidence_;
    bool follows_tail_ = true;
    bool jump_to_latest_available_ = false;
};

} // namespace mint::ui
