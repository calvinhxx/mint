#include "agent_command_internal.hpp"

#include "mint/infrastructure/config.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace mint::cli::command_detail {
namespace {

bool confirmed_by_user(Console& console) {
    std::string answer;
    if (!console.read_line(answer)) {
        return false;
    }
    return answer == "y" || answer == "Y" || answer == "yes" || answer == "YES";
}

bool approve_command(const CommandApprovalRequest& request, Console& console) {
    console.write_error("\n[命令审批] 程序=", request.program, " cwd=", request.cwd,
                        " timeout=", request.timeout_seconds, "s\n参数=", Json(request.args).dump(),
                        "\n允许本次执行？[y/N] ");
    console.flush_error();
    return confirmed_by_user(console);
}

bool approve_changeset(const ChangeSetApprovalRequest& request, Console& console) {
    console.write_error("\n[ChangeSet 审批] ", request.paths.size(), " 个文件");
    if (request.diff_truncated) {
        console.write_error("（预览已截断）");
    }
    console.write_error('\n', request.unified_diff, "允许提交这一组修改？[y/N] ");
    console.flush_error();
    return confirmed_by_user(console);
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
        console_.write_error(event.delta);
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

CommandApproval command_approval(Console& console) {
    return [&console](const CommandApprovalRequest& request) {
        return approve_command(request, console);
    };
}

ChangeSetApproval change_set_approval(Console& console) {
    return [&console](const ChangeSetApprovalRequest& request) {
        return approve_changeset(request, console);
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
            console.write_line("提示: ", command_line.config.generic_string(),
                               " 中的 api_key 为空；只有不需要密钥的本地接口可以这样运行。");
        } else if (!config.api_key_env.empty()) {
            console.write_line("模型认证: 环境变量 ", config.api_key_env);
        }
        console.write_line("模型配置: ", command_line.config.generic_string(), "（",
                           model_provider_name(profile.provider), " / ",
                           model_adapter_name(profile.adapter), " / ", config.model, " / ",
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
    output << "工作范围: " << tools.root().generic_string() << '\n'
           << "文件写入: " << (tools.can_write() ? "已显式允许" : "关闭") << '\n';
    if (managed_demo) {
        output << "任务策略: 离线演示强制只读，不能恢复为真实模型任务\n";
    } else if (task_policy.has_value()) {
        output << "任务策略: " << task_policy->source_path.generic_string()
               << "（fingerprint=" << task_policy->fingerprint << "）\n";
    }
    if (!tools.allowed_write_paths().empty()) {
        output << "写路径范围:";
        for (const auto& path : tools.allowed_write_paths()) {
            output << ' ' << path;
        }
        output << '\n';
    }
    if (tools.can_run_commands()) {
        output << (tools.uses_command_recipes() ? "命令配方:" : "命令执行: 已显式授权");
        const auto& names =
            tools.uses_command_recipes() ? tools.command_recipe_names() : tools.allowed_programs();
        for (const auto& name : names) {
            output << ' ' << name;
        }
        output << "（无 shell，OS 沙箱=" << tools.command_sandbox_backend() << "）\n";
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
        output << "任务 ID: " << managed_task->id << '\n'
               << "任务目录: " << managed_task->directory.generic_string() << '\n';
    }
}

} // namespace mint::cli::command_detail
