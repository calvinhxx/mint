#include "mint_cli_protocol.hpp"

#include "mint/domain/model.hpp"

#include <utility>

namespace mint::infrastructure::desktop {
namespace {

using mint::desktop::ApprovalKind;
using mint::desktop::ApprovalRequest;
using mint::desktop::RunEvent;
using mint::desktop::RunEventKind;
using mint::desktop::TaskResult;
using mint::desktop::UsageSummary;

std::string string_field(const Json& value, const char* key) {
    return value.contains(key) && value.at(key).is_string() ? value.at(key).get<std::string>()
                                                            : std::string{};
}

std::size_t size_field(const Json& value, const char* key) {
    return value.contains(key) && value.at(key).is_number_unsigned()
               ? value.at(key).get<std::size_t>()
               : std::size_t{0};
}

std::filesystem::path path_from_utf8(const std::string& value) {
#if defined(_WIN32)
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
#else
    return std::filesystem::path(value);
#endif
}

RunEventKind event_kind(std::string_view type) {
    if (type == "task_started") {
        return RunEventKind::task_started;
    }
    if (type == "model_requested") {
        return RunEventKind::model_requested;
    }
    if (type == "model_completed") {
        return RunEventKind::model_completed;
    }
    if (type == "model_progress") {
        return RunEventKind::model_progress;
    }
    if (type == "tool_started") {
        return RunEventKind::tool_started;
    }
    if (type == "tool_completed") {
        return RunEventKind::tool_completed;
    }
    if (type == "approval_requested") {
        return RunEventKind::approval_requested;
    }
    if (type == "approval_resolved") {
        return RunEventKind::approval_resolved;
    }
    if (type == "verification_ready") {
        return RunEventKind::verification_ready;
    }
    if (type == "verification_blocked") {
        return RunEventKind::verification_blocked;
    }
    if (type == "context_compacted") {
        return RunEventKind::context_compacted;
    }
    if (type == "task_finished") {
        return RunEventKind::task_finished;
    }
    return RunEventKind::unknown;
}

UsageSummary usage_summary(const Json& value) {
    if (!value.is_object()) {
        return {};
    }
    return {.calls = size_field(value, "calls"),
            .total_tokens = size_field(value, "total_tokens"),
            .prompt_tokens = size_field(value, "prompt_tokens"),
            .cached_tokens = size_field(value, "cached_tokens"),
            .usage_reports = size_field(value, "usage_reports"),
            .cache_usage_reports = size_field(value, "cache_usage_reports")};
}

ApprovalRequest approval_request(ApprovalKind kind, const Json& data) {
    ApprovalRequest request{.kind = kind,
                            .program = string_field(data, "program"),
                            .working_directory = string_field(data, "cwd"),
                            .unified_diff = string_field(data, "unified_diff"),
                            .diff_truncated = data.value("diff_truncated", false)};
    for (const auto& path : data.value("paths", Json::array())) {
        if (path.is_string()) {
            request.paths.push_back(path.get<std::string>());
        }
    }
    for (const auto& argument : data.value("args", Json::array())) {
        if (argument.is_string()) {
            request.arguments.push_back(argument.get<std::string>());
        }
    }
    return request;
}

} // namespace

std::optional<StatusResponse> MintCliProtocol::parse_status(std::string_view payload,
                                                            std::string& error) {
    try {
        const auto document = Json::parse(payload.begin(), payload.end());
        StatusResponse response;
        response.state_directory = path_from_utf8(string_field(document, "state_directory"));
        for (const auto& item : document.value("tasks", Json::array())) {
            response.tasks.push_back(
                {.id = string_field(item, "id"),
                 .question = string_field(item, "question"),
                 .status = string_field(item, "status"),
                 .created_at = string_field(item, "created_at"),
                 .verification_status = string_field(item, "verification_status"),
                 .completed = item.value("completed", false),
                 .resumable = item.value("resumable", false)});
        }
        return response;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

std::optional<RunEvent> MintCliProtocol::parse_event(std::string_view payload) {
    try {
        const auto document = Json::parse(payload.begin(), payload.end());
        const auto data = document.value("data", Json::object());
        const auto raw_type = string_field(document, "type");
        const auto arguments = data.value("arguments_summary", Json::object());
        auto target = string_field(arguments, "path");
        if (target.empty()) {
            target = string_field(arguments, "program");
        }
        if (target.empty()) {
            target = string_field(arguments, "cwd");
        }
        if (target.empty()) {
            target = string_field(data, "workspace_root");
        }
        RunEvent event{.sequence = document.value("seq", std::size_t{0}),
                       .timestamp = string_field(document, "timestamp"),
                       .kind = event_kind(raw_type),
                       .raw_type = raw_type,
                       .name = string_field(data, "name"),
                       .target = std::move(target),
                       .status = string_field(data, "status"),
                       .verification_status = string_field(data, "verification_status"),
                       .evidence = string_field(data, "unified_diff"),
                       .affected_paths = data.value("paths", Json::array()).size(),
                       .usage = usage_summary(data.value("model", Json::object()))};
        if (data.contains("turn") && data.at("turn").is_number_unsigned()) {
            event.turn = data.at("turn").get<std::size_t>();
        }
        if (event.evidence.empty() && !data.empty()) {
            event.evidence = data.dump(2);
        }
        return event;
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

std::optional<TaskResult> MintCliProtocol::parse_result(std::string_view payload) {
    try {
        const auto document = Json::parse(payload.begin(), payload.end());
        if (!document.is_object() || document.contains("error") ||
            string_field(document, "status").empty() || !document.contains("answer") ||
            !document.at("answer").is_string()) {
            return std::nullopt;
        }
        const auto changes = document.value("changes", Json::object());
        return TaskResult{.status = string_field(document, "status"),
                          .answer = string_field(document, "answer"),
                          .unified_diff = string_field(changes, "unified_diff"),
                          .usage = usage_summary(document.value("model", Json::object()))};
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

std::optional<InteractionMessage> MintCliProtocol::parse_interaction(std::string_view payload) {
    try {
        const auto document = Json::parse(payload.begin(), payload.end());
        if (string_field(document, "channel") != "mint_interaction") {
            return std::nullopt;
        }
        const auto type = string_field(document, "type");
        const auto data = document.value("data", Json::object());
        if (type == "task_ready") {
            return InteractionMessage{.kind = InteractionKind::task_ready,
                                      .task_id = string_field(data, "task_id"),
                                      .events_path =
                                          path_from_utf8(string_field(data, "events_path"))};
        }
        if (type == "command_approval" || type == "changeset_approval") {
            return InteractionMessage{.kind = InteractionKind::approval,
                                      .approval = approval_request(type == "command_approval"
                                                                       ? ApprovalKind::command
                                                                       : ApprovalKind::changeset,
                                                                   data)};
        }
    } catch (const Json::exception&) {
    }
    return std::nullopt;
}

std::string MintCliProtocol::parse_error(std::string_view payload) {
    try {
        return string_field(Json::parse(payload.begin(), payload.end()), "error");
    } catch (const Json::exception&) {
        return {};
    }
}

std::string MintCliProtocol::approval_decision(bool approved, std::string_view source) {
    return Json{{"approved", approved}, {"decision_source", source}}.dump() + "\n";
}

} // namespace mint::infrastructure::desktop
