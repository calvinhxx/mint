#include "agent_reporting.hpp"

#include "agent_execution.hpp"
#include "agent_model_summary.hpp"

#include "mint/runtime/terminal_text.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace mint::agent_detail {
namespace {

constexpr std::size_t persisted_error_limit = 512;
constexpr std::string_view persisted_error_suffix = "…[已截断]";

void truncate_persisted_error(std::string& value) {
    if (value.size() <= persisted_error_limit) {
        return;
    }
    auto boundary = persisted_error_limit - persisted_error_suffix.size();
    while (boundary > 0 && (static_cast<unsigned char>(value[boundary]) & 0xC0U) == 0x80U) {
        --boundary;
    }
    value.resize(boundary);
    value += persisted_error_suffix;
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

void capture_final_state(AgentResult& result, const AgentTools& tools) {
    result.changes.files.clear();
    result.changes.details.clear();
    const auto snapshot = tools.workspace_change_snapshot();
    if (snapshot.contains("changed_files") && snapshot.at("changed_files").is_array()) {
        for (const auto& file : snapshot.at("changed_files")) {
            if (file.is_object() && file.contains("path") && file.at("path").is_string()) {
                auto path = file.at("path").get<std::string>();
                result.changes.files.push_back(path);
                result.changes.details.push_back(
                    {.path = std::move(path), .status = file.value("status", "modified")});
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
            output << "，tokens " << result.model.total_tokens << "，缓存 "
                   << result.model.cached_tokens;
            const auto hit_rate =
                model_usage::cache_hit_rate(result.model.cached_tokens, result.model.prompt_tokens);
            output << "，命中 ";
            if (hit_rate.has_value()) {
                output << static_cast<std::size_t>(*hit_rate * 100.0) << '%';
            } else {
                output << "n/a";
            }
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
    output << '\n';
    for (const auto& file : result.changes.details) {
        if (file.status == "policy_violation" || file.status == "unauditable") {
            output << "[工作区风险] " << escape_terminal_field(file.path) << "："
                   << (file.status == "policy_violation" ? "超出写入策略" : "无法安全审计") << '\n';
        }
    }
    output << escape_terminal_text(result.changes.unified_diff);
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
                                                        "resource_limited",
                                                        "resource_limit",
                                                        "resource_limits",
                                                        "duration_ms",
                                                        "output_truncated",
                                                        "sandboxed",
                                                        "sandbox_backend",
                                                        "recipe",
                                                        "verification_eligible",
                                                        "workspace_changed",
                                                        "approval_decision_source",
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
        if (const auto error = result.find("error"); error != result.end() && error->is_string()) {
            auto error_text = error->get<std::string>();
            truncate_persisted_error(error_text);
            summary["error"] = std::move(error_text);
        }
    } catch (const Json::exception&) {
        summary["parse_error"] = true;
    }
    return summary;
}

Json event_arguments_summary(const AgentTools& tools, const ToolCall& call) {
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

} // namespace mint::agent_detail

namespace mint {

Json agent_result_to_json(const AgentResult& result) {
    Json change_details = Json::array();
    for (const auto& file : result.changes.details) {
        change_details.push_back({{"path", file.path}, {"status", file.status}});
    }
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
              {"details", std::move(change_details)},
              {"unified_diff", result.changes.unified_diff},
              {"diff_truncated", result.changes.diff_truncated}}}};
}

} // namespace mint
