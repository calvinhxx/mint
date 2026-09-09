#pragma once

#include "ui/support/localized_text.hpp"

#include <QString>

namespace mint::ui {

inline QString status_text(const QString& status) {
    using localization::Message;
    if (status == QStringLiteral("running")) {
        return text(Message::gui_status_running);
    }
    if (status == QStringLiteral("completed")) {
        return text(Message::gui_status_completed);
    }
    if (status == QStringLiteral("failed")) {
        return text(Message::gui_status_failed);
    }
    if (status == QStringLiteral("max_turns")) {
        return text(Message::gui_status_turn_limit);
    }
    if (status == QStringLiteral("timed_out")) {
        return text(Message::gui_status_timed_out);
    }
    if (status == QStringLiteral("budget_exhausted")) {
        return text(Message::gui_status_budget_exhausted);
    }
    if (status == QStringLiteral("cancelled")) {
        return text(Message::gui_status_cancelled);
    }
    if (status == QStringLiteral("waiting")) {
        return text(Message::gui_status_waiting);
    }
    if (status == QStringLiteral("resumable")) {
        return text(Message::gui_status_resumable);
    }
    if (status.isEmpty() || status == QStringLiteral("pending") ||
        status == QStringLiteral("created")) {
        return text(Message::gui_status_pending);
    }
    return text(Message::gui_status_unknown);
}

inline QString persisted_status_code(const QString& status, bool resumable) {
    return resumable ? QStringLiteral("resumable") : status;
}

} // namespace mint::ui
