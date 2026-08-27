#include "agent_run.hpp"

#include "agent_support.hpp"

#include <stdexcept>
#include <utility>

namespace mint::agent_detail {

void AgentRun::execute_next_tool() {
    const auto call = pending_calls_.front();
    in_flight_call_ = call;
    save_checkpoint("running");
    emit("tool_started", {{"turn", result_.turns},
                          {"tool_call_id", call.id},
                          {"name", call.name},
                          {"arguments_summary", event_arguments_summary(tools_, call)}});
    output_ << "[调用工具] " << call.name << ' ' << tools_.describe_call(call) << '\n';
    const auto tool_result = tools_.execute(call);
    record_execution(result_.execution, call, tool_result);
    messages_.push_back({{"role", "tool"}, {"tool_call_id", call.id}, {"content", tool_result}});
    pending_calls_.pop_front();
    in_flight_call_.reset();
    emit("tool_completed", {{"turn", result_.turns},
                            {"tool_call_id", call.id},
                            {"name", call.name},
                            {"result", safe_tool_result(tool_result)}});
    output_ << "[工具完成] 结果已交回模型\n";
    save_checkpoint("running");
}

ModelReply AgentRun::request_model() {
    const auto turn = result_.turns + 1;
    output_ << "\n[第 " << turn << " 轮] 询问模型下一步...\n";
    const auto context = compact_context(messages_, options_.max_context_bytes);
    if (context.dropped_groups != 0 || context.payloads_compacted) {
        emit("context_compacted", {{"turn", turn},
                                   {"full_bytes", context.full_bytes},
                                   {"sent_bytes", context.sent_bytes},
                                   {"full_estimated_tokens", context.full_estimated_tokens()},
                                   {"sent_estimated_tokens", context.sent_estimated_tokens()},
                                   {"dropped_groups", context.dropped_groups},
                                   {"payloads_compacted", context.payloads_compacted}});
    }
    emit("model_requested", {{"turn", turn},
                             {"message_count", context.messages.size()},
                             {"full_context_bytes", context.full_bytes},
                             {"sent_context_bytes", context.sent_bytes},
                             {"full_context_estimated_tokens", context.full_estimated_tokens()},
                             {"sent_context_estimated_tokens", context.sent_estimated_tokens()}});
    return model_.complete(context.messages, tool_definitions_);
}

void AgentRun::accept_model_reply(ModelReply& reply) {
    ++model_calls_this_run_;
    ++result_.turns;
    record_model_call(result_.model, reply);
    print_model_usage(output_, reply.usage);
    if (!reply.assistant_message.is_object()) {
        throw std::runtime_error("模型客户端返回了无效的 assistant message");
    }

    messages_.push_back(reply.assistant_message);
    pending_calls_.clear();
    for (auto& call : reply.tool_calls) {
        pending_calls_.push_back(std::move(call));
    }
    emit("model_completed", {{"turn", result_.turns},
                             {"tool_call_count", pending_calls_.size()},
                             {"has_text", !reply.text.empty()},
                             {"usage", model_usage_json(reply.usage)},
                             {"metadata", model_metadata_json(reply.metadata)}});
    save_checkpoint("running");
}

bool AgentRun::block_unverified_finish() {
    const auto status = verification_status(result_.execution, tools_.has_workspace_changes());
    if (!options_.require_verification_after_write || status == "not_required" ||
        status == "passed") {
        return false;
    }

    output_ << "\n[继续] 工作区变更尚未通过最新验证（" << status << "），拒绝结束任务。\n";
    messages_.push_back(
        {{"role", "user"},
         {"content",
          "[Harness requirement] The workspace still has unverified changes. "
          "The current verification status is " +
              status +
              ". Continue the task: inspect the last command result, fix the files if needed, "
              "and run an approved verification command after the latest edit. "
              "Only provide a final answer after the latest command exits with code 0."}});
    emit("verification_blocked", {{"turn", result_.turns}, {"verification_status", status}});
    save_checkpoint("running");
    return true;
}

} // namespace mint::agent_detail
