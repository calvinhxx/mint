#include "aiagent/application/agent.hpp"
#include "aiagent/domain/task_policy.hpp"
#include "aiagent/infrastructure/chat_completions_client.hpp"
#include "aiagent/infrastructure/config.hpp"
#include "aiagent/infrastructure/event_log.hpp"
#include "aiagent/infrastructure/project_store.hpp"
#include "aiagent/infrastructure/session_store.hpp"
#include "aiagent/runtime/task_control.hpp"
#include "aiagent/tools/tool_registry.hpp"
#include "aiagent/version.hpp"

#include "command_line.hpp"
#include "project_commands.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t interrupt_requested = 0;

void handle_interrupt(int) {
    interrupt_requested = 1;
}

bool approve_command(const aiagent::CommandApprovalRequest& request) {
    std::cerr << "\n[命令审批] 程序=" << request.program << " cwd=" << request.cwd
              << " timeout=" << request.timeout_seconds << "s\n"
              << "参数=" << aiagent::Json(request.args).dump() << "\n"
              << "允许本次执行？[y/N] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return false;
    }
    return answer == "y" || answer == "Y" || answer == "yes" || answer == "YES";
}

bool approve_changeset(const aiagent::ChangeSetApprovalRequest& request) {
    std::cerr << "\n[ChangeSet 审批] " << request.paths.size() << " 个文件";
    if (request.diff_truncated) {
        std::cerr << "（预览已截断）";
    }
    std::cerr << "\n" << request.unified_diff << "允许提交这一组修改？[y/N] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return false;
    }
    return answer == "y" || answer == "Y" || answer == "yes" || answer == "YES";
}

int exit_code_for(const aiagent::AgentResult& result) {
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

void print_model_progress(const aiagent::ModelProgress& progress) {
    switch (progress.kind) {
    case aiagent::ModelProgressKind::attempt_started:
        std::cerr << "  [模型] 等待响应（尝试 " << progress.attempt << '/' << progress.max_attempts
                  << "）\n";
        break;
    case aiagent::ModelProgressKind::retry_scheduled:
        std::cerr << "  [模型] "
                  << (progress.http_status == 0 ? "网络请求暂时失败"
                                                : "HTTP " + std::to_string(progress.http_status))
                  << "，" << progress.delay_ms << " ms 后重试\n";
        break;
    case aiagent::ModelProgressKind::request_succeeded:
        std::cerr << "  [模型] 收到响应（HTTP " << progress.http_status << "，"
                  << progress.elapsed_ms << " ms）\n";
        break;
    case aiagent::ModelProgressKind::request_failed:
        std::cerr << "  [模型] 请求失败（尝试 " << progress.attempt << '/' << progress.max_attempts
                  << "）\n";
        break;
    }
}

} // namespace

int main(int argc, char** argv) {
    const bool error_as_json = aiagent::cli::requested_json_output(argc, argv);
    std::optional<aiagent::ManagedTaskPaths> managed_task;
    try {
        auto command_line = aiagent::cli::parse_arguments(argc, argv);
        if (command_line.help) {
            aiagent::cli::print_help(argv[0]);
            return 0;
        }
        if (command_line.version) {
            std::cout << "aiagent " << aiagent::version << '\n';
            return 0;
        }

        std::optional<aiagent::ProjectStore> project_store;
        if (aiagent::cli::is_managed_mode(command_line.mode)) {
            project_store.emplace(command_line.root, command_line.state_dir);
            if (command_line.mode == aiagent::cli::CommandMode::init) {
                return aiagent::cli::handle_init_command(command_line, *project_store, std::cout);
            }
            if (command_line.mode == aiagent::cli::CommandMode::status) {
                return aiagent::cli::handle_status_command(command_line, *project_store, std::cout);
            }
            (void)project_store->load_profile();
            if (command_line.mode == aiagent::cli::CommandMode::run) {
                if (command_line.question.empty()) {
                    if (command_line.json_output) {
                        throw std::invalid_argument("--json 模式需要在命令行提供任务内容");
                    }
                    std::cout << "你想让 Agent 完成什么？ ";
                    std::getline(std::cin, command_line.question);
                }
                if (command_line.question.empty()) {
                    throw std::invalid_argument("没有收到任务内容");
                }
                managed_task = project_store->create_task(
                    command_line.question, command_line.demo ? aiagent::ManagedTaskMode::demo
                                                             : aiagent::ManagedTaskMode::model);
            } else {
                if (command_line.demo) {
                    throw std::invalid_argument("离线固定脚本不支持 resume");
                }
                if (!command_line.task_id.empty()) {
                    const auto summary = project_store->task_summary(command_line.task_id);
                    if (!summary.has_value()) {
                        throw std::runtime_error("找不到任务: " + command_line.task_id);
                    }
                    if (summary->mode == aiagent::ManagedTaskMode::demo) {
                        throw std::runtime_error("离线演示任务不可恢复");
                    }
                    if (!summary->resumable) {
                        throw std::runtime_error("任务状态 " + summary->status + " 不可恢复");
                    }
                    managed_task = project_store->task_paths(command_line.task_id);
                } else {
                    managed_task = project_store->latest_resumable_task();
                    if (!managed_task.has_value()) {
                        throw std::runtime_error("没有可恢复任务");
                    }
                }
            }
            command_line.policy = managed_task->policy;
            command_line.session = managed_task->session;
            command_line.events_jsonl = managed_task->events;
            command_line.resume_session = command_line.mode == aiagent::cli::CommandMode::resume;
        }

        std::optional<aiagent::TaskPolicy> task_policy;
        if (!command_line.policy.empty()) {
            if (command_line.policy_conflict) {
                throw std::invalid_argument(
                    "--policy 不能与 --allow-write、--allow-command、--require-verification、"
                    "--unsafe-no-command-sandbox 或预算参数混用");
            }
            task_policy = aiagent::load_task_policy(command_line.policy);
            command_line.allow_write = !task_policy->write_paths.empty();
            command_line.allowed_write_paths = task_policy->write_paths;
            command_line.command_recipes = task_policy->recipes;
            command_line.require_verification = task_policy->require_verification;
            command_line.max_turns = task_policy->max_turns;
            command_line.max_context_bytes = task_policy->max_context_bytes;
            command_line.max_seconds = task_policy->max_seconds;
            command_line.policy_fingerprint = task_policy->fingerprint;
        }
        const bool managed_demo = managed_task.has_value() && command_line.demo;
        if (managed_demo) {
            command_line.allow_write = false;
            command_line.allowed_write_paths.clear();
            command_line.command_recipes.clear();
            command_line.require_verification = false;
            command_line.policy_fingerprint.clear();
        }
        const bool commands_enabled =
            !command_line.allowed_programs.empty() || !command_line.command_recipes.empty();
        if (command_line.resume_session && command_line.session.empty()) {
            throw std::invalid_argument("--resume 必须与 --session 一起使用");
        }
        if (command_line.retry_inflight && !command_line.resume_session) {
            throw std::invalid_argument("--retry-inflight 必须与 --resume 一起使用");
        }
        if (command_line.resume_session && command_line.demo) {
            throw std::invalid_argument("离线固定脚本不支持 --resume");
        }
        if (command_line.resume_session && !command_line.question.empty()) {
            throw std::invalid_argument("--resume 不能同时提供新的问题");
        }
        if (command_line.approve_each_command && !commands_enabled) {
            throw std::invalid_argument(
                "--approve-each-command 需要 --allow-command 或 policy recipe");
        }
        if (!command_line.allowed_write_paths.empty() && !command_line.allow_write) {
            throw std::invalid_argument("--allow-write-path 需要同时传入 --allow-write");
        }
        if (command_line.approve_each_changeset && !command_line.allow_write) {
            throw std::invalid_argument("--approve-each-changeset 需要启用写入能力");
        }
        if (command_line.unsafe_no_command_sandbox && !commands_enabled) {
            throw std::invalid_argument("--unsafe-no-command-sandbox 只在同时授权命令时有意义");
        }
        if (command_line.require_verification && (!command_line.allow_write || !commands_enabled)) {
            throw std::invalid_argument("验证门禁需要同时启用写入和至少一个命令或 recipe");
        }

        if (!command_line.resume_session && command_line.question.empty()) {
            if (command_line.json_output) {
                throw std::invalid_argument("--json 模式需要在命令行提供任务内容");
            }
            std::cout << "你想让 Agent 完成什么？ ";
            std::getline(std::cin, command_line.question);
        }
        if (!command_line.resume_session && command_line.question.empty()) {
            throw std::invalid_argument("没有收到任务内容");
        }

        interrupt_requested = 0;
        std::signal(SIGINT, handle_interrupt);
        const auto total_budget = command_line.max_seconds == 0
                                      ? std::chrono::milliseconds::zero()
                                      : std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::seconds(command_line.max_seconds));
        auto task_control =
            std::make_shared<aiagent::TaskControl>(total_budget, &interrupt_requested);

        const auto normalized_config = aiagent::cli::normalized_path(command_line.config);
        const auto normalized_policy = command_line.policy.empty()
                                           ? std::filesystem::path{}
                                           : aiagent::cli::normalized_path(command_line.policy);
        const auto normalized_session = command_line.session.empty()
                                            ? std::filesystem::path{}
                                            : aiagent::cli::normalized_path(command_line.session);
        const auto normalized_events =
            command_line.events_jsonl.empty()
                ? std::filesystem::path{}
                : aiagent::cli::normalized_path(command_line.events_jsonl);
        if (!normalized_policy.empty() && normalized_policy == normalized_config) {
            throw std::invalid_argument("task policy 与模型配置必须使用不同文件");
        }
        if ((!normalized_session.empty() && normalized_session == normalized_config) ||
            (!normalized_events.empty() && normalized_events == normalized_config)) {
            throw std::invalid_argument("会话或事件路径不能与模型配置文件相同");
        }
        if (!normalized_session.empty() && normalized_session == normalized_events) {
            throw std::invalid_argument("会话快照与事件日志必须使用不同文件");
        }
        if ((!normalized_policy.empty() && normalized_policy == normalized_session) ||
            (!normalized_policy.empty() && normalized_policy == normalized_events)) {
            throw std::invalid_argument("task policy 不能与会话或事件文件共用路径");
        }

        std::unique_ptr<aiagent::SessionStore> session_store;
        if (!command_line.session.empty()) {
            session_store = std::make_unique<aiagent::SessionStore>(command_line.session);
        }
        std::unique_ptr<aiagent::EventLog> event_log;
        if (!command_line.events_jsonl.empty()) {
            event_log = std::make_unique<aiagent::EventLog>(command_line.events_jsonl,
                                                            command_line.resume_session);
        }

        std::vector<std::filesystem::path> protected_paths = {command_line.config};
        if (task_policy.has_value()) {
            protected_paths.push_back(task_policy->source_path);
        }
        if (session_store != nullptr) {
            protected_paths.push_back(session_store->path());
        }
        if (event_log != nullptr) {
            protected_paths.push_back(event_log->path());
        }
        if (managed_task.has_value()) {
            protected_paths.push_back(project_store->project_directory());
        }

        aiagent::ToolRegistry tools(
            command_line.root,
            aiagent::ToolRegistryOptions{
                .protected_paths = std::move(protected_paths),
                .allow_write = command_line.allow_write,
                .allowed_write_paths = command_line.allowed_write_paths,
                .allowed_programs = command_line.allowed_programs,
                .command_recipes = command_line.command_recipes,
                .policy_fingerprint = command_line.policy_fingerprint,
                .task_control = task_control,
                .command_approval = command_line.approve_each_command
                                        ? aiagent::CommandApproval(approve_command)
                                        : aiagent::CommandApproval{},
                .change_set_approval = command_line.approve_each_changeset
                                           ? aiagent::ChangeSetApproval(approve_changeset)
                                           : aiagent::ChangeSetApproval{},
                .require_command_sandbox =
                    commands_enabled && !command_line.unsafe_no_command_sandbox});
        std::unique_ptr<aiagent::ModelClient> model;

        if (command_line.demo) {
            model = std::make_unique<aiagent::DemoModelClient>();
            if (!command_line.json_output) {
                std::cout << "离线演示模式：这里的“模型决策”是固定脚本，用来观察循环。\n";
            }
        } else {
            auto config = aiagent::load_chat_completions_config(command_line.config);
            config.task_control = task_control;
            if (event_log != nullptr || !command_line.json_output) {
                config.progress = [progress_log = event_log.get(),
                                   print_progress = !command_line.json_output](
                                      const aiagent::ModelProgress& progress) {
                    if (progress_log != nullptr) {
                        progress_log->emit("model_progress",
                                           aiagent::model_progress_to_json(progress));
                    }
                    if (print_progress) {
                        print_model_progress(progress);
                    }
                };
            }
            if (!command_line.json_output) {
                if (config.api_key.empty()) {
                    std::cout << "提示: " << command_line.config.generic_string()
                              << " 中的 api_key 为空；只有不需要密钥的本地接口"
                                 "可以这样运行。\n";
                }
                std::cout << "模型配置: " << command_line.config.generic_string() << "（"
                          << config.model << "）\n";
            }
            model = std::make_unique<aiagent::ChatCompletionsClient>(std::move(config));
        }

        if (!command_line.json_output) {
            std::cout << "工作范围: " << tools.root().generic_string() << '\n'
                      << "文件写入: " << (tools.can_write() ? "已显式允许" : "关闭") << '\n';
            if (managed_demo) {
                std::cout << "任务策略: 离线演示强制只读，不能恢复为真实模型任务\n";
            } else if (task_policy.has_value()) {
                std::cout << "任务策略: " << task_policy->source_path.generic_string()
                          << "（fingerprint=" << task_policy->fingerprint << "）\n";
            }
            if (!tools.allowed_write_paths().empty()) {
                std::cout << "写路径范围:";
                for (const auto& path : tools.allowed_write_paths()) {
                    std::cout << ' ' << path;
                }
                std::cout << '\n';
            }
            if (tools.can_run_commands()) {
                if (tools.uses_command_recipes()) {
                    std::cout << "命令配方:";
                    for (const auto& recipe : tools.command_recipe_names()) {
                        std::cout << ' ' << recipe;
                    }
                } else {
                    std::cout << "命令执行: 已显式授权";
                    for (const auto& program : tools.allowed_programs()) {
                        std::cout << ' ' << program;
                    }
                }
                std::cout << "（无 shell，OS 沙箱=" << tools.command_sandbox_backend() << "）\n";
            } else {
                std::cout << "命令执行: 关闭\n";
            }
            std::cout << "逐命令审批: " << (command_line.approve_each_command ? "启用" : "关闭")
                      << '\n'
                      << "逐 ChangeSet 审批: "
                      << (command_line.approve_each_changeset ? "启用" : "关闭") << '\n'
                      << "验证门禁: " << (command_line.require_verification ? "启用" : "关闭")
                      << '\n'
                      << "总时间预算: "
                      << (command_line.max_seconds == 0
                              ? "不限制"
                              : std::to_string(command_line.max_seconds) + " 秒")
                      << '\n'
                      << "会话模式: " << (command_line.resume_session ? "恢复" : "新任务") << '\n';
            if (managed_task.has_value()) {
                std::cout << "任务 ID: " << managed_task->id << '\n'
                          << "任务目录: " << managed_task->directory.generic_string() << '\n';
            }
        }

        std::ostringstream machine_mode_log;
        std::ostream& agent_output = command_line.json_output
                                         ? static_cast<std::ostream&>(machine_mode_log)
                                         : static_cast<std::ostream&>(std::cout);
        aiagent::Agent agent(
            *model, tools, agent_output,
            aiagent::AgentOptions{.max_turns = command_line.max_turns,
                                  .max_context_bytes = command_line.max_context_bytes,
                                  .require_verification_after_write =
                                      command_line.require_verification,
                                  .task_control = task_control,
                                  .event_log = event_log.get(),
                                  .session_store = session_store.get(),
                                  .resume_session = command_line.resume_session,
                                  .retry_in_flight_tool = command_line.retry_inflight});
        const auto result = agent.run(command_line.resume_session ? "" : command_line.question);
        if (command_line.json_output) {
            auto machine_result = aiagent::agent_result_to_json(result);
            if (managed_task.has_value()) {
                machine_result["task_id"] = managed_task->id;
                machine_result["task_directory"] = managed_task->directory.generic_string();
            }
            std::cout << machine_result.dump() << '\n';
        }
        return exit_code_for(result);
    } catch (const std::exception& error) {
        if (error_as_json) {
            aiagent::Json result = {{"schema_version", 1},
                                    {"status", "error"},
                                    {"completed", false},
                                    {"error", error.what()}};
            if (managed_task.has_value()) {
                result["task_id"] = managed_task->id;
                result["task_directory"] = managed_task->directory.generic_string();
            }
            std::cout << result.dump() << '\n';
        } else {
            std::cerr << "错误: " << error.what() << '\n';
            if (managed_task.has_value()) {
                std::cerr << "任务 ID: " << managed_task->id << '\n';
            }
        }
        return 1;
    }
}
