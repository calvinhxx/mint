#include "approval_panel.hpp"

#include "ui/support/localized_text.hpp"

#include "mint/localization/localization.hpp"

#include <FluentQt/BasicInput.h>
#include <FluentQt/Design.h>
#include <FluentQt/TextFields.h>

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QStyle>
#include <QVBoxLayout>

namespace mint::ui {
namespace {

using localization::arg;
using localization::Message;
using localization::Placeholder;

QString join_arguments(const std::vector<std::string>& arguments) {
    QStringList result;
    for (const auto& argument : arguments) {
        result.push_back(QString::fromStdString(argument));
    }
    return result.join(QLatin1Char('\n'));
}

} // namespace

ApprovalPanel::ApprovalPanel(QWidget* parent) : fluent::layout::Card(parent) {
    setObjectName(QStringLiteral("MintApprovalCard"));
    setAppearance(fluent::layout::Card::LayerAlt);
    setBorderVisible(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(8);

    eyebrow_ = new fluent::textfields::Label(this);
    eyebrow_->setObjectName(QStringLiteral("MintAttentionText"));
    eyebrow_->setFluentTypography(Typography::FontRole::Caption);
    eyebrow_->setText(text(Message::gui_event_approval_requested));
    layout->addWidget(eyebrow_);

    title_ = new fluent::textfields::Label(this);
    title_->setObjectName(QStringLiteral("MintPrimaryText"));
    title_->setFluentTypography(Typography::FontRole::Subtitle);
    layout->addWidget(title_);

    note_ = new fluent::textfields::Label(this);
    note_->setObjectName(QStringLiteral("MintSecondaryText"));
    note_->setFluentTypography(Typography::FontRole::Body);
    note_->setWordWrap(true);
    note_->setText(text(Message::gui_approval_note));
    layout->addWidget(note_);

    target_ = new fluent::textfields::Label(this);
    target_->setObjectName(QStringLiteral("MintPrimaryText"));
    target_->setFluentTypography(Typography::FontRole::Body);
    target_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    target_->setWordWrap(true);
    layout->addWidget(target_);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    evidence_button_ =
        new fluent::basicinput::Button(text(Message::gui_action_open_evidence), this);
    evidence_button_->setIconGlyph(Typography::Icons::View);
    evidence_button_->setFluentLayout(fluent::basicinput::Button::IconBefore);
    actions->addWidget(evidence_button_);
    actions->addStretch();

    deny_button_ = new fluent::basicinput::Button(text(Message::gui_action_deny), this);
    deny_button_->setIconGlyph(Typography::Icons::Cancel);
    deny_button_->setFluentLayout(fluent::basicinput::Button::IconBefore);
    approve_button_ = new fluent::basicinput::Button(text(Message::gui_action_approve_once), this);
    approve_button_->setIconGlyph(Typography::Icons::CheckMark);
    approve_button_->setFluentLayout(fluent::basicinput::Button::IconBefore);
    approve_button_->setFluentStyle(fluent::basicinput::Button::Accent);
    actions->addWidget(deny_button_);
    actions->addWidget(approve_button_);
    layout->addLayout(actions);

    connect(evidence_button_, &QPushButton::clicked, this, &ApprovalPanel::evidence_requested);
    connect(deny_button_, &QPushButton::clicked, this, &ApprovalPanel::denied);
    connect(approve_button_, &QPushButton::clicked, this, &ApprovalPanel::approved);
    hide();
}

void ApprovalPanel::set_request(const desktop::ApprovalRequest& request) {
    request_ = request;
    QStringList targets;
    const auto truncated =
        request.kind == desktop::ApprovalKind::changeset && request.diff_truncated;
    approve_button_->setEnabled(!truncated);
    note_->setObjectName(truncated ? QStringLiteral("MintCriticalText")
                                   : QStringLiteral("MintSecondaryText"));
    note_->setText(truncated ? text(Message::gui_approval_diff_truncated)
                             : text(Message::gui_approval_note));
    note_->style()->unpolish(note_);
    note_->style()->polish(note_);
    if (request.kind == desktop::ApprovalKind::changeset) {
        title_->setText(text(Message::gui_approval_changeset_title,
                             {arg(Placeholder::count, request.paths.size())}));
        for (const auto& path : request.paths) {
            targets.push_back(QString::fromStdString(path));
        }
        evidence_ = QString::fromStdString(request.unified_diff);
    } else {
        title_->setText(text(Message::gui_approval_command_title));
        target_->setText(text(Message::gui_approval_command_detail,
                              {arg(Placeholder::program, request.program),
                               arg(Placeholder::cwd, request.working_directory)}));
        evidence_ = join_arguments(request.arguments);
    }
    if (!targets.isEmpty()) {
        target_->setText(targets.join(QLatin1Char('\n')));
    }
    evidence_button_->setEnabled(!evidence_.isEmpty());
    show();
}

void ApprovalPanel::clear() {
    request_.reset();
    evidence_.clear();
    target_->clear();
    approve_button_->setEnabled(true);
    hide();
}

void ApprovalPanel::retranslate() {
    eyebrow_->setText(text(Message::gui_event_approval_requested));
    evidence_button_->setText(text(Message::gui_action_open_evidence));
    deny_button_->setText(text(Message::gui_action_deny));
    approve_button_->setText(text(Message::gui_action_approve_once));
    if (request_.has_value()) {
        const auto request = *request_;
        set_request(request);
    }
}

QString ApprovalPanel::evidence_text() const {
    return evidence_;
}

} // namespace mint::ui
