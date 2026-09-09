#pragma once

#include "application/desktop_types.hpp"

#include <QString>

namespace mint::ui {

enum class RunEventTone { neutral, attention, success, critical };

struct RunEventPresentation {
    QString title;
    QString detail;
    RunEventTone tone = RunEventTone::neutral;
};

[[nodiscard]] RunEventPresentation present_run_event(const desktop::RunEvent& event);

} // namespace mint::ui
