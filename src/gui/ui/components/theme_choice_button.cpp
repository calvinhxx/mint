#include "theme_choice_button.hpp"

#include "ui/theme/layout_metrics.hpp"
#include "ui/theme/mint_theme.hpp"

#include <FluentQt/Design.h>

#include <QEnterEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>

namespace mint::ui {
namespace {

void paint_preview(QPainter& painter, const QRectF& bounds, theme::Scheme scheme,
                   const QRectF& clip) {
    const auto& colors = theme::palette(scheme);
    painter.save();
    painter.setClipRect(clip);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.chrome);
    painter.drawRoundedRect(bounds, metrics::theme_preview_radius, metrics::theme_preview_radius);

    const auto header_width = bounds.width() * 0.34;
    const QRectF header(bounds.center().x() - header_width / 2.0, bounds.top() + 10, header_width,
                        4);
    painter.setBrush(colors.stroke_strong);
    painter.drawRoundedRect(header, 2, 2);

    const QRectF card(bounds.left() + 10, bounds.top() + 22, bounds.width() - 20,
                      bounds.height() - 28);
    painter.setBrush(colors.panel);
    painter.drawRoundedRect(card, 6, 6);

    const QRectF sidebar(card.left(), card.top(), card.width() * 0.28, card.height());
    painter.setBrush(colors.selected);
    painter.drawRoundedRect(sidebar, 6, 6);

    painter.setBrush(colors.stroke);
    const auto line_left = sidebar.right() + 8;
    const auto line_width = card.right() - line_left - 7;
    for (int row = 0; row < 3; ++row) {
        const auto y = card.top() + 8 + row * 9;
        painter.drawRoundedRect(QRectF(line_left, y, line_width * (row == 1 ? 0.7 : 1.0), 3), 1.5,
                                1.5);
    }
    painter.restore();
}

} // namespace

ThemeChoiceButton::ThemeChoiceButton(desktop::ThemeMode mode, QWidget* parent)
    : QAbstractButton(parent), mode_(mode) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(metrics::theme_choice_height);
}

desktop::ThemeMode ThemeChoiceButton::mode() const noexcept {
    return mode_;
}

QSize ThemeChoiceButton::sizeHint() const {
    return {metrics::theme_choice_preferred_width, metrics::theme_choice_height};
}

QSize ThemeChoiceButton::minimumSizeHint() const {
    return {metrics::theme_choice_minimum_width, metrics::theme_choice_height};
}

void ThemeChoiceButton::enterEvent(QEnterEvent* event) {
    hovered_ = true;
    update();
    QAbstractButton::enterEvent(event);
}

void ThemeChoiceButton::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void ThemeChoiceButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto& colors = theme::palette();

    const QRectF preview(metrics::theme_choice_frame_inset, metrics::theme_choice_frame_inset,
                         width() - metrics::theme_choice_frame_inset * 2,
                         metrics::theme_preview_height);
    if (mode_ == desktop::ThemeMode::system) {
        const QRectF left_clip(preview.left(), preview.top(), preview.width() / 2.0,
                               preview.height());
        const QRectF right_clip(preview.center().x(), preview.top(), preview.width() / 2.0,
                                preview.height());
        paint_preview(painter, preview, theme::Scheme::light, left_clip);
        paint_preview(painter, preview, theme::Scheme::dark, right_clip);
    } else {
        paint_preview(painter, preview,
                      mode_ == desktop::ThemeMode::light ? theme::Scheme::light
                                                         : theme::Scheme::dark,
                      preview);
    }

    const auto border_color =
        isChecked() ? colors.action : (hovered_ ? colors.stroke_strong : colors.stroke);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(border_color, isChecked() ? 2.0 : 1.0));
    painter.drawRoundedRect(preview, metrics::theme_preview_radius, metrics::theme_preview_radius);

    if (hasFocus()) {
        painter.setPen(QPen(colors.action, 1.0, Qt::DashLine));
        painter.drawRoundedRect(preview.adjusted(3, 3, -3, -3), metrics::theme_preview_radius - 2,
                                metrics::theme_preview_radius - 2);
    }

    painter.setPen(isEnabled() ? colors.text_primary : colors.text_disabled);
    painter.setFont(Typography::fontStyle(Typography::FontRole::Caption).toQFont());
    const QRectF label(0, preview.bottom() + metrics::space_sm, width(),
                       height() - preview.bottom() - metrics::space_sm);
    painter.drawText(label, Qt::AlignHCenter | Qt::AlignTop, text());
}

void ThemeChoiceButton::onThemeUpdated() {
    update();
}

} // namespace mint::ui
