#include "underline_free_input.hpp"

#include "ui/theme/layout_metrics.hpp"

#include <QApplication>
#include <QEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>

namespace mint::ui {
namespace {

template <typename Input> bool contains_focus(const Input& input) {
    const auto* focused = QApplication::focusWidget();
    return focused == &input || (focused != nullptr && input.isAncestorOf(focused));
}

template <typename Input> void paint_uniform_frame(Input& input) {
    const auto& colors = input.themeColorsRef();
    QColor background;
    QColor border;
    const auto focused = contains_focus(input);
    if (!input.isEnabled()) {
        background = colors.controlDisabled;
        border = colors.strokeDivider;
    } else if (input.isReadOnly()) {
        background = colors.controlAltSecondary;
        border = colors.strokeDefault;
    } else if (focused) {
        const auto theme = input.effectiveTheme();
        background = theme == fluent::FluentElement::Dark ? colors.bgSolid : colors.controlDefault;
        border = colors.strokeFocusOuter;
    } else if (input.underMouse()) {
        background = colors.controlSecondary;
        border = colors.strokeSecondary;
    } else {
        background = colors.controlDefault;
        border = colors.strokeDefault;
    }

    QPainter painter(&input);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto scale = input.devicePixelRatioF();
    const auto stroke = std::max(1.0, std::round(scale)) / scale;
    const auto inset = stroke / 2.0;
    const auto frame = QRectF(input.rect()).adjusted(inset, inset, -inset, -inset);
    QPainterPath path;
    const auto radius = input.themeRadius().control;
    path.addRoundedRect(frame, radius, radius);

    painter.setBrush(background);
    painter.setPen(QPen(border, stroke));
    painter.drawPath(path);
}

} // namespace

UnderlineFreeLineEdit::UnderlineFreeLineEdit(QWidget* parent)
    : fluent::textfields::LineEdit(parent) {
    setFrameVisible(false);
    setFocusedBorderWidth(0);
    setUnfocusedBorderWidth(0);
}

void UnderlineFreeLineEdit::paintEvent(QPaintEvent* event) {
    paint_uniform_frame(*this);
    fluent::textfields::LineEdit::paintEvent(event);
}

UnderlineFreePasswordBox::UnderlineFreePasswordBox(QWidget* parent)
    : fluent::textfields::PasswordBox(parent) {
    setFocusedBorderWidth(0);
    setUnfocusedBorderWidth(0);
    connect(this, &QLineEdit::textChanged, this, [this] { update_trailing_action_geometry(); });
}

void UnderlineFreePasswordBox::set_trailing_action(QWidget* action) {
    trailing_action_ = action;
    if (trailing_action_ != nullptr) {
        trailing_action_->setParent(this);
        trailing_action_->raise();
    }
    update_trailing_action_geometry();
}

void UnderlineFreePasswordBox::changeEvent(QEvent* event) {
    fluent::textfields::PasswordBox::changeEvent(event);
    update_trailing_action_geometry();
}

void UnderlineFreePasswordBox::paintEvent(QPaintEvent* event) {
    // EN: Mint uses a headerless, permanently masked field; its adapter owns the frame.
    // ZH-CN: Mint 使用无标题、始终掩码的输入框，外框由此适配器统一绘制。
    paint_uniform_frame(*this);
    QLineEdit::paintEvent(event);
}

void UnderlineFreePasswordBox::resizeEvent(QResizeEvent* event) {
    fluent::textfields::PasswordBox::resizeEvent(event);
    update_trailing_action_geometry();
}

void UnderlineFreePasswordBox::update_trailing_action_geometry() {
    if (trailing_action_ == nullptr) {
        return;
    }
    const auto x = width() - trailing_action_->width() - metrics::space_xs;
    const auto y = (height() - trailing_action_->height()) / 2;
    trailing_action_->move(x, y);
    trailing_action_->raise();

    auto margins = contentMargins();
    margins.setRight(trailing_action_->width() + metrics::space_xs + metrics::space_sm);
    setContentMargins(margins);
}

UnderlineFreeTextEdit::UnderlineFreeTextEdit(QWidget* parent)
    : fluent::textfields::TextEdit(parent) {
    setFocusedBorderWidth(0);
    setUnfocusedBorderWidth(0);
}

void UnderlineFreeTextEdit::set_surface_visible(bool visible) {
    if (surface_visible_ == visible) {
        return;
    }
    surface_visible_ = visible;
    update();
}

void UnderlineFreeTextEdit::paintEvent(QPaintEvent* event) {
    if (!surface_visible_) {
        QWidget::paintEvent(event);
        return;
    }
    paint_uniform_frame(*this);
}

} // namespace mint::ui
