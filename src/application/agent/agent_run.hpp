#pragma once

#include "agent_conversation.hpp"

#include "mint/application/agent.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <ostream>
#include <string>

namespace mint::agent_detail {

class AgentRun final {
  public:
    AgentRun(ModelClient& model, AgentTools& tools, std::ostream& output,
             const AgentOptions& options, const AgentServices& services,
             const std::string& requested_task, std::string system_prompt);

    [[nodiscard]] AgentResult run();

  private:
    void initialize();
    void start_task();
    void update_duration();
    void save_checkpoint(const std::string& status);
    void emit(std::string type, Json data = Json::object());
    [[nodiscard]] std::string stop_reason() const;
    [[nodiscard]] std::optional<AgentResult> finish_if_stopped();
    [[nodiscard]] AgentResult finish(std::string status, std::string reason, std::string answer);

    void execute_next_tool();
    void prompt_verified_completion();
    [[nodiscard]] bool can_request_verified_final_answer() const;
    [[nodiscard]] ModelReply request_model(const Json& available_tools);
    void accept_model_reply(ModelReply& reply);
    [[nodiscard]] bool block_unverified_finish();

    ModelClient& model_;
    AgentTools& tools_;
    std::ostream& output_;
    const AgentOptions& options_;
    const AgentServices& services_;
    std::chrono::steady_clock::time_point started_at_;
    std::string user_request_;
    std::string system_prompt_;
    Conversation conversation_;
    AgentResult result_;
    std::deque<ToolCall> pending_calls_;
    std::optional<ToolCall> in_flight_call_;
    Json tool_definitions_;
    long long previous_duration_ms_ = 0;
    std::size_t model_calls_this_run_ = 0;
    std::size_t prompted_verification_call_ = 0;
    ChangeTransactionRecovery transaction_recovery_ = ChangeTransactionRecovery::none;
    bool recovered_in_flight_ = false;
};

} // namespace mint::agent_detail
