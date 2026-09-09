#pragma once

#include <QWidget>

namespace mint::ui {

class TitleBarSurface final : public QWidget {
  public:
    explicit TitleBarSurface(QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;
};

} // namespace mint::ui
