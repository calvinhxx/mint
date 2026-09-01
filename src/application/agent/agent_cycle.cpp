#include "agent_run.hpp"

#include "agent_context.hpp"
#include "agent_execution.hpp"
#include "agent_model_summary.hpp"
#include "agent_reporting.hpp"

#include "mint/runtime/terminal_text.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mint::agent_detail {
namespace {

std::size_t effective_context_limit(std::size_t configured_limit,
                                    const ModelRequestLimits& request_limits) {
    if (!request_limits.bounded()) {
        return configured_limit;
    }

    const auto provider_limit = request_limits.available_input_bytes();
    if (provider_limit == 0) {
        throw std::runtime_error("模型请求 Token 预算不足以容纳输出、工具定义和安全余量");
    }
    return std::min(configured_limit, provider_limit);
}

} // namespace

void AgentRun::execute_next_tool() {
    const auto call = pending_calls_.front();
    in_flight_call_ = call;
    save_checkpoint("running");
    emit("tool_started", {{"turn", result_.turns},
                          {"tool_call_id", call.id},
                          {"name", call.name},
                          {"arguments_summary", event_arguments_summary(tools_, call)}});
    output_ << "[调用工具] " << escape_terminal_field(call.name) << ' '
            << escape_terminal_field(tools_.describe_call(call)) << '\n';
    const auto tool_result = tools_.execute(call);
    record_execution(result_.execution, call, tool_result);
    conversation_.append_tool_result(call, tool_result);
    pending_calls_.pop_front();
    in_flight_call_.reset();
    emit("tool_completed", {{"turn", result_.turns},
                            {"tool_call_id", call.id},
                            {"name", call.name},
                            {"result", safe_tool_result(tool_result)}});
    output_ << "[工具完成] 结果已交回模型\n";
    if (pending_calls_.empty()) {
        prompt_verified_completion();
    }
    save_checkpoint("running");
}

void AgentRun::prompt_verified_completion() {
    const auto& execution = result_.execution;
    if (!options_.require_verification_after_write || execution.last_file_change_call == 0 ||
        execution.last_command_call == 0 ||
        execution.last_command_call == prompted_verification_call_ ||
        verification_status(execution, tools_.has_workspace_changes()) != "passed") {
        return;
    }

    prompted_verification_call_ = execution.last_command_call;
    conversation_.append_user(
        "[Harness status] The latest verification-eligible command passed after the latest "
        "workspace change. If all user requirements are satisfied, call no more tools and "
        "return the final answer now. Continue only for a specific unmet requirement.");
    emit("verification_ready", {{"turn", result_.turns},
                                {"last_file_change_call", execution.last_file_change_call},
                                {"last_command_call", execution.last_command_call}});
    output_ << "[继续] 最新修改已通过验证；若任务已完成，请返回最终回答。\n";
}

bool AgentRun::can_request_verified_final_answer() const {
    const auto& execution = result_.execution;
    return options_.require_verification_after_write && prompted_verification_call_ != 0 &&
           prompted_verification_call_ == execution.last_command_call &&
           verification_status(execution, tools_.has_workspace_changes()) == "passed";
}

ModelReply AgentRun::request_model(const Json& available_tools) {
    const auto turn = result_.turns + 1;
    output_ << "\n[第 " << turn << " 轮] 询问模型下一步...\n";
    const auto request_limits = model_.request_limits(available_tools);
    const auto context_limit = effective_context_limit(options_.max_context_bytes, request_limits);
    const auto context = compact_context(conversation_.messages(), context_limit);
    if (context.dropped_groups != 0 || context.payloads_compacted) {
        emit("context_compacted", {{"turn", turn},
                                   {"full_bytes", context.full_bytes},
                                   {"sent_bytes", context.sent_bytes},
                                   {"full_estimated_tokens", context.full_estimated_tokens()},
                                   {"sent_estimated_tokens", context.sent_estimated_tokens()},
                                   {"dropped_groups", context.dropped_groups},
                                   {"payloads_compacted", context.payloads_compacted},
                                   {"context_byte_limit", context_limit},
                                   {"max_request_tokens", request_limits.max_request_tokens}});
    }
    emit("model_requested", {{"turn", turn},
                             {"message_count", context.messages.size()},
                             {"full_context_bytes", context.full_bytes},
                             {"sent_context_bytes", context.sent_bytes},
                             {"full_context_estimated_tokens", context.full_estimated_tokens()},
                             {"sent_context_estimated_tokens", context.sent_estimated_tokens()},
                             {"context_byte_limit", context_limit},
                             {"max_request_tokens", request_limits.max_request_tokens},
                             {"reserved_output_tokens", request_limits.reserved_output_tokens},
                             {"request_overhead_tokens", request_limits.request_overhead_tokens},
                             {"request_token_safety_margin", request_limits.safety_margin_tokens},
                             {"final_answer_only", available_tools.empty()}});
    return model_.complete(context.messages, available_tools);
}

void AgentRun::accept_model_reply(ModelReply& reply) {
    ++model_calls_this_run_;
    ++result_.turns;
    record_model_call(result_.model, reply);
    print_model_usage(output_, reply.usage);
    conversation_.append_assistant(std::move(reply.assistant_message));
    pending_calls_.clear();
    for (auto& call : reply.tool_calls) {
        pending_calls_.push_back(std::move(call));
    }
    emit("model_completed", {{"turn", result_.turns},
                             {"tool_call_count", pending_calls_.size()},
                             {"has_text", !reply.text.empty()},
                             {"usage", model_usage_json(reply.usage)},
                             {"token_budget", token_budget_to_json(result_.model)},
                             {"metadata", model_metadata_json(reply.metadata)}});
    save_checkpoint("running");
}

bool AgentRun::block_unverified_finish() {
    if (tools_.workspace_integrity_failed()) {
        return false;
    }
    const auto status = verification_status(result_.execution, tools_.has_workspace_changes());
    if (!options_.require_verification_after_write || status == "not_required" ||
        status == "passed") {
        return false;
    }

    output_ << "\n[继续] 工作区变更尚未通过最新验证（" << status << "），拒绝结束任务。\n";
    conversation_.append_user(
        "[Harness requirement] The workspace still has unverified changes. "
        "The current verification status is " +
        status +
        ". Continue the task: inspect the last command result, fix the files if needed, "
        "and run an approved verification command after the latest edit. "
        "Only provide a final answer after the latest command exits with code 0.");
    emit("verification_blocked", {{"turn", result_.turns}, {"verification_status", status}});
    save_checkpoint("running");
    return true;
}

} // namespace mint::agent_detail
