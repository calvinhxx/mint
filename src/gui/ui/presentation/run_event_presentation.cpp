#include "run_event_presentation.hpp"

#include "ui/support/localized_text.hpp"
#include "ui/support/status_text.hpp"

#include "mint/localization/localization.hpp"

#include <string_view>

namespace mint::ui {
namespace {

using localization::arg;
using localization::Message;
using localization::Placeholder;

QString turn_detail(const desktop::RunEvent& event) {
    return event.turn.has_value()
               ? text(Message::gui_event_detail_turn, {arg(Placeholder::turn, *event.turn)})
               : QString{};
}

Message tool_event_message(std::string_view name, bool completed) {
    if (name == "list_files" || name == "read_file") {
        return completed ? Message::gui_event_tool_files_completed
                         : Message::gui_event_tool_files_started;
    }
    if (name == "search_text") {
        return completed ? Message::gui_event_tool_search_completed
                         : Message::gui_event_tool_search_started;
    }
    if (name == "apply_patch" || name == "apply_changeset") {
        return completed ? Message::gui_event_tool_changes_completed
                         : Message::gui_event_tool_changes_started;
    }
    if (name == "run_command" || name == "run_recipe") {
        return completed ? Message::gui_event_tool_command_completed
                         : Message::gui_event_tool_command_started;
    }
    return completed ? Message::gui_event_tool_generic_completed
                     : Message::gui_event_tool_generic_started;
}

QString event_title(const desktop::RunEvent& event) {
    using desktop::RunEventKind;
    switch (event.kind) {
    case RunEventKind::task_started:
        return text(Message::gui_event_task_started);
    case RunEventKind::model_requested:
        return text(Message::gui_event_model_requested);
    case RunEventKind::model_completed:
        return text(Message::gui_event_model_completed);
    case RunEventKind::model_progress:
        return text(Message::gui_event_model_progress);
    case RunEventKind::tool_started:
        return text(tool_event_message(event.name, false));
    case RunEventKind::tool_completed:
        return text(tool_event_message(event.name, true));
    case RunEventKind::approval_requested:
        return text(Message::gui_event_approval_requested);
    case RunEventKind::approval_resolved:
        return text(Message::gui_event_approval_resolved);
    case RunEventKind::verification_ready:
        return text(Message::gui_event_verification_ready);
    case RunEventKind::verification_blocked:
        return text(Message::gui_event_verification_blocked);
    case RunEventKind::context_compacted:
        return text(Message::gui_event_context_compacted);
    case RunEventKind::task_finished:
        return text(Message::gui_event_task_finished);
    case RunEventKind::unknown:
        return text(Message::gui_event_activity);
    }
    return text(Message::gui_event_activity);
}

QString event_detail(const desktop::RunEvent& event) {
    using desktop::RunEventKind;
    if (event.kind == RunEventKind::task_started) {
        return text(Message::gui_event_detail_workspace, {arg(Placeholder::path, event.target)});
    }
    if (event.kind == RunEventKind::task_finished ||
        event.kind == RunEventKind::verification_blocked) {
        const auto& status =
            event.kind == RunEventKind::task_finished ? event.status : event.verification_status;
        const auto localized = status_text(QString::fromStdString(status));
        return text(Message::gui_event_detail_status,
                    {arg(Placeholder::status, localized.toUtf8().toStdString())});
    }
    if (event.kind == RunEventKind::tool_started && !event.target.empty()) {
        return QString::fromStdString(event.target);
    }
    if (event.kind == RunEventKind::approval_requested) {
        if (event.affected_paths > 0) {
            return text(Message::gui_event_detail_file_count,
                        {arg(Placeholder::count, event.affected_paths)});
        }
        return QString::fromStdString(event.target);
    }
    return turn_detail(event);
}

RunEventTone event_tone(const desktop::RunEvent& event) {
    using desktop::RunEventKind;
    if (event.kind == RunEventKind::approval_requested ||
        event.kind == RunEventKind::verification_blocked) {
        return RunEventTone::attention;
    }
    if (event.kind == RunEventKind::verification_ready) {
        return RunEventTone::success;
    }
    if (event.kind == RunEventKind::task_finished) {
        if (event.status == "completed") {
            return RunEventTone::success;
        }
        if (event.status == "failed" || event.status == "timed_out") {
            return RunEventTone::critical;
        }
    }
    return RunEventTone::neutral;
}

} // namespace

RunEventPresentation present_run_event(const desktop::RunEvent& event) {
    return {.title = event_title(event), .detail = event_detail(event), .tone = event_tone(event)};
}

} // namespace mint::ui
