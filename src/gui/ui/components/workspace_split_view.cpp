#include "workspace_split_view.hpp"

#include "ui/theme/layout_metrics.hpp"
#include "ui/theme/mint_theme.hpp"

#include <QCursor>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>

namespace mint::ui {

WorkspaceSplitView::WorkspaceSplitView(QWidget* parent) : fluent::collections::SplitView(parent) {
    setObjectName(QStringLiteral("MintWorkspaceSplitView"));
    setHandleWidth(metrics::workspace_split_handle_width);
    setAutoFillBackground(false);
    connect(this, &fluent::collections::SplitView::resizingChanged, this,
            [this](bool) { sync_pointer(mapFromGlobal(QCursor::pos())); });
    connect(this, &fluent::collections::SplitView::paneSizeChanged, this,
            [this](int, int) { sync_pointer(mapFromGlobal(QCursor::pos())); });
}

bool WorkspaceSplitView::eventFilter(QObject* watched, QEvent* event) {
    const auto handled = fluent::collections::SplitView::eventFilter(watched, event);
    // EN: Pane roots already carry SplitView's filter. Entering a child does not leave the
    // parent widget, so its cursor must be refreshed here rather than inherited by the pane.
    // ZH-CN: 面板根控件已安装 SplitView 的过滤器。进入子控件不会离开父控件，须在此恢复光标。
    if (event->type() == QEvent::Enter) {
        const auto* enter = static_cast<QEnterEvent*>(event);
        sync_pointer(mapFromGlobal(enter->globalPosition().toPoint()));
    } else if (event->type() == QEvent::Leave || event->type() == QEvent::Hide) {
        sync_pointer(mapFromGlobal(QCursor::pos()));
    }
    return handled;
}

void WorkspaceSplitView::enterEvent(QEnterEvent* event) {
    fluent::collections::SplitView::enterEvent(event);
    sync_pointer(event->position().toPoint());
}

void WorkspaceSplitView::leaveEvent(QEvent* event) {
    fluent::collections::SplitView::leaveEvent(event);
    if (!resizing()) {
        sync_pointer(QPoint(-1, -1));
    }
}

void WorkspaceSplitView::mouseMoveEvent(QMouseEvent* event) {
    fluent::collections::SplitView::mouseMoveEvent(event);
    sync_pointer(event->position().toPoint());
}

void WorkspaceSplitView::mouseReleaseEvent(QMouseEvent* event) {
    fluent::collections::SplitView::mouseReleaseEvent(event);
    sync_pointer(event->position().toPoint());
}

void WorkspaceSplitView::resizeEvent(QResizeEvent* event) {
    fluent::collections::SplitView::resizeEvent(event);
    sync_pointer(mapFromGlobal(QCursor::pos()));
}

void WorkspaceSplitView::changeEvent(QEvent* event) {
    fluent::collections::SplitView::changeEvent(event);
    if (event->type() == QEvent::EnabledChange) {
        sync_pointer(mapFromGlobal(QCursor::pos()));
    }
}

void WorkspaceSplitView::sync_pointer(const QPoint& position) {
    auto hovered = -1;
    if (isEnabled()) {
        for (int index = 0; index < handleCount(); ++index) {
            if (handleGeometry(index).contains(position)) {
                hovered = index;
                break;
            }
        }
    }
    if (hovered >= 0 || resizing()) {
        setCursor(orientation() == Qt::Horizontal ? Qt::SplitHCursor : Qt::SplitVCursor);
    } else {
        unsetCursor();
    }
    if (hovered_handle_ != hovered) {
        hovered_handle_ = hovered;
        update();
    }
}

void WorkspaceSplitView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    for (int index = 0; index < handleCount(); ++index) {
        const auto handle = handleGeometry(index);
        painter.fillRect(handle, theme::palette().canvas);
        if (index == hovered_handle_ || resizing()) {
            const auto line = orientation() == Qt::Horizontal
                                  ? QRect(handle.center().x(), handle.top(), 1, handle.height())
                                  : QRect(handle.left(), handle.center().y(), handle.width(), 1);
            painter.fillRect(line, theme::palette().stroke_strong);
        }
    }
}

} // namespace mint::ui
