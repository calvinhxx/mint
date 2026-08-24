#pragma once

#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "aiagent/domain/model.hpp"
#include "aiagent/tools/tool_registry.hpp"

namespace aiagent {

class EventLog;
class SessionStore;
class TaskControl;

struct AgentOptions {
    std::size_t max_turns = 12;
    std::size_t max_context_bytes = 24 * 1024;
    bool require_verification_after_write = false;
    std::shared_ptr<TaskControl> task_control;
    EventLog* event_log = nullptr;
    SessionStore* session_store = nullptr;
    bool resume_session = false;
    bool retry_in_flight_tool = false;
};

struct ExecutionSummary {
    std::size_t tool_calls = 0;
    std::size_t successful_tool_calls = 0;
    std::size_t tool_errors = 0;
    std::size_t file_changes = 0;
    std::size_t command_calls = 0;
    std::size_t recipe_calls = 0;
    std::size_t verification_commands = 0;
    std::size_t commands_passed = 0;
    std::size_t commands_failed = 0;
    std::size_t commands_timed_out = 0;
    std::size_t commands_cancelled = 0;
    std::size_t commands_denied = 0;
    std::size_t last_file_change_call = 0;
    std::size_t last_command_call = 0;
    std::string last_command_outcome = "not_run";
    bool last_command_verification_eligible = false;
};

struct ChangeSummary {
    std::vector<std::string> files;
    std::string unified_diff;
    bool diff_truncated = false;
};

struct ModelSummary {
    std::size_t calls = 0;
    std::size_t attempts = 0;
    std::size_t retries = 0;
    std::size_t usage_reports = 0;
    std::size_t prompt_tokens = 0;
    std::size_t completion_tokens = 0;
    std::size_t total_tokens = 0;
    std::size_t cached_tokens = 0;
    long long duration_ms = 0;
    std::string adapter;
    std::string model;
    std::string last_response_id;
};

struct AgentResult {
    bool completed = false;
    std::string status = "running";
    std::string stop_reason;
    std::string answer;
    std::size_t turns = 0;
    long long duration_ms = 0;
    ExecutionSummary execution;
    ModelSummary model;
    ChangeSummary changes;
    std::string verification_status = "not_required";
};

[[nodiscard]] Json agent_result_to_json(const AgentResult& result);

class Agent {
  public:
    Agent(ModelClient& model, ToolRegistry& tools, std::ostream& output, AgentOptions options = {});

    [[nodiscard]] AgentResult run(const std::string& user_request);

  private:
    [[nodiscard]] std::string system_prompt() const;

    ModelClient& model_;
    ToolRegistry& tools_;
    std::ostream& output_;
    AgentOptions options_;
};

} // namespace aiagent
