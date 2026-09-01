#include "agent_execution.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace mint::agent_detail {
namespace {

std::size_t required_size(const Json& object, const char* field) {
    if (!object.contains(field) || !object.at(field).is_number_unsigned()) {
        throw std::invalid_argument("会话执行摘要字段无效: " + std::string(field));
    }
    return object.at(field).get<std::size_t>();
}

} // namespace

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
        if (is_command && result.value("workspace_changed", false)) {
            ++summary.file_changes;
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

} // namespace mint::agent_detail
