#pragma once

#include <FluentQt/Collections.h>

namespace mint::ui {

class WorkspaceSplitView final : public fluent::collections::SplitView {
  public:
    explicit WorkspaceSplitView(QWidget* parent = nullptr);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    void sync_pointer(const QPoint& position);
    int hovered_handle_ = -1;
};

} // namespace mint::ui
