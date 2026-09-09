#include "title_bar_surface.hpp"

#include "ui/theme/layout_metrics.hpp"
#include "ui/theme/mint_theme.hpp"

#include <QPainter>

namespace mint::ui {

TitleBarSurface::TitleBarSurface(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MintTitleBarSurface"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
}

void TitleBarSurface::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), theme::palette().canvas);
    painter.fillRect(0, height() - metrics::divider_thickness, width(), metrics::divider_thickness,
                     theme::palette().stroke);
}

} // namespace mint::ui
