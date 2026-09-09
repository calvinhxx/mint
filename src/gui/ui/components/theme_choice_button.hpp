#pragma once

#include "application/preferences/gui_preferences.hpp"

#include <FluentQt/Foundation.h>

#include <QAbstractButton>

namespace mint::ui {

class ThemeChoiceButton final : public QAbstractButton, public fluent::FluentElement {
  public:
    explicit ThemeChoiceButton(desktop::ThemeMode mode, QWidget* parent = nullptr);

    [[nodiscard]] desktop::ThemeMode mode() const noexcept;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void onThemeUpdated() override;

  private:
    desktop::ThemeMode mode_;
    bool hovered_ = false;
};

} // namespace mint::ui
