#include "agent_run.hpp"

#include "agent_checkpoint.hpp"
#include "agent_support.hpp"

#include "mint/infrastructure/event_log.hpp"
#include "mint/infrastructure/session_store.hpp"
#include "mint/runtime/task_control.hpp"

#include "../infrastructure/diagnostic_log.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace mint::agent_detail {

AgentRun::AgentRun(ModelClient& model, ToolRegistry& tools, std::ostream& output,
                   const AgentOptions& options, const std::string& requested_task,
                   std::string system_prompt)
    : model_(model), tools_(tools), output_(output), options_(options),
      started_at_(std::chrono::steady_clock::now()), user_request_(requested_task),
      system_prompt_(std::move(system_prompt)) {}

AgentResult AgentRun::run() {
    initialize();
    previous_duration_ms_ = result_.duration_ms;
    start_task();
    tool_definitions_ = tools_.definitions();

    while (true) {
        if (auto stopped = finish_if_stopped()) {
            return std::move(*stopped);
        }
        if (!pending_calls_.empty()) {
            execute_next_tool();
            continue;
        }
        if (model_calls_this_run_ >= options_.max_turns) {
            return finish("max_turns", "max_turns_exhausted",
                          "达到本次运行的最大轮数，任务尚未完成；可以使用会话快照继续。");
        }

        ModelReply reply;
        try {
            reply = request_model();
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
        if (!pending_calls_.empty()) {
            continue;
        }
        if (reply.text.empty()) {
            throw std::runtime_error("模型既没有回答，也没有调用工具");
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
            throw std::invalid_argument("任务内容不能为空");
        }
        messages_ = Json::array({{{"role", "system"}, {"content", system_prompt_}},
                                 {{"role", "user"}, {"content", user_request_}}});
        return;
    }
    if (!user_request_.empty()) {
        throw std::invalid_argument("恢复会话时不能同时提供新的任务内容");
    }
    if (!options_.session_store->exists()) {
        throw std::invalid_argument("找不到要恢复的会话快照");
    }

    auto restored = restore_session(options_.session_store->load(), tools_,
                                    options_.require_verification_after_write,
                                    options_.max_context_bytes, options_.retry_in_flight_tool);
    user_request_ = std::move(restored.user_request);
    messages_ = std::move(restored.messages);
    result_ = std::move(restored.result);
    pending_calls_ = std::move(restored.pending_calls);
    recovered_in_flight_ = restored.recovered_in_flight;
}

void AgentRun::start_task() {
    if (!options_.resume_session) {
        save_checkpoint("running");
    }
    diagnostics::emit(diagnostics::Level::info, "task.started",
                      {{"resumed", options_.resume_session},
                       {"recovered_in_flight", recovered_in_flight_},
                       {"previous_turns", result_.turns},
                       {"max_turns", options_.max_turns},
                       {"max_context_bytes", options_.max_context_bytes},
                       {"verification_required", options_.require_verification_after_write}});
    emit("task_started", {{"workspace_root", tools_.root().generic_string()},
                          {"resumed", options_.resume_session},
                          {"recovered_in_flight", recovered_in_flight_},
                          {"retry_in_flight_authorized", options_.retry_in_flight_tool},
                          {"previous_turns", result_.turns},
                          {"max_turns_this_run", options_.max_turns},
                          {"max_context_bytes", options_.max_context_bytes},
                          {"max_context_estimated_tokens", (options_.max_context_bytes + 3) / 4},
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
    if (options_.session_store != nullptr) {
        options_.session_store->save(make_checkpoint_document(
            status, user_request_, messages_, result_, pending_calls_, in_flight_call_, tools_,
            options_.require_verification_after_write, options_.max_context_bytes));
    }
}

void AgentRun::emit(std::string type, Json data) {
    if (options_.event_log != nullptr) {
        options_.event_log->emit(std::move(type), std::move(data));
    }
}

std::string AgentRun::stop_reason() const {
    return options_.task_control == nullptr ? std::string{} : options_.task_control->stop_reason();
}

std::optional<AgentResult> AgentRun::finish_if_stopped() {
    const auto reason = stop_reason();
    if (reason.empty()) {
        return std::nullopt;
    }
    return finish(reason == "cancelled" ? "cancelled" : "timed_out",
                  reason == "cancelled" ? "user_cancelled" : "total_budget_exhausted",
                  reason == "cancelled" ? "任务已取消。" : "任务超过总时间预算。");
}

AgentResult AgentRun::finish(std::string status, std::string reason, std::string answer) {
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
          {"model", model_summary_to_json(result_.model)},
          {"changed_file_count", result_.changes.files.size()}});
    diagnostics::emit(diagnostics::Level::info, "task.finished",
                      {{"status", result_.status},
                       {"turns", result_.turns},
                       {"duration_ms", result_.duration_ms},
                       {"verification_status", result_.verification_status},
                       {"tool_calls", result_.execution.tool_calls},
                       {"changed_file_count", result_.changes.files.size()}});
    if (result_.completed) {
        output_ << "\n[最终回答]\n" << result_.answer << '\n';
    } else {
        output_ << "\n[停止] " << result_.answer << '\n';
    }
    print_final_state(output_, result_);
    return result_;
}

AgentResult run_agent_loop(ModelClient& model, ToolRegistry& tools, std::ostream& output,
                           const AgentOptions& options, const std::string& requested_task,
                           std::string system_prompt) {
    return AgentRun(model, tools, output, options, requested_task, std::move(system_prompt)).run();
}

} // namespace mint::agent_detail
