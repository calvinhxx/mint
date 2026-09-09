#include "run_conversation.hpp"

#include "ui/components/approval_panel.hpp"
#include "ui/components/conversation_transcript.hpp"
#include "ui/support/localized_text.hpp"
#include "ui/theme/layout_metrics.hpp"

#include <FluentQt/Design.h>
#include <FluentQt/TextFields.h>

#include <QGridLayout>
#include <QHBoxLayout>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

namespace mint::ui {
namespace {

using localization::arg;
using localization::Message;
using localization::Placeholder;

} // namespace

RunConversation::RunConversation(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MintRunConversation"));
    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    transcript_ = new ConversationTranscript(this);
    layout->addWidget(transcript_, 0, 0);

    action_layer_ = new QWidget(this);
    action_layer_->setObjectName(QStringLiteral("MintConversationActionLayer"));
    action_layer_->setAutoFillBackground(false);
    action_layer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    auto* footer_band_layout = new QHBoxLayout(action_layer_);
    footer_band_layout->setContentsMargins(metrics::conversation_horizontal_padding,
                                           metrics::space_md,
                                           metrics::conversation_horizontal_padding, 0);
    footer_band_layout->setSpacing(0);
    footer_band_layout->addStretch();

    action_content_ = new QWidget(action_layer_);
    action_content_->setMaximumWidth(metrics::conversation_maximum_width);
    action_content_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* footer_content_layout = new QVBoxLayout(action_content_);
    footer_content_layout->setContentsMargins(0, 0, 0, 0);
    footer_content_layout->setSpacing(metrics::space_sm);

    approval_ = new ApprovalPanel(action_content_);
    footer_content_layout->addWidget(approval_);

    notice_ = new fluent::textfields::Label(action_content_);
    notice_->setObjectName(QStringLiteral("MintAttentionText"));
    notice_->setFluentTypography(Typography::FontRole::Caption);
    notice_->setWordWrap(true);
    notice_->hide();
    footer_content_layout->addWidget(notice_);

    footer_band_layout->addWidget(action_content_, 1);
    footer_band_layout->addStretch();
    action_layer_->hide();
    layout->addWidget(action_layer_, 0, 0, Qt::AlignBottom);
    action_layer_->raise();
    set_bottom_inset(metrics::composer_content_clearance);

    connect(transcript_, &ConversationTranscript::evidence_selected, this,
            &RunConversation::evidence_selected);
    connect(transcript_, &ConversationTranscript::jump_to_latest_availability_changed, this,
            [this](bool available) {
                transcript_jump_available_ = available;
                update_jump_to_latest_availability();
            });
    connect(approval_, &ApprovalPanel::approved, this, [this] { emit approval_resolved(true); });
    connect(approval_, &ApprovalPanel::denied, this, [this] { emit approval_resolved(false); });
    connect(approval_, &ApprovalPanel::evidence_requested, this,
            [this] { emit evidence_selected(approval_->evidence_text()); });
}

void RunConversation::retranslate() {
    approval_->retranslate();
    transcript_->retranslate();
    update_scroll_clearance();
}

void RunConversation::set_bottom_inset(int height) {
    composer_clearance_ = qMax(0, height);
    action_layer_->layout()->setContentsMargins(
        metrics::conversation_horizontal_padding, metrics::space_md,
        metrics::conversation_horizontal_padding, composer_clearance_);
    action_layer_->updateGeometry();
    update_scroll_clearance();
}

void RunConversation::scroll_to_latest() {
    transcript_->scroll_to_latest();
}

void RunConversation::render(const desktop::TaskWorkspaceState& state,
                             desktop::WorkspaceChange change) {
    transcript_->render(state, change);
    if (change == desktop::WorkspaceChange::event_appended) {
        return;
    }

    const auto showing_approval = state.viewing_running_task() && state.approval.has_value();
    if (showing_approval) {
        approval_->set_request(*state.approval);
    } else {
        approval_->clear();
    }

    bool notice_visible = false;
    if (!state.error.empty()) {
        notice_->setObjectName(QStringLiteral("MintCriticalText"));
        notice_->setText(
            text(Message::gui_error_runtime, {arg(Placeholder::message, state.error)}));
        notice_->show();
        notice_visible = true;
    } else if (state.notice == desktop::WorkspaceNotice::incomplete_history) {
        notice_->setObjectName(QStringLiteral("MintAttentionText"));
        notice_->setText(text(Message::gui_history_incomplete));
        notice_->show();
        notice_visible = true;
    } else if (state.notice == desktop::WorkspaceNotice::invalid_workspace) {
        notice_->setObjectName(QStringLiteral("MintAttentionText"));
        notice_->setText(text(Message::gui_workspace_invalid));
        notice_->show();
        notice_visible = true;
    } else {
        notice_->hide();
    }
    const auto action_visible = showing_approval || notice_visible;
    action_layer_->setVisible(action_visible);
    if (action_visible) {
        action_layer_->raise();
    }
    update_jump_to_latest_availability();
    action_layer_->updateGeometry();
    action_content_->updateGeometry();
    update_scroll_clearance();
    QTimer::singleShot(0, this, [this] { update_scroll_clearance(); });
    notice_->style()->unpolish(notice_);
    notice_->style()->polish(notice_);
}

void RunConversation::update_scroll_clearance() {
    const auto action_clearance =
        action_layer_->isHidden() ? 0 : action_content_->sizeHint().height() + metrics::space_md;
    transcript_->set_bottom_inset(composer_clearance_ + action_clearance);
}

void RunConversation::update_jump_to_latest_availability() {
    const auto available = transcript_jump_available_ && action_layer_->isHidden();
    if (jump_available_ == available) {
        return;
    }
    jump_available_ = available;
    emit jump_to_latest_availability_changed(available);
}

} // namespace mint::ui
