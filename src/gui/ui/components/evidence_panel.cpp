#include "evidence_panel.hpp"

#include "ui/support/localized_text.hpp"

#include <FluentQt/BasicInput.h>
#include <FluentQt/Design.h>
#include <FluentQt/TextFields.h>

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QVBoxLayout>

namespace mint::ui {

EvidencePanel::EvidencePanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MintEvidencePanel"));
    setMinimumWidth(320);
    setMaximumWidth(440);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);

    auto* header = new QHBoxLayout;
    title_ = new fluent::textfields::Label(this);
    title_->setObjectName(QStringLiteral("MintPrimaryText"));
    title_->setFluentTypography(Typography::FontRole::BodyStrong);
    header->addWidget(title_);
    header->addStretch();
    close_ = new fluent::basicinput::Button(this);
    close_->setIconGlyph(Typography::Icons::ChromeClose);
    close_->setFluentLayout(fluent::basicinput::Button::IconOnly);
    header->addWidget(close_);
    layout->addLayout(header);

    content_ = new fluent::textfields::TextEdit(this);
    content_->setReadOnly(true);
    content_->setFontRole(Typography::FontRole::Caption);
    content_->setMinVisibleLines(24);
    content_->setMaxVisibleLines(36);
    layout->addWidget(content_, 1);

    connect(close_, &QPushButton::clicked, this, &EvidencePanel::close_requested);
    retranslate();
}

void EvidencePanel::set_evidence(const QString& content) {
    evidence_ = content;
    content_->setPlainText(evidence_.isEmpty() ? text(localization::Message::gui_evidence_empty)
                                               : evidence_);
}

void EvidencePanel::retranslate() {
    title_->setText(text(localization::Message::gui_evidence_title));
    close_->setToolTip(text(localization::Message::gui_action_close_evidence));
    close_->setAccessibleName(close_->toolTip());
    set_evidence(evidence_);
}

} // namespace mint::ui
