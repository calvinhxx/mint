#pragma once

#include <QSvgRenderer>
#include <QWidget>

namespace mint::ui {

class FoxMark final : public QWidget {
  public:
    explicit FoxMark(QWidget* parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QSvgRenderer renderer_;
};

} // namespace mint::ui
