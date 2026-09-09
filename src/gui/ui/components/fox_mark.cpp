#include "fox_mark.hpp"

#include "ui/theme/layout_metrics.hpp"

#include <QPainter>

namespace mint::ui {

FoxMark::FoxMark(QWidget* parent)
    : QWidget(parent), renderer_(QStringLiteral(":/mint/branding/mint-arctic-fox-mark.svg"), this) {
    setFixedSize(metrics::identity_mark_size, metrics::identity_mark_size);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

QSize FoxMark::sizeHint() const {
    return {metrics::identity_mark_size, metrics::identity_mark_size};
}

void FoxMark::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer_.render(&painter, QRectF(rect()).adjusted(1, 1, -1, -1));
}

} // namespace mint::ui
