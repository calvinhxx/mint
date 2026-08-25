#include "aiagent/application/agent.hpp"

#include "aiagent/infrastructure/event_log.hpp"
#include "aiagent/infrastructure/session_store.hpp"
#include "aiagent/runtime/task_control.hpp"
#include "aiagent/version.hpp"

#include "agent_support.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace aiagent {
namespace {

using agent_detail::capture_final_state;
using agent_detail::compact_context;
using agent_detail::event_arguments_summary;
using agent_detail::execution_from_json;
using agent_detail::execution_to_json;
using agent_detail::model_metadata_json;
using agent_detail::model_summary_from_json;
using agent_detail::model_summary_to_json;
using agent_detail::model_usage_json;
using agent_detail::print_final_state;
using agent_detail::print_model_usage;
using agent_detail::record_execution;
using agent_detail::record_model_call;
using agent_detail::safe_tool_result;
using agent_detail::tool_call_from_json;
using agent_detail::tool_call_to_json;
using agent_detail::verification_status;

Json checkpoint_document(const std::string& status, const std::string& user_request,
                         const Json& messages, const AgentResult& result,
                         const std::vector<ToolCall>& pending_calls,
                         const std::optional<ToolCall>& in_flight_call, const ToolRegistry& tools,
                         bool require_verification, std::size_t max_context_bytes) {
    Json pending = Json::array();
    for (const auto& call : pending_calls) {
        pending.push_back(tool_call_to_json(call));
    }
    return {{"schema_version", session_schema_version},
            {"status", status},
            {"workspace_root", tools.root().generic_string()},
            {"user_request", user_request},
            {"messages", messages},
            {"turns", result.turns},
            {"duration_ms", result.duration_ms},
            {"stop_reason", result.stop_reason.empty() ? Json(nullptr) : Json(result.stop_reason)},
            {"answer", result.answer},
            {"verification_status",
             verification_status(result.execution, tools.has_workspace_changes())},
            {"execution", execution_to_json(result.execution)},
            {"model", model_summary_to_json(result.model)},
            {"pending_tool_calls", std::move(pending)},
            {"in_flight_tool_call",
             in_flight_call.has_value() ? tool_call_to_json(*in_flight_call) : Json(nullptr)},
            {"change_journal", tools.workspace_change_state()},
            {"capabilities",
             {{"allow_write", tools.can_write()},
              {"allowed_write_paths", tools.allowed_write_paths()},
              {"allowed_programs", tools.allowed_programs()},
              {"command_recipes", tools.command_recipe_names()},
              {"policy_fingerprint", tools.policy_fingerprint()},
              {"approve_each_command", tools.requires_command_approval()},
              {"approve_each_changeset", tools.requires_change_set_approval()},
              {"require_verification", require_verification},
              {"command_sandboxed", tools.commands_are_os_sandboxed()},
              {"command_sandbox_backend", tools.command_sandbox_backend()},
              {"max_context_bytes", max_context_bytes}}}};
}

struct RestoredSession {
    std::string user_request;
    Json messages;
    AgentResult result;
    std::vector<ToolCall> pending_calls;
    bool recovered_in_flight = false;
};

RestoredSession restore_session(const Json& snapshot, ToolRegistry& tools,
                                bool require_verification, std::size_t max_context_bytes,
                                bool retry_in_flight_tool) {
    const auto schema_version = snapshot.is_object() ? snapshot.value("schema_version", 0) : 0;
    if (!snapshot.is_object() ||
        (schema_version != 2 && schema_version != session_schema_version)) {
        throw std::invalid_argument("会话快照 schema_version 不受支持");
    }
    const auto snapshot_status = snapshot.value("status", "");
    if (snapshot_status == "completed") {
        throw std::invalid_argument("该会话已经完成，不能再次恢复");
    }
    if (snapshot_status != "running" && snapshot_status != "max_turns" &&
        snapshot_status != "cancelled" && snapshot_status != "timed_out") {
        throw std::invalid_argument("会话快照终态无效");
    }
    if (snapshot.value("workspace_root", "") != tools.root().generic_string()) {
        throw std::invalid_argument("会话工作区与当前 --root 不一致");
    }
    if (!snapshot.contains("capabilities") || !snapshot.at("capabilities").is_object()) {
        throw std::invalid_argument("会话快照缺少能力策略");
    }
    const auto& capabilities = snapshot.at("capabilities");
    const bool recipe_capability_matches =
        capabilities.contains("command_recipes")
            ? capabilities.at("command_recipes") == Json(tools.command_recipe_names())
            : schema_version == 2 && tools.command_recipe_names().empty();
    const bool policy_capability_matches =
        capabilities.contains("policy_fingerprint")
            ? capabilities.value("policy_fingerprint", "") == tools.policy_fingerprint()
            : schema_version == 2 && tools.policy_fingerprint().empty();
    const bool changeset_approval_matches =
        capabilities.contains("approve_each_changeset")
            ? capabilities.value("approve_each_changeset", false) ==
                  tools.requires_change_set_approval()
            : schema_version == 2 && !tools.requires_change_set_approval();
    if (capabilities.value("allow_write", false) != tools.can_write() ||
        !capabilities.contains("allowed_write_paths") ||
        capabilities.at("allowed_write_paths") != Json(tools.allowed_write_paths()) ||
        capabilities.value("approve_each_command", false) != tools.requires_command_approval() ||
        capabilities.value("require_verification", false) != require_verification ||
        capabilities.value("command_sandboxed", false) != tools.commands_are_os_sandboxed() ||
        capabilities.value("command_sandbox_backend", "none") != tools.command_sandbox_backend() ||
        capabilities.value("max_context_bytes", std::size_t{0}) != max_context_bytes ||
        !capabilities.contains("allowed_programs") ||
        capabilities.at("allowed_programs") != Json(tools.allowed_programs()) ||
        !recipe_capability_matches || !policy_capability_matches || !changeset_approval_matches) {
        throw std::invalid_argument("恢复会话时必须使用与原任务相同的能力授权");
    }
    if (!snapshot.contains("user_request") || !snapshot.at("user_request").is_string() ||
        !snapshot.contains("messages") || !snapshot.at("messages").is_array() ||
        !snapshot.contains("turns") || !snapshot.at("turns").is_number_unsigned() ||
        !snapshot.contains("duration_ms") || !snapshot.at("duration_ms").is_number_integer() ||
        !snapshot.contains("execution") || !snapshot.contains("pending_tool_calls") ||
        !snapshot.at("pending_tool_calls").is_array() || !snapshot.contains("change_journal") ||
        (schema_version == session_schema_version &&
         (!snapshot.contains("in_flight_tool_call") || !snapshot.contains("model") ||
          !snapshot.at("model").is_object() || !snapshot.at("execution").is_object() ||
          !snapshot.at("execution").contains("recipe_calls") ||
          !snapshot.at("execution").contains("verification_commands") ||
          !snapshot.at("execution").contains("last_command_verification_eligible")))) {
        throw std::invalid_argument("会话快照缺少必需状态");
    }

    RestoredSession restored;
    restored.user_request = snapshot.at("user_request").get<std::string>();
    restored.messages = snapshot.at("messages");
    restored.result.turns = snapshot.at("turns").get<std::size_t>();
    restored.result.duration_ms = snapshot.at("duration_ms").get<long long>();
    if (restored.result.duration_ms < 0 || restored.messages.size() < 2) {
        throw std::invalid_argument("会话快照状态无效");
    }
    restored.result.execution = execution_from_json(snapshot.at("execution"));
    if (snapshot.contains("model")) {
        restored.result.model = model_summary_from_json(snapshot.at("model"));
    }
    for (const auto& call : snapshot.at("pending_tool_calls")) {
        restored.pending_calls.push_back(tool_call_from_json(call));
    }
    if (schema_version == session_schema_version && snapshot.contains("in_flight_tool_call") &&
        !snapshot.at("in_flight_tool_call").is_null()) {
        const auto in_flight = tool_call_from_json(snapshot.at("in_flight_tool_call"));
        if (restored.pending_calls.empty() || restored.pending_calls.front().id != in_flight.id ||
            restored.pending_calls.front().name != in_flight.name ||
            restored.pending_calls.front().arguments != in_flight.arguments) {
            throw std::invalid_argument("会话中的 in-flight 工具与待执行队列不一致");
        }
        const bool read_only_retry =
            in_flight.name == "list_files" || in_flight.name == "read_file" ||
            in_flight.name == "search_text" || in_flight.name == "workspace_changes";
        if (!read_only_retry && !retry_in_flight_tool) {
            throw std::runtime_error("检查点记录到未确认完成的 in-flight 工具 " + in_flight.name +
                                     " (" + in_flight.id +
                                     ")；默认拒绝重复副作用。"
                                     "检查工作区后，只有明确接受重试风险时才使用 --retry-inflight");
        }
        restored.recovered_in_flight = true;
    }
    tools.restore_workspace_change_state(snapshot.at("change_journal"));
    return restored;
}

} // namespace

Agent::Agent(ModelClient& model, ToolRegistry& tools, std::ostream& output, AgentOptions options)
    : model_(model), tools_(tools), output_(output), options_(std::move(options)) {
    if (options_.max_turns == 0) {
        throw std::invalid_argument("max_turns 必须大于 0");
    }
    if (options_.max_context_bytes < 16 * 1024 || options_.max_context_bytes > 8 * 1024 * 1024) {
        throw std::invalid_argument("max_context_bytes 必须在 16 KiB 到 8 MiB 之间");
    }
    if (options_.require_verification_after_write &&
        (!tools_.can_write() || !tools_.can_run_commands())) {
        throw std::invalid_argument(
            "require_verification_after_write 需要同时启用写工具和至少一个命令程序");
    }
    if (options_.resume_session && options_.session_store == nullptr) {
        throw std::invalid_argument("resume_session 需要会话存储");
    }
    if (options_.retry_in_flight_tool && !options_.resume_session) {
        throw std::invalid_argument("retry_in_flight_tool 只允许用于恢复会话");
    }
}

AgentResult Agent::run(const std::string& requested_task) {
    const auto invocation_started = std::chrono::steady_clock::now();
    Json messages;
    AgentResult result;
    std::vector<ToolCall> pending_calls;
    std::optional<ToolCall> in_flight_call;
    bool recovered_in_flight = false;
    std::string user_request = requested_task;

    if (options_.resume_session) {
        if (!requested_task.empty()) {
            throw std::invalid_argument("恢复会话时不能同时提供新的任务内容");
        }
        if (!options_.session_store->exists()) {
            throw std::invalid_argument("找不到要恢复的会话快照");
        }
        auto restored = restore_session(options_.session_store->load(), tools_,
                                        options_.require_verification_after_write,
                                        options_.max_context_bytes, options_.retry_in_flight_tool);
        user_request = std::move(restored.user_request);
        messages = std::move(restored.messages);
        result = std::move(restored.result);
        pending_calls = std::move(restored.pending_calls);
        recovered_in_flight = restored.recovered_in_flight;
    } else {
        if (user_request.empty()) {
            throw std::invalid_argument("任务内容不能为空");
        }
        messages = Json::array({{{"role", "system"}, {"content", system_prompt()}},
                                {{"role", "user"}, {"content", user_request}}});
    }

    const auto previous_duration = result.duration_ms;
    const auto update_duration = [&]() {
        result.duration_ms =
            previous_duration + std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - invocation_started)
                                    .count();
    };
    const auto save_checkpoint = [&](const std::string& status) {
        update_duration();
        if (options_.session_store != nullptr) {
            options_.session_store->save(checkpoint_document(
                status, user_request, messages, result, pending_calls, in_flight_call, tools_,
                options_.require_verification_after_write, options_.max_context_bytes));
        }
    };
    const auto emit = [&](std::string type, Json data = Json::object()) {
        if (options_.event_log != nullptr) {
            options_.event_log->emit(std::move(type), std::move(data));
        }
    };

    if (!options_.resume_session) {
        save_checkpoint("running");
    }
    emit("task_started", {{"workspace_root", tools_.root().generic_string()},
                          {"resumed", options_.resume_session},
                          {"recovered_in_flight", recovered_in_flight},
                          {"retry_in_flight_authorized", options_.retry_in_flight_tool},
                          {"previous_turns", result.turns},
                          {"max_turns_this_run", options_.max_turns},
                          {"max_context_bytes", options_.max_context_bytes},
                          {"max_context_estimated_tokens", (options_.max_context_bytes + 3) / 4},
                          {"require_verification", options_.require_verification_after_write}});

    const auto finish = [&](std::string status, std::string reason,
                            std::string answer) -> AgentResult {
        result.status = std::move(status);
        result.completed = result.status == "completed";
        result.stop_reason = std::move(reason);
        result.answer = std::move(answer);
        update_duration();
        capture_final_state(result, tools_);
        save_checkpoint(result.status);
        emit(
            "task_finished",
            {{"status", result.status},
             {"stop_reason", result.stop_reason.empty() ? Json(nullptr) : Json(result.stop_reason)},
             {"turns", result.turns},
             {"duration_ms", result.duration_ms},
             {"verification_status", result.verification_status},
             {"model", model_summary_to_json(result.model)},
             {"changed_file_count", result.changes.files.size()}});
        if (result.completed) {
            output_ << "\n[最终回答]\n" << result.answer << '\n';
        } else {
            output_ << "\n[停止] " << result.answer << '\n';
        }
        print_final_state(output_, result);
        return result;
    };

    const auto stop_if_requested = [&]() -> std::string {
        if (options_.task_control == nullptr) {
            return {};
        }
        return options_.task_control->stop_reason();
    };
    const auto finish_stopped = [&](const std::string& reason) {
        return finish(reason == "cancelled" ? "cancelled" : "timed_out",
                      reason == "cancelled" ? "user_cancelled" : "total_budget_exhausted",
                      reason == "cancelled" ? "任务已取消。" : "任务超过总时间预算。");
    };

    const auto tool_definitions = tools_.definitions();
    std::size_t model_calls_this_run = 0;
    while (true) {
        if (const auto reason = stop_if_requested(); !reason.empty()) {
            return finish_stopped(reason);
        }

        if (!pending_calls.empty()) {
            const auto call = pending_calls.front();
            in_flight_call = call;
            save_checkpoint("running");
            emit("tool_started", {{"turn", result.turns},
                                  {"tool_call_id", call.id},
                                  {"name", call.name},
                                  {"arguments_summary", event_arguments_summary(tools_, call)}});
            output_ << "[调用工具] " << call.name << ' ' << tools_.describe_call(call) << '\n';
            const auto tool_result = tools_.execute(call);
            record_execution(result.execution, call, tool_result);
            messages.push_back(
                {{"role", "tool"}, {"tool_call_id", call.id}, {"content", tool_result}});
            pending_calls.erase(pending_calls.begin());
            in_flight_call.reset();
            emit("tool_completed", {{"turn", result.turns},
                                    {"tool_call_id", call.id},
                                    {"name", call.name},
                                    {"result", safe_tool_result(tool_result)}});
            output_ << "[工具完成] 结果已交回模型\n";
            save_checkpoint("running");
            continue;
        }

        if (model_calls_this_run >= options_.max_turns) {
            return finish("max_turns", "max_turns_exhausted",
                          "达到本次运行的最大轮数，任务尚未完成；可以使用会话快照继续。");
        }

        const auto turn = result.turns + 1;
        output_ << "\n[第 " << turn << " 轮] 询问模型下一步...\n";
        const auto model_context = compact_context(messages, options_.max_context_bytes);
        if (model_context.dropped_groups != 0 || model_context.payloads_compacted) {
            emit("context_compacted",
                 {{"turn", turn},
                  {"full_bytes", model_context.full_bytes},
                  {"sent_bytes", model_context.sent_bytes},
                  {"full_estimated_tokens", model_context.full_estimated_tokens()},
                  {"sent_estimated_tokens", model_context.sent_estimated_tokens()},
                  {"dropped_groups", model_context.dropped_groups},
                  {"payloads_compacted", model_context.payloads_compacted}});
        }
        emit("model_requested",
             {{"turn", turn},
              {"message_count", model_context.messages.size()},
              {"full_context_bytes", model_context.full_bytes},
              {"sent_context_bytes", model_context.sent_bytes},
              {"full_context_estimated_tokens", model_context.full_estimated_tokens()},
              {"sent_context_estimated_tokens", model_context.sent_estimated_tokens()}});

        ModelReply reply;
        try {
            reply = model_.complete(model_context.messages, tool_definitions);
        } catch (...) {
            if (!stop_if_requested().empty()) {
                const auto reason = stop_if_requested();
                return finish_stopped(reason);
            }
            throw;
        }
        ++model_calls_this_run;
        ++result.turns;
        record_model_call(result.model, reply);
        print_model_usage(output_, reply.usage);

        if (!reply.assistant_message.is_object()) {
            throw std::runtime_error("模型客户端返回了无效的 assistant message");
        }
        messages.push_back(reply.assistant_message);
        pending_calls = std::move(reply.tool_calls);
        emit("model_completed", {{"turn", result.turns},
                                 {"tool_call_count", pending_calls.size()},
                                 {"has_text", !reply.text.empty()},
                                 {"usage", model_usage_json(reply.usage)},
                                 {"metadata", model_metadata_json(reply.metadata)}});
        save_checkpoint("running");

        if (const auto reason = stop_if_requested(); !reason.empty()) {
            return finish_stopped(reason);
        }

        if (!pending_calls.empty()) {
            continue;
        }
        if (reply.text.empty()) {
            throw std::runtime_error("模型既没有回答，也没有调用工具");
        }

        const auto current_verification =
            verification_status(result.execution, tools_.has_workspace_changes());
        if (options_.require_verification_after_write && current_verification != "not_required" &&
            current_verification != "passed") {
            output_ << "\n[继续] 工作区变更尚未通过最新验证（" << current_verification
                    << "），拒绝结束任务。\n";
            messages.push_back(
                {{"role", "user"},
                 {"content",
                  "[Harness requirement] The workspace still has unverified changes. "
                  "The current verification status is " +
                      current_verification +
                      ". "
                      "Continue the task: inspect the last command result, fix the files if "
                      "needed, "
                      "and run an approved verification command after the latest edit. "
                      "Only provide a final answer after the latest command exits with code 0."}});
            emit("verification_blocked",
                 {{"turn", result.turns}, {"verification_status", current_verification}});
            save_checkpoint("running");
            continue;
        }
        return finish("completed", {}, std::move(reply.text));
    }
}

} // namespace aiagent
