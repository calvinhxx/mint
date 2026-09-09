#pragma once

#include <QStyledItemDelegate>

namespace mint::ui {

class RunTimelineDelegate final : public QStyledItemDelegate {
  public:
    explicit RunTimelineDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;

    [[nodiscard]] QRect evidence_action_rect(const QStyleOptionViewItem& option,
                                             const QModelIndex& index) const;

  private:
    [[nodiscard]] QRect content_rect(const QStyleOptionViewItem& option) const;
    [[nodiscard]] int result_height(const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const;
};

} // namespace mint::ui
