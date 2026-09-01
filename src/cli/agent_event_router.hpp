#pragma once

#include "mint/ports/agent_event_sink.hpp"

namespace mint {

class EventLog;

namespace cli::command_detail {

class AgentEventRouter final : public AgentEventSink {
  public:
    explicit AgentEventRouter(EventLog* event_log) noexcept;

    void emit(std::string type, Json data = Json::object()) override;

  private:
    EventLog* event_log_;
};

} // namespace cli::command_detail
} // namespace mint
