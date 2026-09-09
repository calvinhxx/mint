#include "task_item_delegate.hpp"

#include "ui/models/task_list_model.hpp"
#include "ui/theme/layout_metrics.hpp"
#include "ui/theme/mint_theme.hpp"

#include <FluentQt/Design.h>
#include <FluentQt/Foundation.h>

#include <QAbstractItemView>
#include <QPainter>
#include <QVariantAnimation>

#include <algorithm>

namespace mint::ui {
namespace {

constexpr int row_height = 36;
constexpr int row_horizontal_inset = 2;
constexpr int row_vertical_inset = 1;
constexpr int row_radius = 8;
constexpr int disclosure_slot_width = 18;
constexpr int workspace_icon_slot_width = 28;
constexpr int task_indent = 46;
constexpr int trailing_slot_width = 32;
constexpr int trailing_action_size = 28;
constexpr int trailing_icon_size = 12;
constexpr int workspace_icon_size = 16;
constexpr int disclosure_icon_size = 9;

int option_width(const QStyleOptionViewItem& option) {
    if (option.rect.width() > 0) {
        return option.rect.width();
    }
    return option.widget == nullptr ? metrics::task_sidebar_preferred_width
                                    : option.widget->width();
}

} // namespace

TaskItemDelegate::TaskItemDelegate(QObject* parent) : QStyledItemDelegate(parent) {
    hover_animation_ = new QVariantAnimation(this);
    hover_animation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(hover_animation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                hover_progress_ = value.toReal();
                update_index(hovered_index_);
                update_index(previous_hovered_index_);
            });
    disclosure_animation_ = new QVariantAnimation(this);
    disclosure_animation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(disclosure_animation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                disclosure_angle_ = value.toReal();
                update_index(disclosure_index_);
            });
    connect(&fluent::MotionPolicy::instance(), &fluent::MotionPolicy::modeChanged, this,
            [this](fluent::MotionPolicy::Mode) {
                hover_animation_->stop();
                disclosure_animation_->stop();
                hover_progress_ = 1.0;
                update_index(hovered_index_);
                update_index(previous_hovered_index_);
                update_index(disclosure_index_);
                previous_hovered_index_ = QPersistentModelIndex{};
                disclosure_index_ = QPersistentModelIndex{};
            });
}

void TaskItemDelegate::update_index(const QPersistentModelIndex& index) {
    if (auto* view = qobject_cast<QAbstractItemView*>(parent());
        view != nullptr && index.isValid()) {
        view->viewport()->update(view->visualRect(index));
    }
}

void TaskItemDelegate::set_hovered_index(const QModelIndex& index) {
    if (hovered_index_ == index) {
        return;
    }
    hover_animation_->stop();
    update_index(previous_hovered_index_);
    previous_hovered_index_ = hovered_index_;
    hovered_index_ = index;
    const auto duration = fluent::MotionPolicy::instance().resolvedDuration(100);
    hover_progress_ = duration == 0 ? 1.0 : 0.0;
    update_index(hovered_index_);
    update_index(previous_hovered_index_);
    if (duration > 0) {
        hover_animation_->setDuration(duration);
        hover_animation_->setStartValue(0.0);
        hover_animation_->setEndValue(1.0);
        hover_animation_->start();
    }
}

void TaskItemDelegate::animate_disclosure(const QModelIndex& index, bool was_expanded) {
    disclosure_animation_->stop();
    update_index(disclosure_index_);
    disclosure_index_ = index;
    const auto expanded = index.data(TaskListModel::ExpandedWorkspaceRole).toBool();
    const auto duration = fluent::MotionPolicy::instance().resolvedDuration(140);
    if (duration == 0 || expanded == was_expanded) {
        disclosure_index_ = QPersistentModelIndex{};
        return;
    }
    disclosure_animation_->setDuration(duration);
    disclosure_animation_->setStartValue(was_expanded ? 90.0 : 0.0);
    disclosure_animation_->setEndValue(expanded ? 90.0 : 0.0);
    disclosure_animation_->start();
}

void TaskItemDelegate::set_action_row(int row) noexcept {
    action_row_ = row;
}

void TaskItemDelegate::set_running_row(int row) noexcept {
    running_row_ = row;
}

QRect TaskItemDelegate::trailing_action_rect(const QRect& item_rect) {
    const auto bounds = item_rect.adjusted(row_horizontal_inset, row_vertical_inset,
                                           -row_horizontal_inset, -row_vertical_inset);
    return {bounds.right() - trailing_action_size + 1,
            bounds.center().y() - trailing_action_size / 2, trailing_action_size,
            trailing_action_size};
}

QRect TaskItemDelegate::workspace_icon_rect(const QRect& item_rect) {
    const auto bounds = item_rect.adjusted(row_horizontal_inset, row_vertical_inset,
                                           -row_horizontal_inset, -row_vertical_inset);
    return {bounds.left() + disclosure_slot_width, bounds.top(), workspace_icon_slot_width,
            bounds.height()};
}

void TaskItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const {
    const auto& p = theme::palette();
    painter->save();
    painter->setClipRect(option.rect);
    painter->setRenderHint(QPainter::Antialiasing);
    const auto bounds = option.rect.adjusted(row_horizontal_inset, row_vertical_inset,
                                             -row_horizontal_inset, -row_vertical_inset);
    const auto hover_opacity = index == hovered_index_            ? hover_progress_
                               : index == previous_hovered_index_ ? 1.0 - hover_progress_
                                                                  : 0.0;
    const auto focused = option.state.testFlag(QStyle::State_HasFocus);
    auto hover_color = p.subtle_hover;
    hover_color.setAlphaF(hover_color.alphaF() * hover_opacity);
    const auto paint_focus = [&] {
        if (focused) {
            painter->setPen(QPen(p.stroke_strong, 1.0));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(bounds).adjusted(0.5, 0.5, -0.5, -0.5), row_radius,
                                     row_radius);
        }
    };
    const auto kind =
        static_cast<TaskListModel::RowKind>(index.data(TaskListModel::RowKindRole).toInt());
    if (kind == TaskListModel::RowKind::workspace) {
        if (hover_opacity > 0) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(hover_color);
            painter->drawRoundedRect(bounds, row_radius, row_radius);
        }
        const auto active = index.data(TaskListModel::ActiveWorkspaceRole).toBool();
        const auto expanded = index.data(TaskListModel::ExpandedWorkspaceRole).toBool();
        const auto expandable = index.data(TaskListModel::WorkspaceExpandableRole).toBool();
        painter->setPen(active ? p.text_primary : p.text_secondary);
        const QRect disclosure_slot(bounds.left() + 2, bounds.top(), disclosure_slot_width,
                                    bounds.height());
        if (expandable) {
            painter->save();
            const auto center = QRectF(disclosure_slot).center();
            painter->translate(center);
            painter->rotate(index == disclosure_index_ &&
                                    disclosure_animation_->state() == QAbstractAnimation::Running
                                ? disclosure_angle_
                            : expanded ? 90.0
                                       : 0.0);
            painter->translate(-center);
            Typography::Icons::paintGlyph(*painter, QRectF(disclosure_slot),
                                          Typography::Icons::ChevronRightMed, disclosure_icon_size,
                                          Qt::AlignCenter);
            painter->restore();
        }
        const auto waiting = index.data(TaskListModel::AwaitingApprovalRole).toBool();
        if (waiting || !index.data(TaskListModel::LoadingRole).toBool()) {
            painter->setPen(waiting ? p.attention : active ? p.text_primary : p.text_secondary);
            Typography::Icons::paintGlyph(*painter, QRectF(workspace_icon_rect(option.rect)),
                                          waiting ? Typography::Icons::Clock
                                                  : Typography::Icons::Folder,
                                          workspace_icon_size, Qt::AlignCenter);
        }
        painter->setPen(active ? p.text_primary : p.text_secondary);
        painter->setFont(Typography::fontStyle(active ? Typography::FontRole::BodyStrong
                                                      : Typography::FontRole::Body)
                             .toQFont());
        const auto leading_width = disclosure_slot_width + workspace_icon_slot_width;
        const auto right_inset = trailing_slot_width;
        const auto available_name_width = std::max(0, bounds.width() - leading_width - right_inset);
        const auto name = painter->fontMetrics().elidedText(
            index.data(TaskListModel::WorkspaceNameRole).toString(), Qt::ElideRight,
            available_name_width);
        painter->drawText(bounds.adjusted(leading_width, 0, -right_inset, 0),
                          Qt::AlignLeft | Qt::AlignVCenter, name);
        paint_focus();
        painter->restore();
        return;
    }

    const auto selected = option.state.testFlag(QStyle::State_Selected);
    if (selected) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(p.navigation_selected);
        painter->drawRoundedRect(bounds, row_radius, row_radius);
    } else if (hover_opacity > 0) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(hover_color);
        painter->drawRoundedRect(bounds, row_radius, row_radius);
    }

    const auto status = index.data(TaskListModel::StatusRole).toString();
    const auto pinned = index.data(TaskListModel::PinnedRole).toBool();
    const auto loading =
        index.data(TaskListModel::LoadingRole).toBool() || index.row() == running_row_;
    const auto resumable = index.data(TaskListModel::ResumableRole).toBool();
    const auto archived = index.data(TaskListModel::ArchivedRole).toBool();
    const auto waiting = index.data(TaskListModel::AwaitingApprovalRole).toBool();
    const auto has_action = index.row() == action_row_ && !loading;
    // EN: Reserve the action slot at rest so hover cannot change text elision or row geometry.
    // ZH-CN: 常态也预留操作槽位，避免悬停时文字省略位置和行布局发生跳变。
    const auto right_inset = trailing_slot_width;

    painter->setFont(Typography::fontStyle(Typography::FontRole::Body).toQFont());
    painter->setPen(p.text_primary);
    const auto question = painter->fontMetrics().elidedText(
        index.data(TaskListModel::QuestionRole).toString(), Qt::ElideRight,
        bounds.width() - task_indent - right_inset);
    painter->drawText(bounds.adjusted(task_indent, 0, -right_inset, 0),
                      Qt::AlignLeft | Qt::AlignVCenter, question);

    if (waiting || (!loading && !has_action)) {
        auto glyph = QString{};
        auto color = p.text_tertiary;
        if (waiting) {
            glyph = Typography::Icons::Clock;
            color = p.attention;
        } else if (pinned) {
            glyph = Typography::Icons::PinFill;
            color = p.text_secondary;
        } else if (status == QStringLiteral("failed")) {
            glyph = Typography::Icons::ErrorBadge12;
            color = p.critical;
        } else if (resumable) {
            glyph = Typography::Icons::Clock;
        } else if (archived) {
            glyph = Typography::Icons::History;
        }
        if (!glyph.isEmpty()) {
            painter->setPen(color);
            Typography::Icons::paintGlyph(*painter, QRectF(trailing_action_rect(option.rect)),
                                          glyph, trailing_icon_size, Qt::AlignCenter);
        }
    }
    paint_focus();
    painter->restore();
}

QSize TaskItemDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const {
    return {option_width(option), row_height};
}

} // namespace mint::ui
