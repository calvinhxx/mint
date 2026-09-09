#pragma once

#include <QWidget>

#include <string>

namespace fluent::basicinput {
class Button;
}
namespace mint::ui {

class TaskComposer final : public QWidget {
    Q_OBJECT

  public:
    explicit TaskComposer(QWidget* parent = nullptr);
    ~TaskComposer() override;

    void set_running(bool running);
    void set_run_available(bool available);
    void set_workspace_available(bool available);
    void set_model_unavailable_reason(QString reason);
    [[nodiscard]] QString draft() const;
    void set_draft(const QString& draft);
    void focus_input();
    void retranslate();

  signals:
    void submitted(std::string question);
    void cancel_requested();
    void height_changed(int height);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void update_action();
    void update_focus_state(QWidget* focused);

    class UnderlineFreeTextEdit* input_ = nullptr;
    fluent::basicinput::Button* action_ = nullptr;
    bool running_ = false;
    bool run_available_ = true;
    bool workspace_available_ = true;
    QString model_unavailable_reason_;
};

} // namespace mint::ui
