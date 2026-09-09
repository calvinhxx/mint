#include "task_composer.hpp"

#include "ui/components/underline_free_input.hpp"
#include "ui/support/localized_text.hpp"
#include "ui/theme/layout_metrics.hpp"

#include <FluentQt/BasicInput.h>
#include <FluentQt/Design.h>
#include <FluentQt/TextFields.h>

#include <QApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QStyle>

#include <utility>

namespace mint::ui {

TaskComposer::TaskComposer(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MintComposer"));
    setAttribute(Qt::WA_StyledBackground, true);
    setProperty("focused", false);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(metrics::space_md, metrics::space_sm, metrics::space_sm,
                               metrics::space_sm);
    layout->setSpacing(metrics::space_sm);

    input_ = new UnderlineFreeTextEdit(this);
    input_->setObjectName(QStringLiteral("MintTaskInput"));
    input_->set_surface_visible(false);
    input_->setMinVisibleLines(2);
    input_->setMaxVisibleLines(6);
    input_->setTabChangesFocus(true);
    input_->setContentMargins(
        QMargins(metrics::space_md, metrics::space_sm, metrics::space_md, metrics::space_sm));
    input_->setLineHeight(
        QFontMetrics(Typography::fontStyle(Typography::FontRole::Body).toQFont()).lineSpacing());
    layout->addWidget(input_, 1);

    action_ = new fluent::basicinput::Button(this);
    action_->setObjectName(QStringLiteral("MintTaskAction"));
    action_->setFluentLayout(fluent::basicinput::Button::IconOnly);
    action_->setFluentStyle(fluent::basicinput::Button::Accent);
    action_->setFluentSize(fluent::basicinput::Button::Small);
    action_->setFixedSize(metrics::composer_action_size, metrics::composer_action_size);
    action_->setCornerRadii(
        QMargins(metrics::composer_action_size / 2, metrics::composer_action_size / 2,
                 metrics::composer_action_size / 2, metrics::composer_action_size / 2));
    action_->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(action_, 0, Qt::AlignBottom);
    QWidget::setTabOrder(input_, action_);

    connect(input_, &fluent::textfields::TextEdit::textChanged, this, [this] { update_action(); });
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget*, QWidget* focused) { update_focus_state(focused); });
    connect(action_, &QPushButton::clicked, this, [this] {
        if (running_) {
            emit cancel_requested();
            return;
        }
        const auto question = input_->toPlainText().trimmed();
        if (workspace_available_ && run_available_ && !question.isEmpty()) {
            emit submitted(question.toUtf8().toStdString());
        }
    });
    retranslate();
}

TaskComposer::~TaskComposer() {
    // EN: QWidget can change child focus before QObject disconnects these callbacks.
    // ZH-CN: QWidget 销毁子控件时可能改变焦点，此时 QObject 尚未断开这些回调。
    QObject::disconnect(qApp, nullptr, this, nullptr);
    QObject::disconnect(input_, nullptr, this, nullptr);
    QObject::disconnect(action_, nullptr, this, nullptr);
}

void TaskComposer::set_running(bool running) {
    running_ = running;
    input_->setEnabled(workspace_available_ && !running);
    update_action();
}

void TaskComposer::set_run_available(bool available) {
    run_available_ = available;
    update_action();
}

void TaskComposer::set_workspace_available(bool available) {
    if (workspace_available_ == available) {
        return;
    }
    workspace_available_ = available;
    input_->setEnabled(workspace_available_ && !running_);
    retranslate();
}

void TaskComposer::set_model_unavailable_reason(QString reason) {
    if (model_unavailable_reason_ == reason) {
        return;
    }
    model_unavailable_reason_ = std::move(reason);
    retranslate();
}

QString TaskComposer::draft() const {
    return input_->toPlainText();
}

void TaskComposer::set_draft(const QString& draft) {
    if (input_->toPlainText() != draft) {
        input_->setPlainText(draft);
    }
}

void TaskComposer::focus_input() {
    input_->clear();
    input_->setFocus(Qt::ShortcutFocusReason);
}

void TaskComposer::retranslate() {
    input_->setPlaceholderText(!workspace_available_
                                   ? text(localization::Message::gui_workspace_required)
                               : !model_unavailable_reason_.isEmpty()
                                   ? model_unavailable_reason_
                                   : text(localization::Message::gui_task_input_placeholder));
    input_->setAccessibleName(text(localization::Message::gui_task_input_label));
    if (auto* editor = input_->focusProxy(); editor != nullptr) {
        editor->setAccessibleName(input_->accessibleName());
    }
    update_action();
}

void TaskComposer::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (event->oldSize().height() != event->size().height()) {
        emit height_changed(event->size().height());
    }
}

void TaskComposer::update_action() {
    action_->setIconGlyph(running_ ? Typography::Icons::Stop : Typography::Icons::Up,
                          Typography::IconSize::Standard);
    action_->setIconOffset(running_ ? QPoint{} : QPoint(0, -1));
    const auto action_text = text(running_ ? localization::Message::gui_action_stop
                                           : localization::Message::gui_action_send);
    action_->setToolTip(
        !workspace_available_ ? text(localization::Message::gui_workspace_required)
        : !running_ && !model_unavailable_reason_.isEmpty() ? model_unavailable_reason_
        : !running_ && !run_available_ ? text(localization::Message::gui_task_wait_for_running_task)
                                       : action_text);
    action_->setAccessibleName(action_text);
    action_->setAccessibleDescription(action_->toolTip());
    action_->setEnabled(running_ || (workspace_available_ && run_available_ &&
                                     !input_->toPlainText().trimmed().isEmpty()));
    action_->setCriticalOnHover(running_);
}

void TaskComposer::update_focus_state(QWidget* focused) {
    const auto contains_focus = focused != nullptr && (focused == this || isAncestorOf(focused));
    if (property("focused").toBool() == contains_focus) {
        return;
    }
    setProperty("focused", contains_focus);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

} // namespace mint::ui
