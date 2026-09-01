#include "agent_command_internal.hpp"

#include "mint/infrastructure/config.hpp"
#include "mint/runtime/terminal_text.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace mint::cli::command_detail {
namespace {

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
        console.write_error("\n[命令审批] 程序=", escape_terminal_field(request.program),
                            " cwd=", escape_terminal_field(request.cwd),
                            " timeout=", request.timeout_seconds, "s\n参数=", arguments,
                            "\n允许本次执行？[y/N] ");
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
        console.write_error("\n[ChangeSet 审批] ", request.paths.size(), " 个文件");
        if (request.diff_truncated) {
            console.write_error("（预览已截断）");
        }
        console.write_error('\n', escape_terminal_text(request.unified_diff),
                            "允许提交这一组修改？[y/N] ");
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
            console_.write_error("  [模型流] ");
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
        console.write_error_line("  [模型] 等待响应（尝试 ", progress.attempt, '/',
                                 progress.max_attempts, "）");
        break;
    case ModelProgressKind::stream_started:
        console.write_error_line("  [模型] 已请求流式响应");
        break;
    case ModelProgressKind::stream_completed:
        console.write_error_line("  [模型] 流结束（", progress.stream_events, " 个事件，",
                                 progress.streamed_bytes, " 字节增量）");
        break;
    case ModelProgressKind::retry_scheduled:
        console.write_error_line("  [模型] ",
                                 progress.http_status == 0
                                     ? "网络请求暂时失败"
                                     : "HTTP " + std::to_string(progress.http_status),
                                 "，", progress.delay_ms, " ms 后重试");
        break;
    case ModelProgressKind::request_succeeded:
        console.write_error_line("  [模型] 收到响应（HTTP ", progress.http_status, "，",
                                 progress.elapsed_ms, " ms）");
        break;
    case ModelProgressKind::request_failed:
        console.write_error_line("  [模型] 请求失败（尝试 ", progress.attempt, '/',
                                 progress.max_attempts, "）");
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
        throw std::invalid_argument("--json 模式需要在命令行提供任务内容");
    }
    console.write("你想让 Agent 完成什么？ ");
    console.flush_output();
    (void)console.read_line(command_line.question);
    if (command_line.question.empty()) {
        throw std::invalid_argument("没有收到任务内容");
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
            console.write_line("离线演示模式：这里的“模型决策”是固定脚本，用来观察循环。");
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
                "提示: ", escape_terminal_field(command_line.config.generic_string()),
                " 中的 api_key 为空；只有不需要密钥的本地接口可以这样运行。");
        } else if (!config.api_key_env.empty()) {
            console.write_line("模型认证: 环境变量 ", escape_terminal_field(config.api_key_env));
        }
        console.write_line(
            "模型配置: ", escape_terminal_field(command_line.config.generic_string()), "（",
            model_provider_name(profile.provider), " / ", model_adapter_name(profile.adapter),
            " / ", escape_terminal_field(config.model), " / ",
            config.stream ? "stream" : "non-stream", "）");
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
    output << "工作范围: " << escape_terminal_field(tools.root().generic_string()) << '\n'
           << "文件写入: " << (tools.can_write() ? "已显式允许" : "关闭") << '\n';
    if (managed_demo) {
        output << "任务策略: 离线演示强制只读，不能恢复为真实模型任务\n";
    } else if (task_policy.has_value()) {
        output << "任务策略: " << escape_terminal_field(task_policy->source_path.generic_string())
               << "（fingerprint=" << escape_terminal_field(task_policy->fingerprint) << "）\n";
    }
    if (!tools.allowed_write_paths().empty()) {
        output << "写路径范围:";
        for (const auto& path : tools.allowed_write_paths()) {
            output << ' ' << escape_terminal_field(path);
        }
        output << '\n';
    }
    if (tools.can_run_commands()) {
        output << (tools.uses_command_recipes() ? "命令配方:" : "命令执行: 已显式授权");
        const auto& names =
            tools.uses_command_recipes() ? tools.command_recipe_names() : tools.allowed_programs();
        for (const auto& name : names) {
            output << ' ' << escape_terminal_field(name);
        }
        output << "（无 shell，OS 沙箱=" << escape_terminal_field(tools.command_sandbox_backend())
               << "）\n";
    } else {
        output << "命令执行: 关闭\n";
    }
    output << "逐命令审批: " << (command_line.approve_each_command ? "启用" : "关闭") << '\n'
           << "逐 ChangeSet 审批: " << (command_line.approve_each_changeset ? "启用" : "关闭")
           << '\n'
           << "验证门禁: " << (command_line.require_verification ? "启用" : "关闭") << '\n'
           << "总时间预算: "
           << (command_line.max_seconds == 0 ? "不限制"
                                             : std::to_string(command_line.max_seconds) + " 秒")
           << '\n'
           << "会话模式: " << (command_line.resume_session ? "恢复" : "新任务") << '\n';
    if (managed_task.has_value()) {
        output << "任务 ID: " << escape_terminal_field(managed_task->id) << '\n'
               << "任务目录: " << escape_terminal_field(managed_task->directory.generic_string())
               << '\n';
    }
}

} // namespace mint::cli::command_detail
