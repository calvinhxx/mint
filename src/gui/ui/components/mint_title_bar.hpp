#pragma once

#include <QWidget>

namespace fluent::basicinput {
class Button;
}
namespace fluent::textfields {
class Label;
}

namespace mint::ui {

class MintTitleBar final : public QWidget {
    Q_OBJECT

  public:
    explicit MintTitleBar(QWidget* parent = nullptr);

    void set_navigation_boundary(int x);
    void set_task(QString title, bool can_resume);
    void retranslate();

  signals:
    void settings_requested();
    void resume_requested();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void sync_regions();

    QWidget* identity_region_ = nullptr;
    fluent::textfields::Label* task_title_ = nullptr;
    fluent::textfields::Label* name_ = nullptr;
    fluent::basicinput::Button* resume_ = nullptr;
    fluent::basicinput::Button* settings_ = nullptr;
    QString task_title_text_;
    int navigation_boundary_ = 0;
    bool can_resume_ = false;
};

} // namespace mint::ui
