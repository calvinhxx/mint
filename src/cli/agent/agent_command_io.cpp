#include "agent_command_internal.hpp"
#include "provider/provider_credentials.hpp"

#include "mint/infrastructure/config.hpp"
#include "mint/localization/localization.hpp"
#include "mint/runtime/terminal_text.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace mint::cli::command_detail {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

ApprovalDecision confirmed_by_user(Console& console, bool interaction_jsonl) {
    std::string answer;
    if (!console.read_line(answer)) {
        return ApprovalDecisionKind::interaction_closed;
    }
    if (interaction_jsonl) {
        try {
            const auto response = Json::parse(answer);
            if (!response.is_object() || !response.contains("approved") ||
                !response.at("approved").is_boolean()) {
                return ApprovalDecisionKind::invalid_response;
            }
            const auto source = response.value("decision_source", std::string("user"));
            if (source != "user" && source != "run_cancelled") {
                return ApprovalDecisionKind::invalid_response;
            }
            const bool approved = response.at("approved").get<bool>();
            if (source == "run_cancelled" && approved) {
                return ApprovalDecisionKind::invalid_response;
            }
            if (source == "run_cancelled") {
                return ApprovalDecisionKind::run_cancelled;
            }
            return approved ? ApprovalDecisionKind::approved : ApprovalDecisionKind::rejected;
        } catch (const Json::exception&) {
            return ApprovalDecisionKind::invalid_response;
        }
    }
    return answer == "y" || answer == "Y" || answer == "yes" || answer == "YES";
}

Json command_approval_data(const CommandApprovalRequest& request) {
    return {{"kind", "command"},
            {"program", request.program},
            {"args", request.args},
            {"cwd", request.cwd},
            {"timeout_seconds", request.timeout_seconds}};
}

Json changeset_approval_data(const ChangeSetApprovalRequest& request) {
    return {{"kind", "changeset"},
            {"paths", request.paths},
            {"unified_diff", request.unified_diff},
            {"diff_truncated", request.diff_truncated}};
}

ApprovalDecision approve_command(const CommandApprovalRequest& request, Console& console,
                                 bool interaction_jsonl, EventLog* event_log) {
    auto evidence = command_approval_data(request);
    if (event_log != nullptr) {
        event_log->emit("approval_requested", evidence);
    }
    ApprovalDecision decision;
    if (interaction_jsonl) {
        auto interaction = evidence;
        interaction.erase("kind");
        emit_interaction_message(console, "command_approval", std::move(interaction));
        decision = confirmed_by_user(console, true);
    } else {
        const auto arguments = escape_terminal_field(Json(request.args).dump());
        console.write_error(
            message(Message::cli_approval_command,
                    {arg(Placeholder::program, escape_terminal_field(request.program)),
                     arg(Placeholder::cwd, escape_terminal_field(request.cwd)),
                     arg(Placeholder::timeout, request.timeout_seconds),
                     arg(Placeholder::arguments, arguments)}));
        console.flush_error();
        decision = confirmed_by_user(console, false);
    }
    if (event_log != nullptr) {
        evidence["approved"] = decision.approved();
        evidence["decision_source"] = decision.source();
        event_log->emit("approval_resolved", std::move(evidence));
    }
    return decision;
}

ApprovalDecision approve_changeset(const ChangeSetApprovalRequest& request, Console& console,
                                   bool interaction_jsonl, EventLog* event_log) {
    auto evidence = changeset_approval_data(request);
    if (event_log != nullptr) {
        event_log->emit("approval_requested", evidence);
    }
    ApprovalDecision decision;
    if (interaction_jsonl) {
        auto interaction = evidence;
        interaction.erase("kind");
        emit_interaction_message(console, "changeset_approval", std::move(interaction));
        decision = confirmed_by_user(console, true);
    } else {
        console.write_error(message(Message::cli_approval_changeset,
                                    {arg(Placeholder::count, request.paths.size())}));
        if (request.diff_truncated) {
            console.write_error(message(Message::cli_approval_preview_truncated));
        }
        console.write_error('\n', escape_terminal_text(request.unified_diff),
                            message(Message::cli_approval_changeset_prompt));
        console.flush_error();
        decision = confirmed_by_user(console, false);
    }
    if (event_log != nullptr) {
        evidence["approved"] = decision.approved();
        evidence["decision_source"] = decision.source();
        event_log->emit("approval_resolved", std::move(evidence));
    }
    return decision;
}

class ModelStreamPrinter {
  public:
    explicit ModelStreamPrinter(Console& console) : console_(console) {}

    void close_text_line() {
        if (text_line_open_) {
            console_.write_error_line();
            text_line_open_ = false;
        }
    }

    void print(const ModelStreamEvent& event) {
        if (event.kind != ModelStreamEventKind::text_delta || event.delta.empty()) {
            return;
        }
        if (!text_line_open_) {
            console_.write_error(message(Message::cli_model_stream_prefix));
            text_line_open_ = true;
        }
        console_.write_error(escape_terminal_text(event.delta));
        console_.flush_error();
    }

  private:
    Console& console_;
    bool text_line_open_ = false;
};

void print_model_progress(const ModelProgress& progress, Console& console,
                          ModelStreamPrinter* stream_printer) {
    if (stream_printer != nullptr && (progress.kind == ModelProgressKind::attempt_started ||
                                      progress.kind == ModelProgressKind::stream_completed ||
                                      progress.kind == ModelProgressKind::retry_scheduled ||
                                      progress.kind == ModelProgressKind::request_failed)) {
        stream_printer->close_text_line();
    }
    switch (progress.kind) {
    case ModelProgressKind::attempt_started:
        console.write_error_line(message(Message::cli_model_waiting,
                                         {arg(Placeholder::attempt, progress.attempt),
                                          arg(Placeholder::maximum, progress.max_attempts)}));
        break;
    case ModelProgressKind::stream_started:
        console.write_error_line(message(Message::cli_model_stream_started));
        break;
    case ModelProgressKind::stream_completed:
        console.write_error_line(message(Message::cli_model_stream_completed,
                                         {arg(Placeholder::events, progress.stream_events),
                                          arg(Placeholder::bytes, progress.streamed_bytes)}));
        break;
    case ModelProgressKind::retry_scheduled:
        console.write_error_line(
            message(Message::cli_model_retry,
                    {arg(Placeholder::reason, progress.http_status == 0
                                                  ? message(Message::cli_model_network_failure)
                                                  : "HTTP " + std::to_string(progress.http_status)),
                     arg(Placeholder::delay_ms, progress.delay_ms)}));
        break;
    case ModelProgressKind::request_succeeded:
        console.write_error_line(message(Message::cli_model_succeeded,
                                         {arg(Placeholder::status, progress.http_status),
                                          arg(Placeholder::elapsed_ms, progress.elapsed_ms)}));
        break;
    case ModelProgressKind::request_failed:
        console.write_error_line(
            message(Message::cli_model_failed, {arg(Placeholder::attempt, progress.attempt),
                                                arg(Placeholder::maximum, progress.max_attempts)}));
        break;
    }
}

} // namespace

void emit_interaction_message(Console& console, const std::string& type, Json data) {
    console.write_error_line(Json{
        {"schema_version", 1},
        {"channel", "mint_interaction"},
        {"type", type},
        {"data",
         std::move(data)}}.dump());
    console.flush_error();
}

int exit_code_for(const AgentResult& result) {
    if (result.status == "completed") {
        return 0;
    }
    if (result.status == "cancelled") {
        return 130;
    }
    if (result.status == "timed_out") {
        return 124;
    }
    return 2;
}

void ensure_question(CommandLine& command_line, Console& console) {
    if (!command_line.question.empty()) {
        return;
    }
    if (command_line.json_output) {
        throw std::invalid_argument(message(Message::cli_task_json_requires_text));
    }
    console.write(message(Message::cli_task_prompt));
    console.flush_output();
    (void)console.read_line(command_line.question);
    if (command_line.question.empty()) {
        throw std::invalid_argument(message(Message::cli_task_missing));
    }
}

CommandApproval command_approval(Console& console, bool interaction_jsonl, EventLog* event_log,
                                 std::shared_ptr<TaskControl> task_control) {
    return [&console, interaction_jsonl, event_log,
            task_control = std::move(task_control)](const CommandApprovalRequest& request) {
        const auto decision = approve_command(request, console, interaction_jsonl, event_log);
        if (decision.cancels_run() && task_control != nullptr) {
            task_control->request_cancel();
        }
        return decision;
    };
}

ChangeSetApproval change_set_approval(Console& console, bool interaction_jsonl, EventLog* event_log,
                                      std::shared_ptr<TaskControl> task_control) {
    return [&console, interaction_jsonl, event_log,
            task_control = std::move(task_control)](const ChangeSetApprovalRequest& request) {
        const auto decision = approve_changeset(request, console, interaction_jsonl, event_log);
        if (decision.cancels_run() && task_control != nullptr) {
            task_control->request_cancel();
        }
        return decision;
    };
}

std::unique_ptr<ModelClient> create_model(const CommandLine& command_line,
                                          const std::shared_ptr<TaskControl>& task_control,
                                          EventLog* event_log, Console& console) {
    if (command_line.demo) {
        if (!command_line.json_output) {
            console.write_line(message(Message::cli_model_demo_notice));
        }
        return std::make_unique<DemoModelClient>();
    }

    auto config = load_model_provider_config(command_line.config);
    const auto profile = resolve_model_provider_profile(config);
    config.task_control = task_control;
    const auto stream_printer = std::make_shared<ModelStreamPrinter>(console);
    if (config.stream && !command_line.json_output) {
        config.stream_event = [stream_printer](const ModelStreamEvent& event) {
            stream_printer->print(event);
        };
    }
    if (event_log != nullptr || !command_line.json_output) {
        config.progress = [event_log, print_progress = !command_line.json_output, &console,
                           stream_printer](const ModelProgress& progress) {
            if (event_log != nullptr) {
                event_log->emit("model_progress", model_progress_to_json(progress));
            }
            if (print_progress) {
                print_model_progress(progress, console, stream_printer.get());
            }
        };
    }
    if (!command_line.json_output) {
        if (config.api_key.empty() && config.api_key_env.empty()) {
            console.write_line(
                message(Message::cli_provider_empty_key_notice,
                        {arg(Placeholder::path,
                             escape_terminal_field(command_line.config.generic_string()))}));
        } else if (!config.api_key_env.empty()) {
            console.write_line(
                message(Message::cli_provider_authentication_environment,
                        {arg(Placeholder::name, escape_terminal_field(config.api_key_env))}));
        } else {
            console.write_line(
                message(Message::cli_provider_warning,
                        {arg(Placeholder::message,
                             escape_terminal_field(
                                 provider_detail::inline_api_key_deprecation_message()))}));
        }
        console.write_line(message(
            Message::cli_provider_configuration,
            {arg(Placeholder::path, escape_terminal_field(command_line.config.generic_string())),
             arg(Placeholder::provider, model_provider_name(profile.provider)),
             arg(Placeholder::adapter, model_adapter_name(profile.adapter)),
             arg(Placeholder::model, escape_terminal_field(config.model)),
             arg(Placeholder::stream, config.stream ? "stream" : "non-stream")}));
    }
    return std::make_unique<ModelProviderClient>(std::move(config));
}

void print_run_configuration(const CommandLine& command_line, const ToolRegistry& tools,
                             const std::optional<TaskPolicy>& task_policy,
                             const std::optional<ManagedTaskPaths>& managed_task, bool managed_demo,
                             Console& console) {
    if (command_line.json_output) {
        return;
    }

    auto& output = console.output_stream();
    output << message(
                  Message::cli_run_workspace,
                  {arg(Placeholder::path, escape_terminal_field(tools.root().generic_string()))})
           << message(Message::cli_run_file_writes,
                      {arg(Placeholder::status,
                           message(tools.can_write() ? Message::common_explicitly_allowed
                                                     : Message::common_disabled))});
    if (managed_demo) {
        output << message(Message::cli_run_demo_policy);
    } else if (task_policy.has_value()) {
        output << message(
            Message::cli_run_policy,
            {arg(Placeholder::path,
                 escape_terminal_field(task_policy->source_path.generic_string())),
             arg(Placeholder::fingerprint, escape_terminal_field(task_policy->fingerprint))});
    }
    if (!tools.allowed_write_paths().empty()) {
        output << message(Message::cli_run_write_paths);
        for (const auto& path : tools.allowed_write_paths()) {
            output << ' ' << escape_terminal_field(path);
        }
        output << '\n';
    }
    if (tools.can_run_commands()) {
        output << message(tools.uses_command_recipes() ? Message::cli_run_command_recipes
                                                       : Message::cli_run_commands_allowed);
        const auto& names =
            tools.uses_command_recipes() ? tools.command_recipe_names() : tools.allowed_programs();
        for (const auto& name : names) {
            output << ' ' << escape_terminal_field(name);
        }
        output << message(
            Message::cli_run_command_sandbox,
            {arg(Placeholder::backend, escape_terminal_field(tools.command_sandbox_backend()))});
    } else {
        output << message(Message::cli_run_commands_disabled);
    }
    const auto state = [](bool enabled) {
        return message(enabled ? Message::common_enabled : Message::common_disabled);
    };
    const auto time_budget = command_line.max_seconds == 0
                                 ? message(Message::common_unlimited)
                                 : message(Message::cli_run_seconds,
                                           {arg(Placeholder::value, command_line.max_seconds)});
    const auto token_budget =
        command_line.max_total_tokens == 0
            ? message(Message::common_unlimited)
            : message(Message::cli_run_tokens,
                      {arg(Placeholder::value, command_line.max_total_tokens)});
    output << message(
        Message::cli_run_controls,
        {arg(Placeholder::command_approval, state(command_line.approve_each_command)),
         arg(Placeholder::changeset_approval, state(command_line.approve_each_changeset)),
         arg(Placeholder::verification, state(command_line.require_verification)),
         arg(Placeholder::time_budget, time_budget), arg(Placeholder::token_budget, token_budget),
         arg(Placeholder::session,
             message(command_line.resume_session ? Message::cli_run_session_resume
                                                 : Message::cli_run_session_new))});
    if (managed_task.has_value()) {
        output << message(Message::cli_run_managed_task,
                          {arg(Placeholder::id, escape_terminal_field(managed_task->id)),
                           arg(Placeholder::directory,
                               escape_terminal_field(managed_task->directory.generic_string()))});
    }
}

} // namespace mint::cli::command_detail
