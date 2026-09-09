#pragma once

#include <QString>
#include <QWidget>

namespace fluent::basicinput {
class Button;
}
namespace fluent::textfields {
class Label;
class TextEdit;
} // namespace fluent::textfields

namespace mint::ui {

class EvidencePanel final : public QWidget {
    Q_OBJECT

  public:
    explicit EvidencePanel(QWidget* parent = nullptr);

    void set_evidence(const QString& content);
    void retranslate();

  signals:
    void close_requested();

  private:
    fluent::textfields::Label* title_ = nullptr;
    fluent::basicinput::Button* close_ = nullptr;
    fluent::textfields::TextEdit* content_ = nullptr;
    QString evidence_;
};

} // namespace mint::ui
