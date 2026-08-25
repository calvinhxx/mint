#include "agent_support.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace aiagent::agent_detail {
namespace {

std::size_t required_size(const Json& object, const char* field) {
    if (!object.contains(field) || !object.at(field).is_number_unsigned()) {
        throw std::invalid_argument("会话执行摘要字段无效: " + std::string(field));
    }
    return object.at(field).get<std::size_t>();
}

void print_execution_summary(std::ostream& output, const ExecutionSummary& summary) {
    if (summary.tool_calls == 0) {
        return;
    }
    output << "\n[执行摘要] 工具 " << summary.tool_calls << " 次，成功 "
           << summary.successful_tool_calls << "，错误 " << summary.tool_errors << "；文件修改 "
           << summary.file_changes << " 次；命令 " << summary.command_calls << " 次（通过 "
           << summary.commands_passed << "，失败 " << summary.commands_failed << "，超时 "
           << summary.commands_timed_out << "，取消 " << summary.commands_cancelled << "，拒绝 "
           << summary.commands_denied << "）\n";
}

} // namespace

Json model_usage_json(const ModelUsage& usage) {
    if (!usage.available) {
        return nullptr;
    }
    return {{"prompt_tokens", usage.prompt_tokens},
            {"completion_tokens", usage.completion_tokens},
            {"total_tokens", usage.total_tokens},
            {"cached_tokens", usage.cached_tokens}};
}

Json model_metadata_json(const ModelCallMetadata& metadata) {
    return {
        {"adapter", metadata.adapter},
        {"response_id", metadata.response_id.empty() ? Json(nullptr) : Json(metadata.response_id)},
        {"model", metadata.model},
        {"attempts", metadata.attempts},
        {"retries", metadata.retries},
        {"http_status", metadata.http_status},
        {"duration_ms", metadata.duration_ms},
        {"streamed", metadata.streamed},
        {"stream_events", metadata.stream_events},
        {"streamed_bytes", metadata.streamed_bytes}};
}

void record_model_call(ModelSummary& summary, const ModelReply& reply) {
    ++summary.calls;
    summary.attempts += reply.metadata.attempts;
    summary.retries += reply.metadata.retries;
    summary.duration_ms += reply.metadata.duration_ms;
    if (reply.metadata.streamed) {
        ++summary.streamed_calls;
        summary.stream_events += reply.metadata.stream_events;
        summary.streamed_bytes += reply.metadata.streamed_bytes;
    }
    if (!reply.metadata.adapter.empty()) {
        summary.adapter = reply.metadata.adapter;
    }
    if (!reply.metadata.model.empty()) {
        summary.model = reply.metadata.model;
    }
    if (!reply.metadata.response_id.empty()) {
        summary.last_response_id = reply.metadata.response_id;
    }
    if (reply.usage.available) {
        ++summary.usage_reports;
        summary.prompt_tokens += reply.usage.prompt_tokens;
        summary.completion_tokens += reply.usage.completion_tokens;
        summary.total_tokens += reply.usage.total_tokens;
        summary.cached_tokens += reply.usage.cached_tokens;
    }
}

Json model_summary_to_json(const ModelSummary& summary) {
    return {{"calls", summary.calls},
            {"attempts", summary.attempts},
            {"retries", summary.retries},
            {"usage_reports", summary.usage_reports},
            {"prompt_tokens", summary.prompt_tokens},
            {"completion_tokens", summary.completion_tokens},
            {"total_tokens", summary.total_tokens},
            {"cached_tokens", summary.cached_tokens},
            {"streamed_calls", summary.streamed_calls},
            {"stream_events", summary.stream_events},
            {"streamed_bytes", summary.streamed_bytes},
            {"duration_ms", summary.duration_ms},
            {"adapter", summary.adapter},
            {"model", summary.model},
            {"last_response_id",
             summary.last_response_id.empty() ? Json(nullptr) : Json(summary.last_response_id)}};
}

ModelSummary model_summary_from_json(const Json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("会话模型摘要格式无效");
    }
    ModelSummary summary;
    const auto read_size = [&](const char* field) {
        if (!value.contains(field) || !value.at(field).is_number_unsigned()) {
            throw std::invalid_argument("会话模型摘要字段无效: " + std::string(field));
        }
        return value.at(field).get<std::size_t>();
    };
    summary.calls = read_size("calls");
    summary.attempts = read_size("attempts");
    summary.retries = read_size("retries");
    summary.usage_reports = read_size("usage_reports");
    summary.prompt_tokens = read_size("prompt_tokens");
    summary.completion_tokens = read_size("completion_tokens");
    summary.total_tokens = read_size("total_tokens");
    summary.cached_tokens = read_size("cached_tokens");
    const auto read_optional_size = [&](const char* field) {
        if (!value.contains(field)) {
            return std::size_t{0};
        }
        if (!value.at(field).is_number_unsigned()) {
            throw std::invalid_argument("会话模型摘要字段无效: " + std::string(field));
        }
        return value.at(field).get<std::size_t>();
    };
    summary.streamed_calls = read_optional_size("streamed_calls");
    summary.stream_events = read_optional_size("stream_events");
    summary.streamed_bytes = read_optional_size("streamed_bytes");
    if (!value.contains("duration_ms") || !value.at("duration_ms").is_number_integer() ||
        !value.contains("adapter") || !value.at("adapter").is_string() ||
        !value.contains("model") || !value.at("model").is_string()) {
        throw std::invalid_argument("会话模型摘要标识或耗时无效");
    }
    summary.duration_ms = value.at("duration_ms").get<long long>();
    summary.adapter = value.at("adapter").get<std::string>();
    summary.model = value.at("model").get<std::string>();
    if (value.contains("last_response_id") && value.at("last_response_id").is_string()) {
        summary.last_response_id = value.at("last_response_id").get<std::string>();
    } else if (!value.contains("last_response_id") || !value.at("last_response_id").is_null()) {
        throw std::invalid_argument("会话模型摘要 response id 无效");
    }
    if (summary.attempts < summary.calls || summary.retries > summary.attempts ||
        summary.usage_reports > summary.calls || summary.duration_ms < 0 ||
        summary.cached_tokens > summary.prompt_tokens || summary.streamed_calls > summary.calls) {
        throw std::invalid_argument("会话模型摘要计数不一致");
    }
    return summary;
}

void print_model_usage(std::ostream& output, const ModelUsage& usage) {
    if (!usage.available) {
        return;
    }
    output << "[Token] 输入 " << usage.prompt_tokens;
    if (usage.prompt_tokens != 0) {
        const auto hit_rate = usage.cached_tokens * 100 / usage.prompt_tokens;
        output << "（缓存 " << usage.cached_tokens << "，命中 " << hit_rate << "%）";
    }
    output << "，输出 " << usage.completion_tokens << "，合计 " << usage.total_tokens << '\n';
}

void record_execution(ExecutionSummary& summary, const ToolCall& call,
                      const std::string& raw_result) {
    ++summary.tool_calls;
    const bool is_command = call.name == "run_command" || call.name == "run_recipe";
    if (is_command) {
        ++summary.command_calls;
        if (call.name == "run_recipe") {
            ++summary.recipe_calls;
        }
    }

    try {
        const auto result = Json::parse(raw_result);
        const bool ok = result.is_object() && result.value("ok", false);
        if (ok) {
            ++summary.successful_tool_calls;
        } else {
            ++summary.tool_errors;
        }
        if ((call.name == "apply_patch" || call.name == "apply_changeset") && ok) {
            summary.file_changes += call.name == "apply_changeset"
                                        ? result.value("operation_count", std::size_t{1})
                                        : 1;
            summary.last_file_change_call = summary.tool_calls;
        }
        if (!is_command) {
            return;
        }

        summary.last_command_call = summary.tool_calls;
        summary.last_command_verification_eligible =
            result.value("verification_eligible", call.name == "run_command");
        if (summary.last_command_verification_eligible) {
            ++summary.verification_commands;
        }
        const auto status = result.value("status", "");
        if (status == "denied") {
            ++summary.commands_denied;
            summary.last_command_outcome = "denied";
        } else if (status == "cancelled" || result.value("cancelled", false)) {
            ++summary.commands_cancelled;
            summary.last_command_outcome = "cancelled";
        } else if (status == "task_timed_out" || result.value("task_timed_out", false) ||
                   result.value("timed_out", false)) {
            ++summary.commands_timed_out;
            summary.last_command_outcome = "timed_out";
        } else if (!ok) {
            ++summary.commands_failed;
            summary.last_command_outcome = "failed";
        } else if (result.contains("exit_code") && result.at("exit_code").is_number_integer() &&
                   result.at("exit_code").get<int>() == 0) {
            ++summary.commands_passed;
            summary.last_command_outcome = "passed";
        } else {
            ++summary.commands_failed;
            summary.last_command_outcome = "failed";
        }
    } catch (const Json::exception&) {
        ++summary.tool_errors;
        if (is_command) {
            ++summary.commands_failed;
            summary.last_command_call = summary.tool_calls;
            summary.last_command_outcome = "failed";
            summary.last_command_verification_eligible = false;
        }
    }
}

std::string verification_status(const ExecutionSummary& summary, bool has_changes) {
    if (!has_changes) {
        return "not_required";
    }
    if (summary.last_command_call > summary.last_file_change_call &&
        summary.last_command_verification_eligible) {
        return summary.last_command_outcome;
    }
    return "not_run";
}

Json execution_to_json(const ExecutionSummary& summary) {
    return {{"tool_calls", summary.tool_calls},
            {"successful_tool_calls", summary.successful_tool_calls},
            {"tool_errors", summary.tool_errors},
            {"file_changes", summary.file_changes},
            {"command_calls", summary.command_calls},
            {"recipe_calls", summary.recipe_calls},
            {"verification_commands", summary.verification_commands},
            {"commands_passed", summary.commands_passed},
            {"commands_failed", summary.commands_failed},
            {"commands_timed_out", summary.commands_timed_out},
            {"commands_cancelled", summary.commands_cancelled},
            {"commands_denied", summary.commands_denied},
            {"last_file_change_call", summary.last_file_change_call},
            {"last_command_call", summary.last_command_call},
            {"last_command_outcome", summary.last_command_outcome},
            {"last_command_verification_eligible", summary.last_command_verification_eligible}};
}

ExecutionSummary execution_from_json(const Json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("会话执行摘要格式无效");
    }
    ExecutionSummary result;
    result.tool_calls = required_size(value, "tool_calls");
    result.successful_tool_calls = required_size(value, "successful_tool_calls");
    result.tool_errors = required_size(value, "tool_errors");
    result.file_changes = required_size(value, "file_changes");
    result.command_calls = required_size(value, "command_calls");
    result.recipe_calls = value.contains("recipe_calls") ? required_size(value, "recipe_calls") : 0;
    result.verification_commands = value.contains("verification_commands")
                                       ? required_size(value, "verification_commands")
                                       : result.command_calls;
    result.commands_passed = required_size(value, "commands_passed");
    result.commands_failed = required_size(value, "commands_failed");
    result.commands_timed_out = required_size(value, "commands_timed_out");
    result.commands_cancelled = required_size(value, "commands_cancelled");
    result.commands_denied = required_size(value, "commands_denied");
    result.last_file_change_call = required_size(value, "last_file_change_call");
    result.last_command_call = required_size(value, "last_command_call");
    if (!value.contains("last_command_outcome") || !value.at("last_command_outcome").is_string()) {
        throw std::invalid_argument("会话执行摘要缺少 last_command_outcome");
    }
    result.last_command_outcome = value.at("last_command_outcome").get<std::string>();
    if (value.contains("last_command_verification_eligible")) {
        if (!value.at("last_command_verification_eligible").is_boolean()) {
            throw std::invalid_argument("会话执行摘要 last_command_verification_eligible 无效");
        }
        result.last_command_verification_eligible =
            value.at("last_command_verification_eligible").get<bool>();
    } else {
        result.last_command_verification_eligible = result.last_command_call != 0;
    }
    static const std::vector<std::string> allowed_outcomes = {"not_run",   "passed",    "failed",
                                                              "timed_out", "cancelled", "denied"};
    const auto command_outcomes = result.commands_passed + result.commands_failed +
                                  result.commands_timed_out + result.commands_cancelled +
                                  result.commands_denied;
    if (result.successful_tool_calls + result.tool_errors != result.tool_calls ||
        result.file_changes > result.tool_calls || result.command_calls > result.tool_calls ||
        result.recipe_calls > result.command_calls ||
        result.verification_commands > result.command_calls ||
        command_outcomes != result.command_calls ||
        result.last_file_change_call > result.tool_calls ||
        result.last_command_call > result.tool_calls ||
        std::find(allowed_outcomes.begin(), allowed_outcomes.end(), result.last_command_outcome) ==
            allowed_outcomes.end() ||
        (result.last_command_call == 0 && result.last_command_outcome != "not_run") ||
        (result.last_command_call != 0 && result.last_command_outcome == "not_run") ||
        (result.command_calls == 0 && result.last_command_call != 0) ||
        (result.command_calls != 0 && result.last_command_call == 0) ||
        (result.file_changes == 0 && result.last_file_change_call != 0) ||
        (result.file_changes != 0 && result.last_file_change_call == 0)) {
        throw std::invalid_argument("会话执行摘要计数不一致");
    }
    return result;
}

Json tool_call_to_json(const ToolCall& call) {
    return {{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}};
}

ToolCall tool_call_from_json(const Json& value) {
    if (!value.is_object() || !value.contains("id") || !value.at("id").is_string() ||
        !value.contains("name") || !value.at("name").is_string() || !value.contains("arguments") ||
        !value.at("arguments").is_object()) {
        throw std::invalid_argument("会话中的待执行工具调用格式无效");
    }
    return {value.at("id").get<std::string>(), value.at("name").get<std::string>(),
            value.at("arguments")};
}

void capture_final_state(AgentResult& result, const ToolRegistry& tools) {
    result.changes.files.clear();
    const auto snapshot = tools.workspace_change_snapshot();
    if (snapshot.contains("changed_files") && snapshot.at("changed_files").is_array()) {
        for (const auto& file : snapshot.at("changed_files")) {
            if (file.is_object() && file.contains("path") && file.at("path").is_string()) {
                result.changes.files.push_back(file.at("path").get<std::string>());
            }
        }
    }
    result.changes.unified_diff = snapshot.value("diff", "");
    result.changes.diff_truncated = snapshot.value("diff_truncated", false);
    result.verification_status =
        verification_status(result.execution, !result.changes.files.empty());
}

void print_final_state(std::ostream& output, const AgentResult& result) {
    print_execution_summary(output, result.execution);
    if (result.model.calls != 0) {
        output << "[模型摘要] 调用 " << result.model.calls << " 次，HTTP 尝试 "
               << result.model.attempts << "，重试 " << result.model.retries << "，耗时 "
               << result.model.duration_ms << "ms";
        if (result.model.usage_reports != 0) {
            output << "，tokens " << result.model.total_tokens;
        }
        output << '\n';
    }
    output << "[任务状态] " << result.status << '\n'
           << "[验证状态] " << result.verification_status << '\n';
    if (result.changes.files.empty()) {
        return;
    }
    output << "[工作区变更] " << result.changes.files.size() << " 个文件";
    if (result.changes.diff_truncated) {
        output << "（diff 已截断）";
    }
    output << '\n' << result.changes.unified_diff;
    if (!result.changes.unified_diff.empty() && result.changes.unified_diff.back() != '\n') {
        output << '\n';
    }
}

Json safe_tool_result(const std::string& raw_result) {
    Json summary = Json::object();
    try {
        const auto result = Json::parse(raw_result);
        static const std::vector<std::string> fields = {"ok",
                                                        "status",
                                                        "exit_code",
                                                        "signal",
                                                        "timed_out",
                                                        "task_timed_out",
                                                        "cancelled",
                                                        "duration_ms",
                                                        "output_truncated",
                                                        "sandboxed",
                                                        "sandbox_backend",
                                                        "recipe",
                                                        "verification_eligible",
                                                        "operation",
                                                        "path",
                                                        "bytes_before",
                                                        "bytes_after",
                                                        "operation_count",
                                                        "rollback_performed"};
        for (const auto& field : fields) {
            if (result.contains(field)) {
                summary[field] = result.at(field);
            }
        }
        if (result.contains("changed_files") && result.at("changed_files").is_array()) {
            summary["changed_file_count"] = result.at("changed_files").size();
        }
    } catch (const Json::exception&) {
        summary["parse_error"] = true;
    }
    return summary;
}

Json event_arguments_summary(const ToolRegistry& tools, const ToolCall& call) {
    if (call.name == "run_command" || call.name == "run_recipe" || call.name == "apply_patch" ||
        call.name == "apply_changeset") {
        return Json::parse(tools.describe_call(call));
    }

    Json summary = Json::object();
    if (!call.arguments.is_object()) {
        return summary;
    }
    if (call.arguments.contains("path") && call.arguments.at("path").is_string()) {
        summary["path"] = call.arguments.at("path");
    }
    if (call.name == "list_files" && call.arguments.contains("max_depth") &&
        call.arguments.at("max_depth").is_number_integer()) {
        summary["max_depth"] = call.arguments.at("max_depth");
    }
    if (call.name == "search_text") {
        if (call.arguments.contains("query") && call.arguments.at("query").is_string()) {
            summary["query_bytes"] =
                call.arguments.at("query").get_ref<const std::string&>().size();
        }
        if (call.arguments.contains("case_sensitive") &&
            call.arguments.at("case_sensitive").is_boolean()) {
            summary["case_sensitive"] = call.arguments.at("case_sensitive");
        }
    }
    return summary;
}

} // namespace aiagent::agent_detail

namespace aiagent {

Json agent_result_to_json(const AgentResult& result) {
    return {{"schema_version", 1},
            {"status", result.status},
            {"completed", result.completed},
            {"stop_reason", result.stop_reason.empty() ? Json(nullptr) : Json(result.stop_reason)},
            {"answer", result.answer},
            {"turns", result.turns},
            {"duration_ms", result.duration_ms},
            {"verification_status", result.verification_status},
            {"execution", agent_detail::execution_to_json(result.execution)},
            {"model", agent_detail::model_summary_to_json(result.model)},
            {"changes",
             {{"files", result.changes.files},
              {"unified_diff", result.changes.unified_diff},
              {"diff_truncated", result.changes.diff_truncated}}}};
}

} // namespace aiagent
