#include "conversation_transcript.hpp"

#include "ui/delegates/run_timeline_delegate.hpp"
#include "ui/models/run_timeline_model.hpp"
#include "ui/support/localized_text.hpp"
#include "ui/theme/layout_metrics.hpp"

#include <QMouseEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTimer>

namespace mint::ui {

ConversationTranscript::ConversationTranscript(QWidget* parent)
    : fluent::collections::ListView(parent), timeline_model_(new RunTimelineModel(this)),
      timeline_delegate_(new RunTimelineDelegate(this)) {
    setObjectName(QStringLiteral("MintConversationTranscript"));
    setAccessibleName(text(localization::Message::gui_task_conversation));
    setModel(timeline_model_);
    setItemDelegate(timeline_delegate_);
    setSelectionMode(fluent::collections::SelectionMode::None);
    setSelectionIndicatorVisible(false);
    setBorderVisible(false);
    setBackgroundVisible(false);
    setCanReorderItems(false);
    setScrollChainingEnabled(false);
    setUniformItemSizes(false);
    setWordWrap(true);
    setSpacing(0);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setProperty("fluentPreserveParentSurface", true);
    viewport()->setProperty("fluentPreserveParentSurface", true);
    viewport()->setAutoFillBackground(false);
    viewport()->setAttribute(Qt::WA_NoSystemBackground, true);
    (void)timeline_model_->set_bottom_inset(metrics::composer_content_clearance);
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this] { update_tail_navigation(); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { update_follow_state(); });
}

void ConversationTranscript::render(const desktop::TaskWorkspaceState& state,
                                    desktop::WorkspaceChange change) {
    if (change != desktop::WorkspaceChange::all && change != desktop::WorkspaceChange::selection &&
        change != desktop::WorkspaceChange::content &&
        change != desktop::WorkspaceChange::event_appended) {
        return;
    }

    if (change == desktop::WorkspaceChange::all || change == desktop::WorkspaceChange::selection) {
        follows_tail_ = true;
    }
    auto* scrollbar = verticalScrollBar();
    const auto previous_value = scrollbar->value();
    const auto follows_tail = follows_tail_;
    if (change != desktop::WorkspaceChange::event_appended ||
        !timeline_model_->append_latest_event(state)) {
        timeline_model_->set_state(state);
    }

    QTimer::singleShot(0, this, [this, follows_tail, previous_value] {
        auto* current = verticalScrollBar();
        current->setValue(follows_tail ? current->maximum()
                                       : qMin(previous_value, current->maximum()));
        refreshFluentScrollChrome();
        update_tail_navigation();
    });
}

void ConversationTranscript::retranslate() {
    setAccessibleName(text(localization::Message::gui_task_conversation));
    timeline_model_->retranslate();
    doItemsLayout();
}

void ConversationTranscript::set_bottom_inset(int height) {
    const auto follows_tail = follows_tail_;
    if (!timeline_model_->set_bottom_inset(height)) {
        return;
    }
    doItemsLayout();
    QTimer::singleShot(0, this, [this, follows_tail] {
        if (follows_tail) {
            verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        }
        refreshFluentScrollChrome();
        update_tail_navigation();
    });
}

void ConversationTranscript::scroll_to_latest() {
    follows_tail_ = true;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    refreshFluentScrollChrome();
    update_tail_navigation();
}

void ConversationTranscript::select_evidence(std::size_t index) {
    const auto evidence = timeline_model_->evidence_at(index);
    if (!evidence.isEmpty()) {
        emit evidence_selected(evidence);
    }
}

void ConversationTranscript::select_first_evidence() {
    const auto evidence = timeline_model_->first_evidence();
    if (!evidence.isEmpty()) {
        emit evidence_selected(evidence);
    }
}

QString ConversationTranscript::toPlainText() const {
    return timeline_model_->plain_text();
}

int ConversationTranscript::maximum_content_width() const noexcept {
    return metrics::conversation_maximum_width;
}

int ConversationTranscript::content_horizontal_padding() const noexcept {
    return qMax(metrics::conversation_horizontal_padding,
                (viewport()->width() - metrics::conversation_maximum_width) / 2);
}

void ConversationTranscript::mousePressEvent(QMouseEvent* event) {
    pressed_evidence_ = QPersistentModelIndex{};
    const auto index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && index.isValid() &&
        !index.data(RunTimelineModel::EvidenceRole).toString().isEmpty() &&
        timeline_delegate_->evidence_action_rect(option_for(index), index)
            .contains(event->position().toPoint())) {
        pressed_evidence_ = index;
    }
    fluent::collections::ListView::mousePressEvent(event);
}

void ConversationTranscript::mouseReleaseEvent(QMouseEvent* event) {
    const auto pressed = pressed_evidence_;
    pressed_evidence_ = QPersistentModelIndex{};
    const auto index = indexAt(event->position().toPoint());
    const auto activates_evidence =
        event->button() == Qt::LeftButton && pressed.isValid() && pressed == index &&
        timeline_delegate_->evidence_action_rect(option_for(index), index)
            .contains(event->position().toPoint());
    const auto evidence =
        activates_evidence ? index.data(RunTimelineModel::EvidenceRole).toString() : QString{};
    QPointer<ConversationTranscript> guard(this);
    fluent::collections::ListView::mouseReleaseEvent(event);
    if (guard && pressed.isValid() && !evidence.isEmpty() &&
        pressed.data(RunTimelineModel::EvidenceRole).toString() == evidence) {
        emit evidence_selected(evidence);
    }
}

void ConversationTranscript::resizeEvent(QResizeEvent* event) {
    fluent::collections::ListView::resizeEvent(event);
    doItemsLayout();
    refreshFluentScrollChrome();
    update_tail_navigation();
    if (follows_tail_) {
        QTimer::singleShot(0, this, [this] { scroll_to_latest(); });
    }
}

QStyleOptionViewItem ConversationTranscript::option_for(const QModelIndex& index) const {
    QStyleOptionViewItem option;
    initViewItemOption(&option);
    option.rect = visualRect(index);
    option.widget = this;
    return option;
}

void ConversationTranscript::update_follow_state() {
    const auto* scrollbar = verticalScrollBar();
    follows_tail_ = scrollbar->maximum() - scrollbar->value() <= metrics::space_sm;
    update_tail_navigation();
}

void ConversationTranscript::update_tail_navigation() {
    const auto* scrollbar = verticalScrollBar();
    const auto available = scrollbar->maximum() > scrollbar->minimum() && !follows_tail_;
    if (jump_to_latest_available_ == available) {
        return;
    }
    jump_to_latest_available_ = available;
    emit jump_to_latest_availability_changed(available);
}

} // namespace mint::ui
