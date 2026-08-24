#include "aiagent/application/agent.hpp"
#include "aiagent/domain/task_policy.hpp"
#include "aiagent/infrastructure/chat_completions_client.hpp"
#include "aiagent/infrastructure/config.hpp"
#include "aiagent/infrastructure/event_log.hpp"
#include "aiagent/infrastructure/session_store.hpp"
#include "aiagent/runtime/task_control.hpp"
#include "aiagent/tools/tool_registry.hpp"
#include "aiagent/version.hpp"

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

struct CommandLine {
    bool demo = false;
    bool help = false;
    bool version = false;
    bool allow_write = false;
    bool require_verification = false;
    bool approve_each_command = false;
    bool approve_each_changeset = false;
    bool unsafe_no_command_sandbox = false;
    bool json_output = false;
    bool resume = false;
    bool retry_inflight = false;
    bool policy_conflict = false;
    std::filesystem::path config = "config.json";
    std::filesystem::path policy;
    std::filesystem::path root = std::filesystem::current_path();
    std::filesystem::path events_jsonl;
    std::filesystem::path session;
    std::vector<std::filesystem::path> allowed_write_paths;
    std::vector<std::string> allowed_programs;
    std::vector<aiagent::CommandRecipe> command_recipes;
    std::string policy_fingerprint;
    std::size_t max_turns = 12;
    std::size_t max_context_bytes = 24 * 1024;
    long max_seconds = 0;
    std::string question;
};

void print_help(const char* program) {
    std::cout << "最小 C++ Coding Agent\n\n"
              << "用法:\n"
              << "  " << program << " --demo [问题]\n"
              << "  " << program
              << " [--allow-write] [--allow-write-path 路径] [--allow-command 程序] "
                 "[--require-verification]"
                 " [--approve-each-command] [--approve-each-changeset] [--max-seconds 秒]"
                 " [--unsafe-no-command-sandbox] [--max-context-bytes 字节]"
                 " [--events-jsonl 路径] [--session 路径] [--resume] [--retry-inflight] [--json]"
                 " [--config JSON路径] [--policy JSON路径] [--root 路径]"
                 " [--max-turns 数量] [问题]\n\n"
              << "选项:\n"
              << "  --demo            离线演示 Agent 循环，不请求真实模型\n"
              << "  --allow-write     显式允许创建文件和精确替换文本，默认关闭\n"
              << "  --allow-write-path  将 apply_patch 限制到相对文件或已有目录，可重复\n"
              << "  --allow-command   显式授权一个可执行程序，可重复传入；不经过 shell\n"
              << "  --approve-each-command  每次命令启动前在终端逐次确认\n"
              << "  --approve-each-changeset  每次多文件事务提交前显示 diff 并确认\n"
              << "  --unsafe-no-command-sandbox  显式接受风险并关闭命令 OS 沙箱\n"
              << "  --require-verification  修改后的最新命令必须成功才能结束\n"
              << "  --max-seconds 秒  本次运行总墙钟预算，1 到 86400；默认不限制\n"
              << "  --events-jsonl 路径  写入脱敏的结构化运行事件\n"
              << "  --session 路径  持久化稳定检查点\n"
              << "  --resume          从 --session 的最后检查点继续，不接受新问题\n"
              << "  --retry-inflight  恢复时显式重试未确认完成的副作用工具；默认安全阻断\n"
              << "  --json            stdout 只输出机器可读最终 JSON\n"
              << "  --config 路径     模型配置文件，默认 ./config.json\n"
              << "  --policy 路径     显式采用非密钥 task policy；不能与能力/预算参数混用\n"
              << "  --root 路径       限定 Agent 可以访问的工作目录\n"
              << "  --max-turns 数量  本次运行最多模型调用轮数，默认 12\n"
              << "  --max-context-bytes 字节  发给模型的上下文上限，16384 到 8388608；默认 24576\n"
              << "  --version         显示版本\n"
              << "  -h, --help        显示帮助\n\n"
              << "真实模型配置示例:\n"
              << "  {\"api_url\": \"...\", \"api_key\": \"...\", \"model\": \"...\"}\n";
}

unsigned long parse_unsigned(const std::string& text, const std::string& option) {
    std::size_t consumed = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(text, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument(option + " 后面需要一个正整数");
    }
    if (consumed != text.size()) {
        throw std::invalid_argument(option + " 后面需要一个正整数");
    }
    return value;
}

std::filesystem::path normalized_path(std::filesystem::path path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(std::move(path), error);
    if (error) {
        throw std::invalid_argument("无法解析运行时文件路径");
    }
    auto resolved = std::filesystem::weakly_canonical(absolute, error);
    if (!error) {
        return resolved;
    }
    return absolute.lexically_normal();
}

CommandLine parse_arguments(int argc, char** argv) {
    CommandLine result;
    std::vector<std::string> question_parts;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--demo") {
            result.demo = true;
        } else if (argument == "--allow-write") {
            result.allow_write = true;
            result.policy_conflict = true;
        } else if (argument == "--allow-write-path") {
            if (++index >= argc) {
                throw std::invalid_argument("--allow-write-path 后面需要一个相对路径");
            }
            result.allowed_write_paths.emplace_back(argv[index]);
            result.policy_conflict = true;
        } else if (argument == "--allow-command") {
            if (++index >= argc) {
                throw std::invalid_argument("--allow-command 后面需要程序名称或绝对路径");
            }
            result.allowed_programs.emplace_back(argv[index]);
            result.policy_conflict = true;
        } else if (argument == "--approve-each-command") {
            result.approve_each_command = true;
        } else if (argument == "--approve-each-changeset") {
            result.approve_each_changeset = true;
        } else if (argument == "--unsafe-no-command-sandbox") {
            result.unsafe_no_command_sandbox = true;
            result.policy_conflict = true;
        } else if (argument == "--require-verification") {
            result.require_verification = true;
            result.policy_conflict = true;
        } else if (argument == "--json") {
            result.json_output = true;
        } else if (argument == "--resume") {
            result.resume = true;
        } else if (argument == "--retry-inflight") {
            result.retry_inflight = true;
        } else if (argument == "-h" || argument == "--help") {
            result.help = true;
        } else if (argument == "--version") {
            result.version = true;
        } else if (argument == "--config") {
            if (++index >= argc) {
                throw std::invalid_argument("--config 后面需要一个 JSON 文件路径");
            }
            result.config = argv[index];
        } else if (argument == "--policy") {
            if (++index >= argc) {
                throw std::invalid_argument("--policy 后面需要一个 JSON 文件路径");
            }
            result.policy = argv[index];
        } else if (argument == "--root") {
            if (++index >= argc) {
                throw std::invalid_argument("--root 后面需要一个路径");
            }
            result.root = argv[index];
        } else if (argument == "--events-jsonl") {
            if (++index >= argc) {
                throw std::invalid_argument("--events-jsonl 后面需要一个文件路径");
            }
            result.events_jsonl = argv[index];
        } else if (argument == "--session") {
            if (++index >= argc) {
                throw std::invalid_argument("--session 后面需要一个文件路径");
            }
            result.session = argv[index];
        } else if (argument == "--max-turns") {
            if (++index >= argc) {
                throw std::invalid_argument("--max-turns 后面需要一个数字");
            }
            const auto parsed = parse_unsigned(argv[index], "--max-turns");
            if (parsed == 0 || parsed > 50) {
                throw std::invalid_argument("--max-turns 必须在 1 到 50 之间");
            }
            result.max_turns = parsed;
            result.policy_conflict = true;
        } else if (argument == "--max-seconds") {
            if (++index >= argc) {
                throw std::invalid_argument("--max-seconds 后面需要一个数字");
            }
            const auto parsed = parse_unsigned(argv[index], "--max-seconds");
            if (parsed == 0 || parsed > 86400) {
                throw std::invalid_argument("--max-seconds 必须在 1 到 86400 之间");
            }
            result.max_seconds = static_cast<long>(parsed);
            result.policy_conflict = true;
        } else if (argument == "--max-context-bytes") {
            if (++index >= argc) {
                throw std::invalid_argument("--max-context-bytes 后面需要一个数字");
            }
            const auto parsed = parse_unsigned(argv[index], "--max-context-bytes");
            if (parsed < 16 * 1024 || parsed > 8 * 1024 * 1024) {
                throw std::invalid_argument("--max-context-bytes 必须在 16384 到 8388608 之间");
            }
            result.max_context_bytes = parsed;
            result.policy_conflict = true;
        } else if (argument.starts_with('-')) {
            throw std::invalid_argument("未知选项: " + argument);
        } else {
            question_parts.push_back(argument);
        }
    }

    for (const auto& part : question_parts) {
        if (!result.question.empty()) {
            result.question += ' ';
        }
        result.question += part;
    }
    return result;
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

bool requested_json_output(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--json") {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    const bool error_as_json = requested_json_output(argc, argv);
    try {
        auto command_line = parse_arguments(argc, argv);
        if (command_line.help) {
            print_help(argv[0]);
            return 0;
        }
        if (command_line.version) {
            std::cout << "aiagent " << aiagent::version << '\n';
            return 0;
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
        const bool commands_enabled =
            !command_line.allowed_programs.empty() || !command_line.command_recipes.empty();
        if (command_line.resume && command_line.session.empty()) {
            throw std::invalid_argument("--resume 必须与 --session 一起使用");
        }
        if (command_line.retry_inflight && !command_line.resume) {
            throw std::invalid_argument("--retry-inflight 必须与 --resume 一起使用");
        }
        if (command_line.resume && command_line.demo) {
            throw std::invalid_argument("离线固定脚本不支持 --resume");
        }
        if (command_line.resume && !command_line.question.empty()) {
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

        if (!command_line.resume && command_line.question.empty()) {
            if (command_line.json_output) {
                throw std::invalid_argument("--json 模式需要在命令行提供任务内容");
            }
            std::cout << "你想让 Agent 完成什么？ ";
            std::getline(std::cin, command_line.question);
        }
        if (!command_line.resume && command_line.question.empty()) {
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

        const auto normalized_config = normalized_path(command_line.config);
        const auto normalized_policy = command_line.policy.empty()
                                           ? std::filesystem::path{}
                                           : normalized_path(command_line.policy);
        const auto normalized_session = command_line.session.empty()
                                            ? std::filesystem::path{}
                                            : normalized_path(command_line.session);
        const auto normalized_events = command_line.events_jsonl.empty()
                                           ? std::filesystem::path{}
                                           : normalized_path(command_line.events_jsonl);
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
            event_log =
                std::make_unique<aiagent::EventLog>(command_line.events_jsonl, command_line.resume);
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
            if (task_policy.has_value()) {
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
                      << "会话模式: " << (command_line.resume ? "恢复" : "新任务") << '\n';
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
                                  .resume_session = command_line.resume,
                                  .retry_in_flight_tool = command_line.retry_inflight});
        const auto result = agent.run(command_line.resume ? "" : command_line.question);
        if (command_line.json_output) {
            std::cout << aiagent::agent_result_to_json(result).dump() << '\n';
        }
        return exit_code_for(result);
    } catch (const std::exception& error) {
        if (error_as_json) {
            std::cout << aiagent::Json{{"schema_version", 1},
                                       {"status", "error"},
                                       {"completed", false},
                                       {"error", error.what()}}
                             .dump()
                      << '\n';
        } else {
            std::cerr << "错误: " << error.what() << '\n';
        }
        return 1;
    }
}
