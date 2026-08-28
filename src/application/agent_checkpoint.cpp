#include "agent_checkpoint.hpp"

#include "agent_support.hpp"

#include "mint/infrastructure/session_store.hpp"
#include "mint/version.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mint::agent_detail {
namespace {

bool valid_session_status(std::string_view status) {
    return status == "running" || status == "max_turns" || status == "cancelled" ||
           status == "timed_out";
}

bool supported_session_schema(int schema_version) {
    return schema_version == 2 || schema_version == 3 || schema_version == session_schema_version;
}

bool valid_change_transaction_id(std::string_view id) {
    return !id.empty() && id.size() <= 128 &&
           std::all_of(id.begin(), id.end(), [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' || character == '_';
           });
}

bool capabilities_match(const Json& capabilities, const ToolRegistry& tools,
                        bool require_verification, std::size_t max_context_bytes,
                        int schema_version) {
    const bool recipes_match =
        capabilities.contains("command_recipes")
            ? capabilities.at("command_recipes") == Json(tools.command_recipe_names())
            : schema_version == 2 && tools.command_recipe_names().empty();
    const bool policy_matches =
        capabilities.contains("policy_fingerprint")
            ? capabilities.value("policy_fingerprint", "") == tools.policy_fingerprint()
            : schema_version == 2 && tools.policy_fingerprint().empty();
    const bool changeset_approval_matches =
        capabilities.contains("approve_each_changeset")
            ? capabilities.value("approve_each_changeset", false) ==
                  tools.requires_change_set_approval()
            : schema_version == 2 && !tools.requires_change_set_approval();
    const ToolRuntimeSettings default_tool_limits;
    const bool tool_limits_match = capabilities.contains("tool_limits")
                                       ? capabilities.at("tool_limits") ==
                                             tool_runtime_settings_to_json(tools.runtime_settings())
                                       : tools.runtime_settings() == default_tool_limits;
    const bool durable_changesets_match = capabilities.contains("durable_changesets")
                                              ? capabilities.value("durable_changesets", false) ==
                                                    tools.has_durable_change_transactions()
                                              : schema_version < session_schema_version;
    const bool transaction_path_matches =
        capabilities.contains("change_transaction_path")
            ? capabilities.value("change_transaction_path", "") == tools.change_transaction_path()
            : schema_version < session_schema_version;

    return capabilities.value("allow_write", false) == tools.can_write() &&
           capabilities.contains("allowed_write_paths") &&
           capabilities.at("allowed_write_paths") == Json(tools.allowed_write_paths()) &&
           capabilities.value("approve_each_command", false) == tools.requires_command_approval() &&
           capabilities.value("require_verification", false) == require_verification &&
           capabilities.value("command_sandboxed", false) == tools.commands_are_os_sandboxed() &&
           capabilities.value("command_sandbox_backend", "none") ==
               tools.command_sandbox_backend() &&
           capabilities.value("max_context_bytes", std::size_t{0}) == max_context_bytes &&
           capabilities.contains("allowed_programs") &&
           capabilities.at("allowed_programs") == Json(tools.allowed_programs()) && recipes_match &&
           policy_matches && changeset_approval_matches && tool_limits_match &&
           durable_changesets_match && transaction_path_matches;
}

bool has_required_session_state(const Json& snapshot, int schema_version) {
    const bool common_state =
        snapshot.contains("user_request") && snapshot.at("user_request").is_string() &&
        snapshot.contains("messages") && snapshot.at("messages").is_array() &&
        snapshot.contains("turns") && snapshot.at("turns").is_number_unsigned() &&
        snapshot.contains("duration_ms") && snapshot.at("duration_ms").is_number_integer() &&
        snapshot.contains("execution") && snapshot.contains("pending_tool_calls") &&
        snapshot.at("pending_tool_calls").is_array() && snapshot.contains("change_journal");
    if (!common_state) {
        return false;
    }
    if (schema_version >= 3 &&
        (!snapshot.contains("in_flight_tool_call") || !snapshot.contains("model") ||
         !snapshot.at("model").is_object() || !snapshot.at("execution").is_object() ||
         !snapshot.at("execution").contains("recipe_calls") ||
         !snapshot.at("execution").contains("verification_commands") ||
         !snapshot.at("execution").contains("last_command_verification_eligible"))) {
        return false;
    }
    return schema_version < session_schema_version ||
           (snapshot.contains("change_transaction_id") &&
            (snapshot.at("change_transaction_id").is_null() ||
             snapshot.at("change_transaction_id").is_string()));
}

bool safe_to_retry(const ToolCall& call) {
    return call.name == "list_files" || call.name == "read_file" || call.name == "search_text" ||
           call.name == "workspace_changes";
}

} // namespace

Json make_checkpoint_document(const std::string& status, const std::string& user_request,
                              const Json& messages, const AgentResult& result,
                              const std::deque<ToolCall>& pending_calls,
                              const std::optional<ToolCall>& in_flight_call,
                              const ToolRegistry& tools, bool require_verification,
                              std::size_t max_context_bytes) {
    Json pending = Json::array();
    for (const auto& call : pending_calls) {
        pending.push_back(tool_call_to_json(call));
    }
    const auto transaction_id = tools.pending_change_transaction_id();
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
            {"change_transaction_id",
             transaction_id.has_value() ? Json(*transaction_id) : Json(nullptr)},
            {"change_journal", tools.workspace_change_state()},
            {"capabilities",
             {{"allow_write", tools.can_write()},
              {"allowed_write_paths", tools.allowed_write_paths()},
              {"allowed_programs", tools.allowed_programs()},
              {"command_recipes", tools.command_recipe_names()},
              {"policy_fingerprint", tools.policy_fingerprint()},
              {"approve_each_command", tools.requires_command_approval()},
              {"approve_each_changeset", tools.requires_change_set_approval()},
              {"durable_changesets", tools.has_durable_change_transactions()},
              {"change_transaction_path", tools.change_transaction_path()},
              {"tool_limits", tool_runtime_settings_to_json(tools.runtime_settings())},
              {"require_verification", require_verification},
              {"command_sandboxed", tools.commands_are_os_sandboxed()},
              {"command_sandbox_backend", tools.command_sandbox_backend()},
              {"max_context_bytes", max_context_bytes}}}};
}

RestoredSession restore_session(const Json& snapshot, ToolRegistry& tools,
                                bool require_verification, std::size_t max_context_bytes,
                                bool retry_in_flight_tool) {
    const auto schema_version = snapshot.is_object() ? snapshot.value("schema_version", 0) : 0;
    if (!snapshot.is_object() || !supported_session_schema(schema_version)) {
        throw std::invalid_argument("会话快照 schema_version 不受支持");
    }

    const auto status = snapshot.value("status", "");
    if (status == "completed") {
        throw std::invalid_argument("该会话已经完成，不能再次恢复");
    }
    if (!valid_session_status(status)) {
        throw std::invalid_argument("会话快照终态无效");
    }
    if (snapshot.value("workspace_root", "") != tools.root().generic_string()) {
        throw std::invalid_argument("会话工作区与当前 --root 不一致");
    }
    if (!snapshot.contains("capabilities") || !snapshot.at("capabilities").is_object()) {
        throw std::invalid_argument("会话快照缺少能力策略");
    }
    if (!capabilities_match(snapshot.at("capabilities"), tools, require_verification,
                            max_context_bytes, schema_version)) {
        throw std::invalid_argument("恢复会话时必须使用与原任务相同的能力授权");
    }
    if (!has_required_session_state(snapshot, schema_version)) {
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

    std::optional<ToolCall> in_flight;
    if (schema_version >= 3 && !snapshot.at("in_flight_tool_call").is_null()) {
        in_flight = tool_call_from_json(snapshot.at("in_flight_tool_call"));
        if (restored.pending_calls.empty() || restored.pending_calls.front().id != in_flight->id ||
            restored.pending_calls.front().name != in_flight->name ||
            restored.pending_calls.front().arguments != in_flight->arguments) {
            throw std::invalid_argument("会话中的 in-flight 工具与待执行队列不一致");
        }
    }

    std::optional<std::string> checkpoint_transaction_id;
    if (schema_version == session_schema_version &&
        !snapshot.at("change_transaction_id").is_null()) {
        const auto id = snapshot.at("change_transaction_id").get<std::string>();
        if (!valid_change_transaction_id(id)) {
            throw std::invalid_argument("会话中的 changeset 事务 ID 无效");
        }
        checkpoint_transaction_id = id;
    }
    restored.transaction_recovery = tools.reconcile_change_transaction(checkpoint_transaction_id);

    if (in_flight.has_value()) {
        if (in_flight->name == "apply_changeset" &&
            restored.transaction_recovery == ChangeTransactionRecovery::committed) {
            throw std::runtime_error("changeset 事务已经提交，但会话仍将同一工具标记为 in-flight");
        }
        const bool durable_changeset_retry = schema_version == session_schema_version &&
                                             in_flight->name == "apply_changeset" &&
                                             tools.has_durable_change_transactions();
        if (!safe_to_retry(*in_flight) && !durable_changeset_retry && !retry_in_flight_tool) {
            throw std::runtime_error("检查点记录到未确认完成的 in-flight 工具 " + in_flight->name +
                                     " (" + in_flight->id +
                                     ")；默认拒绝重复副作用。"
                                     "检查工作区后，只有明确接受重试风险时才使用 --retry-inflight");
        }
        restored.recovered_in_flight = true;
    }
    tools.restore_workspace_change_state(snapshot.at("change_journal"));
    return restored;
}

} // namespace mint::agent_detail
