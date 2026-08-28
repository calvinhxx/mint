#include "command_line.hpp"
#include "console.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mint::cli {
namespace {

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

CommandMode parse_mode(const std::string& argument) {
    if (argument == "init") {
        return CommandMode::init;
    }
    if (argument == "run") {
        return CommandMode::run;
    }
    if (argument == "resume") {
        return CommandMode::resume;
    }
    if (argument == "status") {
        return CommandMode::status;
    }
    if (argument == "provider") {
        return CommandMode::provider;
    }
    return CommandMode::legacy;
}

} // namespace

bool is_managed_mode(CommandMode mode) noexcept {
    return mode == CommandMode::init || mode == CommandMode::run || mode == CommandMode::resume ||
           mode == CommandMode::status;
}

void print_help(Console& console, const char* program) {
    console.output_stream()
        << "mint - Lightweight General AI Agent\n\n"
        << "日常工作流:\n"
        << "  " << program << " init [--root 路径] [--state-dir 路径] [--force] [--json]\n"
        << "  " << program
        << " run [--root 路径] [--state-dir 路径] [--config 路径] [--demo] [任务]\n"
        << "  " << program
        << " resume [--root 路径] [--state-dir 路径] [--task ID] [--config 路径]\n"
        << "  " << program << " status [--root 路径] [--state-dir 路径] [--task ID] [--json]\n\n"
        << "模型配置:\n"
        << "  " << program << " provider [--config 路径] [--json]  离线检查 provider 能力\n"
        << "  " << program << " provider test [--config 路径] [--json]  发送两轮真实兼容性测试\n\n"
        << "兼容工作流:\n"
        << "  " << program << " --demo [问题]\n"
        << "  " << program
        << " [--allow-write] [--allow-write-path 路径] [--allow-command 程序]"
           " [--require-verification] [--approve-each-command] [--approve-each-changeset]"
           " [--max-seconds 秒] [--unsafe-no-command-sandbox] [--max-context-bytes 字节]"
           " [--log-level 级别]"
           " [--events-jsonl 路径] [--session 路径] [--resume] [--retry-inflight] [--json]"
           " [--config JSON路径] [--policy JSON路径] [--root 路径] [--max-turns 数量] [问题]\n\n"
        << "日常选项:\n"
        << "  --state-dir 路径  覆盖工作区外的本地项目/任务状态目录\n"
        << "  --task ID         status/resume 使用指定任务；resume 默认选择最近可恢复任务\n"
        << "  --force           init 时重新生成项目建议；已有任务及其 policy 快照保留\n"
        << "  --demo            离线演示 Agent 循环，不请求真实模型\n"
        << "  --json            stdout 只输出机器可读 JSON\n"
        << "  --config 路径     模型配置文件，默认 ./config.json\n"
        << "  --log-level 级别  诊断日志级别：trace/debug/info/warn/error/critical/off；默认 warn\n"
        << "  --root 路径       限定 Agent 可以访问的工作目录\n\n"
        << "能力与恢复选项:\n"
        << "  --policy 路径     显式采用 task policy；兼容工作流使用\n"
        << "  --allow-write     显式允许文本写入，默认关闭\n"
        << "  --allow-write-path  限制到相对文件或已有目录，可重复\n"
        << "  --allow-command   显式授权一个可执行程序，可重复；不经过 shell\n"
        << "  --approve-each-command  每次命令启动前确认\n"
        << "  --approve-each-changeset  每次多文件事务提交前显示 diff 并确认\n"
        << "  --unsafe-no-command-sandbox  显式接受风险并关闭命令 OS 沙箱\n"
        << "  --require-verification  最新修改后必须有 eligible 命令成功\n"
        << "  --session 路径    持久化兼容工作流检查点\n"
        << "  --resume          恢复兼容工作流的 --session\n"
        << "  --retry-inflight  显式重试未确认完成的副作用工具\n"
        << "  --events-jsonl 路径  写入脱敏事件\n"
        << "  --max-turns 数量  最大模型轮数，" << runtime_bounds::min_turns << " 到 "
        << runtime_bounds::max_turns << "\n"
        << "  --max-seconds 秒  总墙钟预算，1 到 " << runtime_bounds::max_seconds << "\n"
        << "  --max-context-bytes 字节  模型上下文上限，" << runtime_bounds::min_context_bytes
        << " 到 " << runtime_bounds::max_context_bytes << "\n"
        << "  --version         显示版本\n"
        << "  -h, --help        显示帮助\n";
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
    int begin = 1;
    if (argc > 1) {
        result.mode = parse_mode(argv[1]);
        if (result.mode != CommandMode::legacy) {
            begin = 2;
        }
    }
    if (result.mode == CommandMode::resume) {
        result.resume_session = true;
    }
    if (result.mode == CommandMode::provider && argc > 2 && std::string(argv[2]) == "test") {
        result.provider_action = ProviderCommandAction::test;
        begin = 3;
    }

    for (int index = begin; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--demo") {
            result.demo = true;
        } else if (argument == "--force") {
            result.force = true;
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
            result.resume_session = true;
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
            result.config_specified = true;
        } else if (argument == "--log-level") {
            if (++index >= argc) {
                throw std::invalid_argument("--log-level 后面需要日志级别");
            }
            result.log_level = argv[index];
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
            result.root_specified = true;
        } else if (argument == "--state-dir") {
            if (++index >= argc) {
                throw std::invalid_argument("--state-dir 后面需要一个路径");
            }
            result.state_dir = argv[index];
        } else if (argument == "--task") {
            if (++index >= argc) {
                throw std::invalid_argument("--task 后面需要任务 ID");
            }
            result.task_id = argv[index];
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
            if (parsed < runtime_bounds::min_turns || parsed > runtime_bounds::max_turns) {
                throw std::invalid_argument("--max-turns 超出允许范围");
            }
            result.max_turns = parsed;
            result.policy_conflict = true;
        } else if (argument == "--max-seconds") {
            if (++index >= argc) {
                throw std::invalid_argument("--max-seconds 后面需要一个数字");
            }
            const auto parsed = parse_unsigned(argv[index], "--max-seconds");
            if (parsed == 0 || parsed > static_cast<unsigned long>(runtime_bounds::max_seconds)) {
                throw std::invalid_argument("--max-seconds 超出允许范围");
            }
            result.max_seconds = static_cast<long>(parsed);
            result.policy_conflict = true;
        } else if (argument == "--max-context-bytes") {
            if (++index >= argc) {
                throw std::invalid_argument("--max-context-bytes 后面需要一个数字");
            }
            const auto parsed = parse_unsigned(argv[index], "--max-context-bytes");
            if (parsed < runtime_bounds::min_context_bytes ||
                parsed > runtime_bounds::max_context_bytes) {
                throw std::invalid_argument("--max-context-bytes 超出允许范围");
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

    if (result.mode == CommandMode::legacy) {
        if (!result.state_dir.empty() || !result.task_id.empty() || result.force) {
            throw std::invalid_argument("--state-dir、--task 和 --force 只用于日常子命令");
        }
    } else if (result.mode == CommandMode::provider) {
        if (!result.question.empty() || result.demo || result.force || result.resume_session ||
            result.retry_inflight || result.approve_each_command || result.approve_each_changeset ||
            result.policy_conflict || !result.policy.empty() || !result.session.empty() ||
            !result.events_jsonl.empty() || !result.state_dir.empty() || !result.task_id.empty() ||
            result.root_specified) {
            throw std::invalid_argument(
                "provider 和 provider test 只接受 --config、--json 和 --log-level");
        }
    } else {
        if (!result.policy.empty() || !result.session.empty() || !result.events_jsonl.empty() ||
            (result.mode != CommandMode::resume && result.resume_session)) {
            throw std::invalid_argument(
                "日常子命令自动管理 policy/session/events；不要混用兼容工作流参数");
        }
        if (result.policy_conflict) {
            throw std::invalid_argument(
                "日常子命令从项目 profile 获取能力；不能追加原始能力或预算参数");
        }
        if (result.force && result.mode != CommandMode::init) {
            throw std::invalid_argument("--force 只用于 init");
        }
        if (!result.task_id.empty() && result.mode != CommandMode::resume &&
            result.mode != CommandMode::status) {
            throw std::invalid_argument("--task 只用于 resume 或 status");
        }
        if ((result.mode == CommandMode::init || result.mode == CommandMode::status ||
             result.mode == CommandMode::resume) &&
            !result.question.empty()) {
            throw std::invalid_argument("该子命令不接受任务正文");
        }
        if ((result.mode == CommandMode::init || result.mode == CommandMode::status) &&
            (result.demo || result.retry_inflight || result.approve_each_command ||
             result.approve_each_changeset || result.config_specified)) {
            throw std::invalid_argument("init/status 不接受模型、执行或恢复选项");
        }
    }
    return result;
}

bool requested_json_output(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--json") {
            return true;
        }
    }
    return false;
}

} // namespace mint::cli
