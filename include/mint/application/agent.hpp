#pragma once

#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

#include "mint/domain/model.hpp"
#include "mint/domain/runtime_settings.hpp"
#include "mint/ports/agent_event_sink.hpp"
#include "mint/ports/model_client.hpp"
#include "mint/ports/session_repository.hpp"
#include "mint/ports/stop_token.hpp"
#include "mint/ports/tool_runtime.hpp"

namespace mint {

struct AgentOptions {
    std::size_t max_turns = runtime_defaults::max_turns;
    std::size_t max_context_bytes = runtime_defaults::max_context_bytes;
    // EN: Zero disables the task-level cumulative token budget.
    // ZH-CN: 值为零时，不启用任务级累计 token 预算。
    std::size_t max_total_tokens = runtime_defaults::max_total_tokens;
    bool require_verification_after_write = false;
    bool resume_session = false;
    bool retry_in_flight_tool = false;
};

struct AgentServices {
    StopToken* stop_token = nullptr;
    AgentEventSink* event_sink = nullptr;
    SessionRepository* session_repository = nullptr;
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
    struct File {
        std::string path;
        std::string status;

        bool operator==(const File&) const = default;
    };

    std::vector<std::string> files;
    std::vector<File> details;
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
    std::size_t max_total_tokens = runtime_defaults::max_total_tokens;
    std::size_t streamed_calls = 0;
    std::size_t stream_events = 0;
    std::size_t streamed_bytes = 0;
    long long duration_ms = 0;
    std::string adapter;
    std::string provider;
    std::string model;
    std::string last_response_id;
    std::size_t max_request_tokens = 0;
    std::string max_request_tokens_source;
    std::size_t response_header_max_request_tokens = 0;
    std::size_t request_token_estimate_bytes_per_token =
        model_token_estimation::serialized_bytes_per_token;
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
    Agent(ModelClient& model, AgentTools& tools, std::ostream& output, AgentOptions options = {},
          AgentServices services = {});

    [[nodiscard]] AgentResult run(const std::string& user_request);

  private:
    [[nodiscard]] std::string system_prompt() const;

    ModelClient& model_;
    AgentTools& tools_;
    std::ostream& output_;
    AgentOptions options_;
    AgentServices services_;
};

} // namespace mint
