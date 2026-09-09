#pragma once

#include "application/desktop_types.hpp"

#include <FluentQt/Layout.h>

#include <QString>

#include <optional>

namespace fluent::basicinput {
class Button;
}
namespace fluent::textfields {
class Label;
}

namespace mint::ui {

class ApprovalPanel final : public fluent::layout::Card {
    Q_OBJECT

  public:
    explicit ApprovalPanel(QWidget* parent = nullptr);

    void set_request(const desktop::ApprovalRequest& request);
    void clear();
    void retranslate();
    [[nodiscard]] QString evidence_text() const;

  signals:
    void approved();
    void denied();
    void evidence_requested();

  private:
    fluent::textfields::Label* eyebrow_ = nullptr;
    fluent::textfields::Label* title_ = nullptr;
    fluent::textfields::Label* note_ = nullptr;
    fluent::textfields::Label* target_ = nullptr;
    fluent::basicinput::Button* evidence_button_ = nullptr;
    fluent::basicinput::Button* deny_button_ = nullptr;
    fluent::basicinput::Button* approve_button_ = nullptr;
    std::optional<desktop::ApprovalRequest> request_;
    QString evidence_;
};

} // namespace mint::ui
