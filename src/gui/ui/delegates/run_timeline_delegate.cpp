#include "run_timeline_delegate.hpp"

#include "ui/models/run_timeline_model.hpp"
#include "ui/presentation/run_event_presentation.hpp"
#include "ui/theme/layout_metrics.hpp"
#include "ui/theme/mint_theme.hpp"

#include <FluentQt/Design.h>

#include <QAbstractTextDocumentLayout>
#include <QFontMetrics>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QTextDocument>

#include <cmath>
#include <memory>

namespace mint::ui {
namespace {

RunTimelineModel::ItemKind item_kind(const QModelIndex& index) {
    return static_cast<RunTimelineModel::ItemKind>(
        index.data(RunTimelineModel::ItemKindRole).toInt());
}

QColor tone_color(RunEventTone tone) {
    const auto& palette = theme::palette();
    switch (tone) {
    case RunEventTone::attention:
        return palette.attention;
    case RunEventTone::success:
        return palette.success;
    case RunEventTone::critical:
        return palette.critical;
    case RunEventTone::neutral:
        return palette.text_tertiary;
    }
    return palette.text_tertiary;
}

std::unique_ptr<QTextDocument> result_document(const QModelIndex& index, int width) {
    auto document = std::make_unique<QTextDocument>();
    document->setDocumentMargin(0);
    document->setDefaultFont(Typography::fontStyle(Typography::FontRole::Body).toQFont());
    document->setDefaultStyleSheet(theme::markdown_stylesheet());
    const auto features = QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) |
                          QTextDocument::MarkdownNoHTML;
    document->setMarkdown(index.data(RunTimelineModel::BodyRole).toString(), features);
    document->setTextWidth(qMax(1, width));
    return document;
}

int option_width(const QStyleOptionViewItem& option) {
    if (option.rect.width() > 0) {
        return option.rect.width();
    }
    return option.widget == nullptr ? metrics::conversation_maximum_width : option.widget->width();
}

} // namespace

RunTimelineDelegate::RunTimelineDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void RunTimelineDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const {
    if (painter == nullptr || !index.isValid()) {
        return;
    }

    const auto& palette = theme::palette();
    const auto bounds = content_rect(option);
    painter->save();
    painter->setClipRect(option.rect);
    painter->setRenderHint(QPainter::Antialiasing);

    if (item_kind(index) == RunTimelineModel::ItemKind::spacer) {
        painter->restore();
        return;
    }

    if (item_kind(index) == RunTimelineModel::ItemKind::empty) {
        painter->setFont(Typography::fontStyle(Typography::FontRole::Body).toQFont());
        painter->setPen(palette.text_tertiary);
        painter->drawText(bounds, Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(RunTimelineModel::TitleRole).toString());
        painter->restore();
        return;
    }

    if (item_kind(index) == RunTimelineModel::ItemKind::event) {
        const auto detail = index.data(RunTimelineModel::DetailRole).toString();
        const auto action = index.data(RunTimelineModel::EvidenceActionRole).toString();
        const auto action_rect = evidence_action_rect(option, index);
        const auto center_y = bounds.center().y();

        painter->setPen(Qt::NoPen);
        painter->setBrush(
            tone_color(static_cast<RunEventTone>(index.data(RunTimelineModel::ToneRole).toInt())));
        painter->drawEllipse(
            QPoint(bounds.left() + metrics::timeline_event_dot_diameter / 2, center_y),
            metrics::timeline_event_dot_diameter / 2, metrics::timeline_event_dot_diameter / 2);

        const auto text_right =
            action.isEmpty() ? bounds.right() : action_rect.left() - metrics::space_lg;
        const QRect text_bounds(
            bounds.left() + metrics::timeline_event_text_offset, bounds.top(),
            qMax(0, text_right - bounds.left() - metrics::timeline_event_text_offset),
            bounds.height());
        auto title_font = Typography::fontStyle(Typography::FontRole::BodyStrong).toQFont();
        painter->setFont(title_font);
        painter->setPen(palette.text_primary);
        const auto title =
            painter->fontMetrics().elidedText(index.data(RunTimelineModel::TitleRole).toString(),
                                              Qt::ElideRight, text_bounds.width());
        const auto title_height = painter->fontMetrics().height();
        const auto title_y = detail.isEmpty() ? text_bounds.center().y() - title_height / 2
                                              : text_bounds.top() + metrics::space_md;
        painter->drawText(QRect(text_bounds.left(), title_y, text_bounds.width(), title_height),
                          Qt::AlignLeft | Qt::AlignVCenter, title);

        if (!detail.isEmpty()) {
            painter->setFont(Typography::fontStyle(Typography::FontRole::Caption).toQFont());
            painter->setPen(palette.text_secondary);
            const auto summary =
                painter->fontMetrics().elidedText(detail, Qt::ElideMiddle, text_bounds.width());
            painter->drawText(QRect(text_bounds.left(), title_y + title_height + metrics::space_xs,
                                    text_bounds.width(), painter->fontMetrics().height()),
                              Qt::AlignLeft | Qt::AlignVCenter, summary);
        }

        if (!action.isEmpty()) {
            auto action_font = Typography::fontStyle(Typography::FontRole::Body).toQFont();
            action_font.setUnderline(true);
            painter->setFont(action_font);
            painter->setPen(palette.text_secondary);
            painter->drawText(action_rect, Qt::AlignRight | Qt::AlignVCenter, action);
        }
        painter->restore();
        return;
    }

    const auto card = bounds.adjusted(0, metrics::timeline_result_outer_spacing, 0,
                                      -metrics::timeline_result_outer_spacing);
    painter->setPen(QPen(palette.stroke, 1));
    painter->setBrush(palette.panel);
    painter->drawRoundedRect(card, metrics::timeline_result_radius,
                             metrics::timeline_result_radius);

    auto title_font = Typography::fontStyle(Typography::FontRole::Subtitle).toQFont();
    painter->setFont(title_font);
    painter->setPen(palette.text_primary);
    const auto title_height = painter->fontMetrics().height();
    const QRect title_rect(card.left() + metrics::timeline_result_padding,
                           card.top() + metrics::timeline_result_padding,
                           card.width() - metrics::timeline_result_padding * 2, title_height);
    painter->drawText(title_rect, Qt::AlignLeft | Qt::AlignVCenter,
                      index.data(RunTimelineModel::TitleRole).toString());

    const auto document_width = card.width() - metrics::timeline_result_padding * 2;
    auto document = result_document(index, document_width);
    const auto document_y = title_rect.bottom() + metrics::timeline_result_section_spacing;
    painter->save();
    painter->translate(card.left() + metrics::timeline_result_padding, document_y);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, palette.text_primary);
    document->documentLayout()->draw(painter, context);
    painter->restore();

    const auto footer_height =
        QFontMetrics(Typography::fontStyle(Typography::FontRole::Caption).toQFont()).height();
    const auto footer_y = card.bottom() - metrics::timeline_result_padding - footer_height;
    painter->setFont(Typography::fontStyle(Typography::FontRole::Caption).toQFont());
    painter->setPen(palette.text_tertiary);
    painter->drawText(QRect(card.left() + metrics::timeline_result_padding, footer_y,
                            card.width() - metrics::timeline_result_padding * 2, footer_height),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      index.data(RunTimelineModel::UsageRole).toString());

    const auto action = index.data(RunTimelineModel::EvidenceActionRole).toString();
    if (!action.isEmpty()) {
        auto action_font = Typography::fontStyle(Typography::FontRole::Caption).toQFont();
        action_font.setUnderline(true);
        painter->setFont(action_font);
        painter->setPen(palette.text_secondary);
        painter->drawText(evidence_action_rect(option, index), Qt::AlignRight | Qt::AlignVCenter,
                          action);
    }
    painter->restore();
}

QSize RunTimelineDelegate::sizeHint(const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const {
    switch (item_kind(index)) {
    case RunTimelineModel::ItemKind::empty:
        return {option_width(option), metrics::timeline_empty_row_height};
    case RunTimelineModel::ItemKind::event:
        return {option_width(option), index.data(RunTimelineModel::DetailRole).toString().isEmpty()
                                          ? metrics::timeline_event_row_height
                                          : metrics::timeline_event_detail_row_height};
    case RunTimelineModel::ItemKind::result:
        return {option_width(option), result_height(option, index)};
    case RunTimelineModel::ItemKind::spacer:
        return {option_width(option),
                qMax(0, index.data(RunTimelineModel::BottomInsetRole).toInt())};
    }
    return {option_width(option), metrics::timeline_event_row_height};
}

QRect RunTimelineDelegate::evidence_action_rect(const QStyleOptionViewItem& option,
                                                const QModelIndex& index) const {
    const auto action = index.data(RunTimelineModel::EvidenceActionRole).toString();
    if (action.isEmpty()) {
        return {};
    }
    const auto bounds = content_rect(option);
    const auto role = item_kind(index) == RunTimelineModel::ItemKind::result
                          ? Typography::FontRole::Caption
                          : Typography::FontRole::Body;
    const QFontMetrics font_metrics(Typography::fontStyle(role).toQFont());
    const auto width = font_metrics.horizontalAdvance(action) + metrics::space_sm;
    if (item_kind(index) == RunTimelineModel::ItemKind::result) {
        const auto card = bounds.adjusted(0, metrics::timeline_result_outer_spacing, 0,
                                          -metrics::timeline_result_outer_spacing);
        const auto height = font_metrics.height();
        return {card.right() - metrics::timeline_result_padding - width,
                card.bottom() - metrics::timeline_result_padding - height, width, height};
    }
    return {bounds.right() - width, bounds.top(), width, bounds.height()};
}

QRect RunTimelineDelegate::content_rect(const QStyleOptionViewItem& option) const {
    const auto available_width = option_width(option);
    const auto width =
        qMin(metrics::conversation_maximum_width,
             qMax(0, available_width - metrics::conversation_horizontal_padding * 2));
    const auto left = option.rect.left() + (available_width - width) / 2;
    return {left, option.rect.top(), width, option.rect.height()};
}

int RunTimelineDelegate::result_height(const QStyleOptionViewItem& option,
                                       const QModelIndex& index) const {
    const auto width =
        qMin(metrics::conversation_maximum_width,
             qMax(0, option_width(option) - metrics::conversation_horizontal_padding * 2));
    const auto document_width = qMax(1, width - metrics::timeline_result_padding * 2);
    auto document = result_document(index, document_width);
    const auto title_height =
        QFontMetrics(Typography::fontStyle(Typography::FontRole::Subtitle).toQFont()).height();
    const auto footer_height =
        QFontMetrics(Typography::fontStyle(Typography::FontRole::Caption).toQFont()).height();
    return metrics::timeline_result_outer_spacing * 2 + metrics::timeline_result_padding * 2 +
           title_height + metrics::timeline_result_section_spacing * 2 +
           static_cast<int>(std::ceil(document->size().height())) + footer_height;
}

} // namespace mint::ui
