#pragma once

#include <FluentQt/TextFields.h>

#include <QPointer>

namespace mint::ui {

class UnderlineFreeLineEdit final : public fluent::textfields::LineEdit {
  public:
    explicit UnderlineFreeLineEdit(QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;
};

class UnderlineFreePasswordBox final : public fluent::textfields::PasswordBox {
  public:
    explicit UnderlineFreePasswordBox(QWidget* parent = nullptr);
    void set_trailing_action(QWidget* action);

  protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void update_trailing_action_geometry();

    QPointer<QWidget> trailing_action_;
};

class UnderlineFreeTextEdit final : public fluent::textfields::TextEdit {
  public:
    explicit UnderlineFreeTextEdit(QWidget* parent = nullptr);
    void set_surface_visible(bool visible);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    bool surface_visible_ = true;
};

} // namespace mint::ui
