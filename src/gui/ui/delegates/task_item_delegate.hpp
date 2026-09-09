#pragma once

#include <QPersistentModelIndex>
#include <QRect>
#include <QStyledItemDelegate>

class QVariantAnimation;

namespace mint::ui {

class TaskItemDelegate final : public QStyledItemDelegate {
  public:
    explicit TaskItemDelegate(QObject* parent = nullptr);

    void set_action_row(int row) noexcept;
    void set_running_row(int row) noexcept;
    void set_hovered_index(const QModelIndex& index);
    void animate_disclosure(const QModelIndex& index, bool was_expanded);
    [[nodiscard]] static QRect trailing_action_rect(const QRect& item_rect);
    [[nodiscard]] static QRect workspace_icon_rect(const QRect& item_rect);
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;

  private:
    int action_row_ = -1;
    int running_row_ = -1;
    QPersistentModelIndex hovered_index_;
    QPersistentModelIndex previous_hovered_index_;
    QPersistentModelIndex disclosure_index_;
    QVariantAnimation* hover_animation_ = nullptr;
    QVariantAnimation* disclosure_animation_ = nullptr;
    qreal hover_progress_ = 1.0;
    qreal disclosure_angle_ = 0.0;
    void update_index(const QPersistentModelIndex& index);
};

} // namespace mint::ui
