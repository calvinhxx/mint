#include "agent_reporting.hpp"

#include "agent_execution.hpp"
#include "agent_model_summary.hpp"

#include "mint/localization/localization.hpp"
#include "mint/runtime/terminal_text.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace mint::agent_detail {
namespace {

constexpr std::size_t persisted_error_limit = 512;

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

void truncate_persisted_error(std::string& value) {
    if (value.size() <= persisted_error_limit) {
        return;
    }
    const auto persisted_error_suffix = message(Message::agent_output_truncated_suffix);
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
    output << message(Message::agent_output_execution_summary,
                      {arg(Placeholder::tool_calls, summary.tool_calls),
                       arg(Placeholder::successful_tools, summary.successful_tool_calls),
                       arg(Placeholder::tool_errors, summary.tool_errors),
                       arg(Placeholder::file_changes, summary.file_changes),
                       arg(Placeholder::command_calls, summary.command_calls),
                       arg(Placeholder::commands_passed, summary.commands_passed),
                       arg(Placeholder::commands_failed, summary.commands_failed),
                       arg(Placeholder::commands_timed_out, summary.commands_timed_out),
                       arg(Placeholder::commands_cancelled, summary.commands_cancelled),
                       arg(Placeholder::commands_denied, summary.commands_denied)});
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
        output << message(Message::agent_output_model_summary,
                          {arg(Placeholder::calls, result.model.calls),
                           arg(Placeholder::attempts, result.model.attempts),
                           arg(Placeholder::retries, result.model.retries),
                           arg(Placeholder::duration_ms, result.model.duration_ms)});
        if (result.model.usage_reports != 0) {
            const auto hit_rate =
                model_usage::cache_hit_rate(result.model.cached_tokens, result.model.prompt_tokens);
            const auto hit_rate_text =
                hit_rate.has_value()
                    ? std::to_string(static_cast<std::size_t>(*hit_rate * 100.0)) + "%"
                    : std::string("n/a");
            output << message(Message::agent_output_model_usage_summary,
                              {arg(Placeholder::tokens, result.model.total_tokens),
                               arg(Placeholder::cached, result.model.cached_tokens),
                               arg(Placeholder::hit_rate, hit_rate_text)});
        }
        output << '\n';
    }
    if (result.model.max_total_tokens != 0) {
        const auto budget = token_budget_to_json(result.model);
        output << message(Message::agent_output_token_budget,
                          {arg(Placeholder::reported, result.model.total_tokens),
                           arg(Placeholder::maximum, result.model.max_total_tokens)});
        const auto coverage = budget.at("usage_coverage").get<std::string>();
        if (coverage == "unavailable") {
            output << message(Message::agent_output_token_budget_unavailable);
        } else if (coverage == "partial") {
            output << message(Message::agent_output_token_budget_partial);
        }
        output << '\n';
    }
    output << message(Message::agent_output_status,
                      {arg(Placeholder::task, result.status),
                       arg(Placeholder::verification, result.verification_status)});
    if (result.changes.files.empty()) {
        return;
    }
    output << message(Message::agent_output_workspace_changes,
                      {arg(Placeholder::count, result.changes.files.size())});
    if (result.changes.diff_truncated) {
        output << message(Message::agent_output_diff_truncated);
    }
    output << '\n';
    for (const auto& file : result.changes.details) {
        if (file.status == "policy_violation" || file.status == "unauditable") {
            output << message(
                Message::agent_output_workspace_risk,
                {arg(Placeholder::path, escape_terminal_field(file.path)),
                 arg(Placeholder::risk, message(file.status == "policy_violation"
                                                    ? Message::agent_output_risk_policy
                                                    : Message::agent_output_risk_unauditable))});
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
