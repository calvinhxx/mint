#include "mint_title_bar.hpp"

#include "ui/components/fox_mark.hpp"
#include "ui/support/localized_text.hpp"
#include "ui/theme/layout_metrics.hpp"

#include <FluentQt/BasicInput.h>
#include <FluentQt/Design.h>
#include <FluentQt/TextFields.h>

#include <QHBoxLayout>
#include <QResizeEvent>

#include <utility>

namespace mint::ui {

MintTitleBar::MintTitleBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MintTitleBarContent"));
    setAutoFillBackground(false);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    identity_region_ = new QWidget(this);
    identity_region_->setObjectName(QStringLiteral("MintTitleBarIdentity"));
    identity_region_->setAutoFillBackground(false);
    auto* identity_layout = new QHBoxLayout(identity_region_);
    identity_layout->setContentsMargins(metrics::space_xs, 0, metrics::space_sm, 0);
    identity_layout->setSpacing(metrics::space_sm);

    auto* mark = new FoxMark(identity_region_);
    mark->setObjectName(QStringLiteral("MintAppMark"));
    identity_layout->addWidget(mark);

    name_ = new fluent::textfields::Label(identity_region_);
    name_->setObjectName(QStringLiteral("MintTitleBarAppName"));
    name_->setFluentTypography(Typography::FontRole::BodyStrong);
    name_->setAttribute(Qt::WA_TransparentForMouseEvents);
    identity_layout->addWidget(name_);
    identity_layout->addStretch(1);
    layout->addWidget(identity_region_);

    auto* task_region = new QWidget(this);
    task_region->setObjectName(QStringLiteral("MintTitleBarTaskRegion"));
    task_region->setAutoFillBackground(false);
    auto* task_layout = new QHBoxLayout(task_region);
    task_layout->setContentsMargins(metrics::space_lg, 0, metrics::space_sm, 0);
    task_layout->setSpacing(metrics::space_sm);

    task_title_ = new fluent::textfields::Label(task_region);
    task_title_->setObjectName(QStringLiteral("MintTitleBarTaskTitle"));
    task_title_->setFluentTypography(Typography::FontRole::BodyStrong);
    task_title_->setTextElideMode(Qt::ElideRight);
    task_title_->setAttribute(Qt::WA_TransparentForMouseEvents);
    task_layout->addWidget(task_title_, 1);

    resume_ = new fluent::basicinput::Button(task_region);
    resume_->setObjectName(QStringLiteral("MintTitleBarResumeButton"));
    resume_->setIconGlyph(Typography::Icons::Play);
    resume_->setFluentLayout(fluent::basicinput::Button::IconBefore);
    resume_->setFluentStyle(fluent::basicinput::Button::Accent);
    resume_->setFluentSize(fluent::basicinput::Button::Small);
    resume_->hide();
    task_layout->addWidget(resume_);

    settings_ = new fluent::basicinput::Button(task_region);
    settings_->setObjectName(QStringLiteral("MintSettingsButton"));
    settings_->setIconGlyph(Typography::Icons::Settings);
    settings_->setFluentLayout(fluent::basicinput::Button::IconOnly);
    settings_->setFluentStyle(fluent::basicinput::Button::Subtle);
    settings_->setFixedSize(metrics::title_bar_action_size, metrics::title_bar_action_size);
    task_layout->addWidget(settings_);
    layout->addWidget(task_region, 1);

    connect(settings_, &QPushButton::clicked, this, &MintTitleBar::settings_requested);
    connect(resume_, &QPushButton::clicked, this, &MintTitleBar::resume_requested);
    retranslate();
}

void MintTitleBar::set_navigation_boundary(int x) {
    navigation_boundary_ = qMax(0, x);
    sync_regions();
}

void MintTitleBar::set_task(QString title, bool can_resume) {
    task_title_text_ = std::move(title);
    can_resume_ = can_resume;
    task_title_->setText(task_title_text_.isEmpty() ? text(localization::Message::gui_task_none)
                                                    : task_title_text_);
    resume_->setVisible(can_resume_);
    resume_->setEnabled(can_resume_);
}

void MintTitleBar::retranslate() {
    name_->setText(text(localization::Message::gui_app_name));
    task_title_->setText(task_title_text_.isEmpty() ? text(localization::Message::gui_task_none)
                                                    : task_title_text_);
    resume_->setText(text(localization::Message::gui_action_resume_task));
    settings_->setToolTip(text(localization::Message::gui_action_open_settings));
    settings_->setAccessibleName(settings_->toolTip());
}

void MintTitleBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    sync_regions();
}

void MintTitleBar::sync_regions() {
    if (identity_region_ == nullptr || parentWidget() == nullptr) {
        return;
    }
    const auto local_boundary = mapFrom(parentWidget(), QPoint(navigation_boundary_, 0)).x();
    identity_region_->setFixedWidth(qBound(0, local_boundary, width()));
}

} // namespace mint::ui
