#include "agent_run.hpp"

#include "agent_checkpoint.hpp"
#include "agent_loop.hpp"
#include "agent_model_summary.hpp"
#include "agent_reporting.hpp"

#include "mint/localization/localization.hpp"
#include "mint/runtime/terminal_text.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace mint::agent_detail {
namespace {

using localization::Message;
using localization::message;
using localization::Placeholder;

const char* recovery_name(ChangeTransactionRecovery recovery) {
    switch (recovery) {
    case ChangeTransactionRecovery::none:
        return "none";
    case ChangeTransactionRecovery::rolled_back:
        return "rolled_back";
    case ChangeTransactionRecovery::committed:
        return "committed";
    }
    return "none";
}

} // namespace

AgentRun::AgentRun(ModelClient& model, AgentTools& tools, std::ostream& output,
                   const AgentOptions& options, const AgentServices& services,
                   const std::string& requested_task, std::string system_prompt)
    : model_(model), tools_(tools), output_(output), options_(options), services_(services),
      started_at_(std::chrono::steady_clock::now()), user_request_(requested_task),
      system_prompt_(std::move(system_prompt)),
      conversation_(Conversation::start(system_prompt_, user_request_)) {
    result_.model.max_total_tokens = options_.max_total_tokens;
}

AgentResult AgentRun::run() {
    initialize();
    previous_duration_ms_ = result_.duration_ms;
    start_task();
    tool_definitions_ = tools_.definitions();

    while (true) {
        if (auto stopped = finish_if_stopped()) {
            return std::move(*stopped);
        }
        if (tools_.workspace_integrity_failed()) {
            return finish("failed", "workspace_integrity_failed",
                          message(Message::agent_finish_workspace_integrity_failed));
        }
        if (auto exhausted = finish_if_token_budget_exhausted()) {
            return std::move(*exhausted);
        }
        if (!pending_calls_.empty()) {
            execute_next_tool();
            continue;
        }
        const auto final_answer_only =
            model_calls_this_run_ >= options_.max_turns && can_request_verified_final_answer();
        if (model_calls_this_run_ >= options_.max_turns && !final_answer_only) {
            return finish("max_turns", "max_turns_exhausted",
                          message(Message::agent_finish_max_turns));
        }

        ModelReply reply;
        try {
            reply = request_model(final_answer_only ? Json::array() : tool_definitions_);
        } catch (...) {
            if (auto stopped = finish_if_stopped()) {
                return std::move(*stopped);
            }
            throw;
        }
        accept_model_reply(reply);

        if (auto stopped = finish_if_stopped()) {
            return std::move(*stopped);
        }
        if ((reply.text.empty() || !pending_calls_.empty()) &&
            token_budget_exhausted(result_.model)) {
            auto exhausted = finish_if_token_budget_exhausted();
            return std::move(*exhausted);
        }
        if (final_answer_only && !pending_calls_.empty()) {
            return finish("max_turns", "verified_final_answer_missing",
                          message(Message::agent_finish_final_answer_missing));
        }
        if (final_answer_only && reply.text.empty()) {
            return finish("max_turns", "verified_final_answer_missing",
                          message(Message::agent_finish_final_answer_missing));
        }
        if (!pending_calls_.empty()) {
            continue;
        }
        if (reply.text.empty()) {
            throw std::runtime_error(message(Message::agent_reply_empty));
        }
        if (block_unverified_finish()) {
            continue;
        }
        return finish("completed", {}, std::move(reply.text));
    }
}

void AgentRun::initialize() {
    if (!options_.resume_session) {
        if (user_request_.empty()) {
            throw std::invalid_argument(message(Message::agent_task_empty));
        }
        const auto capabilities = tools_.capabilities();
        if (capabilities.durable_change_transactions && services_.session_repository == nullptr) {
            throw std::invalid_argument(
                message(Message::agent_session_transaction_requires_checkpoint));
        }
        if (services_.session_repository != nullptr && services_.session_repository->exists()) {
            throw std::invalid_argument(message(Message::agent_session_snapshot_exists));
        }
        transaction_recovery_ = tools_.reconcile_change_transaction(std::nullopt);
        return;
    }
    if (!user_request_.empty()) {
        throw std::invalid_argument(message(Message::agent_session_resume_task_conflict));
    }
    if (!services_.session_repository->exists()) {
        throw std::invalid_argument(message(Message::agent_session_snapshot_missing));
    }

    auto restored = restore_session(
        services_.session_repository->load(), tools_, options_.require_verification_after_write,
        options_.max_context_bytes, options_.max_total_tokens, options_.retry_in_flight_tool);
    if (restored.result.model.max_total_tokens != options_.max_total_tokens) {
        throw std::invalid_argument(message(Message::agent_session_token_budget_mismatch));
    }
    user_request_ = std::move(restored.user_request);
    conversation_ = Conversation::restore(std::move(restored.messages));
    result_ = std::move(restored.result);
    pending_calls_ = std::move(restored.pending_calls);
    transaction_recovery_ = restored.transaction_recovery;
    recovered_in_flight_ = restored.recovered_in_flight;
}

void AgentRun::start_task() {
    if (!options_.resume_session) {
        save_checkpoint("running");
    }
    const auto capabilities = tools_.capabilities();
    emit("task_started",
         {{"workspace_root", capabilities.workspace_root.generic_string()},
          {"resumed", options_.resume_session},
          {"transaction_recovery", recovery_name(transaction_recovery_)},
          {"recovered_in_flight", recovered_in_flight_},
          {"retry_in_flight_authorized", options_.retry_in_flight_tool},
          {"previous_turns", result_.turns},
          {"max_turns_this_run", options_.max_turns},
          {"max_context_bytes", options_.max_context_bytes},
          {"max_total_tokens", options_.max_total_tokens},
          {"max_context_estimated_tokens",
           model_token_estimation::from_serialized_bytes(options_.max_context_bytes)},
          {"require_verification", options_.require_verification_after_write}});
}

void AgentRun::update_duration() {
    result_.duration_ms =
        previous_duration_ms_ + std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started_at_)
                                    .count();
}

void AgentRun::save_checkpoint(const std::string& status) {
    update_duration();
    if (services_.session_repository != nullptr) {
        services_.session_repository->save(make_checkpoint_document(
            status, user_request_, conversation_.messages(), result_, pending_calls_,
            in_flight_call_, tools_, options_.require_verification_after_write,
            options_.max_context_bytes, options_.max_total_tokens));
        tools_.finalize_change_transaction();
    }
}

void AgentRun::emit(std::string type, Json data) {
    if (services_.event_sink != nullptr) {
        services_.event_sink->emit(std::move(type), std::move(data));
    }
}

std::string AgentRun::stop_reason() const {
    return services_.stop_token == nullptr ? std::string{} : services_.stop_token->stop_reason();
}

std::optional<AgentResult> AgentRun::finish_if_stopped() {
    const auto reason = stop_reason();
    if (reason.empty()) {
        return std::nullopt;
    }
    return finish(reason == "cancelled" ? "cancelled" : "timed_out",
                  reason == "cancelled" ? "user_cancelled" : "total_budget_exhausted",
                  message(reason == "cancelled" ? Message::agent_finish_cancelled
                                                : Message::agent_finish_timed_out));
}

std::optional<AgentResult> AgentRun::finish_if_token_budget_exhausted() {
    if (!token_budget_exhausted(result_.model)) {
        return std::nullopt;
    }
    emit("token_budget_exhausted", {{"max_total_tokens", options_.max_total_tokens},
                                    {"reported_total_tokens", result_.model.total_tokens},
                                    {"model_calls", result_.model.calls},
                                    {"usage_reports", result_.model.usage_reports},
                                    {"pending_tool_call_count", pending_calls_.size()}});
    return finish("budget_exhausted", "max_total_tokens_exhausted",
                  message(Message::agent_finish_token_budget_exhausted));
}

AgentResult AgentRun::finish(std::string status, std::string reason, std::string answer) {
    if (status != "failed" && tools_.workspace_integrity_failed()) {
        status = "failed";
        reason = "workspace_integrity_failed";
        answer = message(Message::agent_finish_workspace_integrity_failed);
    }
    result_.status = std::move(status);
    result_.completed = result_.status == "completed";
    result_.stop_reason = std::move(reason);
    result_.answer = std::move(answer);
    update_duration();
    capture_final_state(result_, tools_);
    save_checkpoint(result_.status);
    emit("task_finished",
         {{"status", result_.status},
          {"stop_reason", result_.stop_reason.empty() ? Json(nullptr) : Json(result_.stop_reason)},
          {"turns", result_.turns},
          {"duration_ms", result_.duration_ms},
          {"verification_status", result_.verification_status},
          {"tool_calls", result_.execution.tool_calls},
          {"max_total_tokens", result_.model.max_total_tokens},
          {"reported_total_tokens", result_.model.total_tokens},
          {"token_usage_coverage", token_budget_to_json(result_.model).at("usage_coverage")},
          {"model", model_summary_to_json(result_.model)},
          {"changed_file_count", result_.changes.files.size()}});
    if (result_.completed) {
        output_ << message(Message::agent_output_final_answer)
                << escape_terminal_text(result_.answer) << '\n';
    } else {
        output_ << message(
            Message::agent_output_stopped,
            {localization::arg(Placeholder::reason, escape_terminal_field(result_.answer))});
    }
    print_final_state(output_, result_);
    return result_;
}

AgentResult run_agent_loop(ModelClient& model, AgentTools& tools, std::ostream& output,
                           const AgentOptions& options, const AgentServices& services,
                           const std::string& requested_task, std::string system_prompt) {
    return AgentRun(model, tools, output, options, services, requested_task,
                    std::move(system_prompt))
        .run();
}

} // namespace mint::agent_detail
