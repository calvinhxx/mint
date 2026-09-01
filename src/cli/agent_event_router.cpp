#include "agent_event_router.hpp"

#include "mint/infrastructure/diagnostic_log.hpp"
#include "mint/infrastructure/event_log.hpp"

#include <utility>

namespace mint::cli::command_detail {

AgentEventRouter::AgentEventRouter(EventLog* event_log) noexcept : event_log_(event_log) {}

void AgentEventRouter::emit(std::string type, Json data) {
    if (type == "task_started") {
        auto fields = data;
        fields["max_turns"] = data.value("max_turns_this_run", std::size_t{0});
        fields["verification_required"] = data.value("require_verification", false);
        diagnostics::emit(diagnostics::Level::info, "task.started", std::move(fields));
    } else if (type == "task_finished") {
        diagnostics::emit(diagnostics::Level::info, "task.finished", data);
    }

    if (event_log_ != nullptr) {
        event_log_->emit(std::move(type), std::move(data));
    }
}

} // namespace mint::cli::command_detail
