#include "run_timeline_model.hpp"

#include "ui/presentation/run_event_presentation.hpp"
#include "ui/support/localized_text.hpp"

#include "mint/localization/localization.hpp"

#include <algorithm>

namespace mint::ui {
namespace {

using localization::arg;
using localization::Message;
using localization::Placeholder;

bool is_inspectable(desktop::RunEventKind kind) {
    using desktop::RunEventKind;
    return kind == RunEventKind::tool_completed || kind == RunEventKind::approval_requested ||
           kind == RunEventKind::approval_resolved || kind == RunEventKind::verification_ready ||
           kind == RunEventKind::verification_blocked || kind == RunEventKind::task_finished;
}

QString usage_text(const desktop::UsageSummary& usage) {
    const auto hit_rate = usage.cache_hit_percent();
    if (!hit_rate.has_value() && usage.prompt_tokens > 0) {
        return text(usage.cache_usage_reports > 0 ? Message::gui_usage_summary_cache_incomplete
                                                  : Message::gui_usage_summary_cache_unavailable,
                    {arg(Placeholder::requests, usage.calls),
                     arg(Placeholder::tokens, usage.total_tokens)});
    }
    return hit_rate.has_value()
               ? text(Message::gui_usage_summary_cached,
                      {arg(Placeholder::requests, usage.calls),
                       arg(Placeholder::tokens, usage.total_tokens),
                       arg(Placeholder::rate, *hit_rate)})
               : text(Message::gui_usage_summary, {arg(Placeholder::requests, usage.calls),
                                                   arg(Placeholder::tokens, usage.total_tokens)});
}

} // namespace

RunTimelineModel::RunTimelineModel(QObject* parent) : QAbstractListModel(parent) {}

int RunTimelineModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    const auto content_rows = events_.size() + (result_.has_value() ? 1U : 0U);
    return static_cast<int>(std::max<std::size_t>(content_rows, 1U) + 1U);
}

QVariant RunTimelineModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto kind = item_kind(index.row());
    if (role == ItemKindRole) {
        return static_cast<int>(kind);
    }

    if (kind == ItemKind::spacer) {
        return role == BottomInsetRole ? QVariant(bottom_inset_) : QVariant{};
    }

    if (kind == ItemKind::empty) {
        if (role == Qt::DisplayRole || role == TitleRole) {
            return text(has_named_task_   ? Message::gui_result_pending
                        : !has_workspace_ ? Message::gui_workspace_required
                                          : Message::gui_task_empty_hint);
        }
        return {};
    }

    if (kind == ItemKind::result) {
        switch (role) {
        case Qt::DisplayRole:
        case TitleRole:
            return text(Message::gui_result_title);
        case BodyRole:
            return QString::fromStdString(result_->answer);
        case UsageRole:
            return usage_text(result_->usage);
        case EvidenceRole:
            return QString::fromStdString(result_->unified_diff);
        case EvidenceActionRole:
            return result_->unified_diff.empty() ? QString{}
                                                 : text(Message::gui_action_view_changes);
        default:
            return {};
        }
    }

    const auto* event = event_at(index.row());
    if (event == nullptr) {
        return {};
    }
    const auto presentation = present_run_event(*event);
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:
        return presentation.title;
    case DetailRole:
        return presentation.detail;
    case ToneRole:
        return static_cast<int>(presentation.tone);
    case EvidenceRole:
        return is_inspectable(event->kind) ? QString::fromStdString(event->evidence) : QString{};
    case EvidenceActionRole:
        return is_inspectable(event->kind) && !event->evidence.empty()
                   ? text(Message::gui_action_open_evidence)
                   : QString{};
    default:
        return {};
    }
}

QHash<int, QByteArray> RunTimelineModel::roleNames() const {
    return {{ItemKindRole, "itemKind"},
            {TitleRole, "title"},
            {DetailRole, "detail"},
            {ToneRole, "tone"},
            {BodyRole, "body"},
            {UsageRole, "usage"},
            {EvidenceRole, "evidence"},
            {EvidenceActionRole, "evidenceAction"},
            {BottomInsetRole, "bottomInset"}};
}

void RunTimelineModel::set_state(const desktop::TaskWorkspaceState& state) {
    beginResetModel();
    events_ = state.events;
    result_ = state.result;
    has_named_task_ = !state.title.empty();
    has_workspace_ = !state.root.empty();
    endResetModel();
}

bool RunTimelineModel::append_latest_event(const desktop::TaskWorkspaceState& state) {
    if (state.result.has_value() || result_.has_value() || state.events.empty()) {
        return false;
    }
    has_named_task_ = !state.title.empty();
    has_workspace_ = !state.root.empty();

    std::size_t removed = 0;
    if (!events_.empty() && events_.front().sequence != state.events.front().sequence) {
        const auto retained = std::find_if(events_.begin(), events_.end(), [&](const auto& event) {
            return event.sequence == state.events.front().sequence;
        });
        if (retained == events_.end()) {
            return false;
        }
        removed = static_cast<std::size_t>(std::distance(events_.begin(), retained));
    }
    const auto retained_count = events_.size() - removed;
    if (state.events.size() < retained_count ||
        (retained_count > 0 &&
         events_.back().sequence != state.events[retained_count - 1].sequence)) {
        return false;
    }
    if (removed > 0) {
        beginRemoveRows({}, 0, static_cast<int>(removed - 1));
        events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(removed));
        endRemoveRows();
    }
    if (events_.empty()) {
        events_.push_back(state.events.front());
        emit dataChanged(index(0, 0), index(0, 0));
    }
    if (events_.size() < state.events.size()) {
        const auto first = events_.size();
        beginInsertRows({}, static_cast<int>(first), static_cast<int>(state.events.size() - 1));
        events_.insert(events_.end(), state.events.begin() + static_cast<std::ptrdiff_t>(first),
                       state.events.end());
        endInsertRows();
    }
    return true;
}

bool RunTimelineModel::set_bottom_inset(int height) {
    const auto next = std::max(0, height);
    if (bottom_inset_ == next) {
        return false;
    }
    bottom_inset_ = next;
    const auto spacer = index(rowCount() - 1, 0);
    emit dataChanged(spacer, spacer, {BottomInsetRole, Qt::SizeHintRole});
    return true;
}

void RunTimelineModel::retranslate() {
    if (rowCount() > 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0));
    }
}

QString RunTimelineModel::plain_text() const {
    QStringList lines;
    for (int row = 0; row < rowCount(); ++row) {
        const auto item = index(row, 0);
        for (const auto role : {TitleRole, DetailRole, BodyRole, EvidenceActionRole, UsageRole}) {
            const auto value = data(item, role).toString();
            if (!value.isEmpty()) {
                lines.push_back(value);
            }
        }
    }
    return lines.join(QLatin1Char('\n'));
}

QString RunTimelineModel::evidence_at(std::size_t index) const {
    std::size_t current = 0;
    for (const auto& event : events_) {
        if (event.evidence.empty()) {
            continue;
        }
        if (current++ == index) {
            return QString::fromStdString(event.evidence);
        }
    }
    if (result_.has_value() && !result_->unified_diff.empty() && current == index) {
        return QString::fromStdString(result_->unified_diff);
    }
    return {};
}

QString RunTimelineModel::first_evidence() const {
    return evidence_at(0);
}

RunTimelineModel::ItemKind RunTimelineModel::item_kind(int row) const {
    if (row == rowCount() - 1) {
        return ItemKind::spacer;
    }
    if (events_.empty() && !result_.has_value()) {
        return ItemKind::empty;
    }
    if (row >= static_cast<int>(events_.size())) {
        return ItemKind::result;
    }
    return ItemKind::event;
}

const desktop::RunEvent* RunTimelineModel::event_at(int row) const {
    if (row < 0 || row >= static_cast<int>(events_.size())) {
        return nullptr;
    }
    return &events_[static_cast<std::size_t>(row)];
}

} // namespace mint::ui
