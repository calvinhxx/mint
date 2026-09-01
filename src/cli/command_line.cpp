#include "command_line.hpp"
#include "console.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
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

std::filesystem::path utf8_path(std::string_view argument) {
#if defined(_WIN32)
    std::u8string encoded;
    encoded.reserve(argument.size());
    for (const unsigned char byte : argument) {
        encoded.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path(encoded);
#else
    return std::filesystem::path(argument);
#endif
}

} // namespace

bool is_managed_mode(CommandMode mode) noexcept {
    return mode == CommandMode::init || mode == CommandMode::run || mode == CommandMode::resume ||
           mode == CommandMode::status;
}

void print_help(Console& console, const char* program) {
    console.output_stream()
        << "mint - Lightweight General AI Agent\n\n"
        << "用法:\n"
        << "  " << program << " init [--root 路径] [--state-dir 路径] [--force] [--json]\n"
        << "  " << program
        << " run [--root 路径] [--state-dir 路径] [--config 路径] [--demo] [任务]\n"
        << "  " << program
        << " resume [--root 路径] [--state-dir 路径] [--task ID] [--config 路径]\n"
        << "  " << program << " status [--root 路径] [--state-dir 路径] [--task ID] [--json]\n\n"
        << "模型配置:\n"
        << "  " << program << " provider [--config 路径] [--json]  离线检查 provider 能力\n"
        << "  " << program << " provider test [--config 路径] [--json]  发送两轮真实兼容性测试\n\n"
        << "常用选项:\n"
        << "  --root 路径       项目目录，默认当前目录\n"
        << "  --state-dir 路径  覆盖工作区外的本地状态目录\n"
        << "  --config 路径     模型配置，默认 ./config.json\n"
        << "  --task ID         恢复或查看指定任务\n"
        << "  --demo            离线运行，不请求真实模型\n"
        << "  --json            输出机器可读 JSON\n"
        << "  --log-level 级别  终端日志级别，默认 warn\n"
        << "  --log-file-level 级别  文件日志级别；off 关闭落盘\n"
        << "  --version         显示版本\n"
        << "  -h, --help        显示帮助\n"
        << "  --help-legacy     显示旧版兼容模式\n";
}

void print_legacy_help(Console& console, const char* program) {
    console.output_stream()
        << "mint 旧版兼容模式\n\n"
        << "现有脚本仍可使用扁平参数；新任务建议使用 init / run / resume / status。\n\n"
        << "用法:\n"
        << "  " << program
        << " [--allow-write] [--allow-write-path 路径] [--allow-command 程序]"
           " [--require-verification] [--approve-each-command] [--approve-each-changeset]"
           " [--max-seconds 秒] [--unsafe-no-command-sandbox] [--max-context-bytes 字节]"
           " [--max-total-tokens 数量]"
           " [--log-level 级别] [--log-file-level 级别] [--log-dir 路径]"
           " [--events-jsonl 路径] [--session 路径] [--resume] [--retry-inflight] [--json]"
           " [--config JSON路径] [--policy JSON路径] [--root 路径] [--max-turns 数量] [问题]\n\n"
        << "能力与恢复:\n"
        << "  --allow-write             允许文本写入\n"
        << "  --allow-write-path 路径   限制写入范围，可重复\n"
        << "  --allow-command 程序      授权可执行程序，可重复且不经过 shell\n"
        << "  --policy 路径             使用显式 task policy\n"
        << "  --require-verification    修改后必须运行验证命令\n"
        << "  --approve-each-command    每次启动命令前确认\n"
        << "  --approve-each-changeset  每次多文件提交前确认\n"
        << "  --session 路径            保存兼容模式 checkpoint\n"
        << "  --resume                  恢复兼容模式 checkpoint\n"
        << "  --retry-inflight          重试未确认完成的副作用工具\n"
        << "  --unsafe-no-command-sandbox  关闭命令 OS 沙箱\n\n"
        << "预算:\n"
        << "  --max-turns 数量          " << runtime_bounds::min_turns << " 到 "
        << runtime_bounds::max_turns << "\n"
        << "  --max-seconds 秒          1 到 " << runtime_bounds::max_seconds << "\n"
        << "  --max-context-bytes 字节  " << runtime_bounds::min_context_bytes << " 到 "
        << runtime_bounds::max_context_bytes << "\n"
        << "  --max-total-tokens 数量   0（关闭）到 " << runtime_bounds::max_total_tokens << "\n";
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
            result.allowed_write_paths.emplace_back(utf8_path(argv[index]));
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
        } else if (argument == "--interaction-jsonl") {
            result.interaction_jsonl = true;
        } else if (argument == "--cancel-file") {
            if (++index >= argc) {
                throw std::invalid_argument("--cancel-file 后面需要一个文件路径");
            }
            result.cancel_file = utf8_path(argv[index]);
        } else if (argument == "--resume") {
            result.resume_session = true;
        } else if (argument == "--retry-inflight") {
            result.retry_inflight = true;
        } else if (argument == "-h" || argument == "--help") {
            result.help = true;
        } else if (argument == "--help-legacy") {
            result.help = true;
            result.legacy_help = true;
        } else if (argument == "--version") {
            result.version = true;
        } else if (argument == "--config") {
            if (++index >= argc) {
                throw std::invalid_argument("--config 后面需要一个 JSON 文件路径");
            }
            result.config = utf8_path(argv[index]);
            result.config_specified = true;
        } else if (argument == "--log-level") {
            if (++index >= argc) {
                throw std::invalid_argument("--log-level 后面需要日志级别");
            }
            result.log_level = argv[index];
        } else if (argument == "--log-file-level") {
            if (++index >= argc) {
                throw std::invalid_argument("--log-file-level 后面需要日志级别");
            }
            result.log_file_level = argv[index];
        } else if (argument == "--log-dir") {
            if (++index >= argc) {
                throw std::invalid_argument("--log-dir 后面需要目录路径");
            }
            result.log_dir = utf8_path(argv[index]);
        } else if (argument == "--policy") {
            if (++index >= argc) {
                throw std::invalid_argument("--policy 后面需要一个 JSON 文件路径");
            }
            result.policy = utf8_path(argv[index]);
        } else if (argument == "--root") {
            if (++index >= argc) {
                throw std::invalid_argument("--root 后面需要一个路径");
            }
            result.root = utf8_path(argv[index]);
            result.root_specified = true;
        } else if (argument == "--state-dir") {
            if (++index >= argc) {
                throw std::invalid_argument("--state-dir 后面需要一个路径");
            }
            result.state_dir = utf8_path(argv[index]);
        } else if (argument == "--task") {
            if (++index >= argc) {
                throw std::invalid_argument("--task 后面需要任务 ID");
            }
            result.task_id = argv[index];
        } else if (argument == "--events-jsonl") {
            if (++index >= argc) {
                throw std::invalid_argument("--events-jsonl 后面需要一个文件路径");
            }
            result.events_jsonl = utf8_path(argv[index]);
        } else if (argument == "--session") {
            if (++index >= argc) {
                throw std::invalid_argument("--session 后面需要一个文件路径");
            }
            result.session = utf8_path(argv[index]);
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
        } else if (argument == "--max-total-tokens") {
            if (++index >= argc) {
                throw std::invalid_argument("--max-total-tokens 后面需要一个数字");
            }
            const auto parsed = parse_unsigned(argv[index], "--max-total-tokens");
            if (parsed > runtime_bounds::max_total_tokens) {
                throw std::invalid_argument("--max-total-tokens 超出允许范围");
            }
            result.max_total_tokens = parsed;
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
        if (!result.state_dir.empty() || !result.task_id.empty() || !result.cancel_file.empty() ||
            result.force || result.interaction_jsonl) {
            throw std::invalid_argument(
                "--state-dir、--task、--force 和 --interaction-jsonl 只用于日常子命令");
        }
    } else if (result.mode == CommandMode::provider) {
        if (!result.question.empty() || result.demo || result.force || result.resume_session ||
            result.retry_inflight || result.approve_each_command || result.approve_each_changeset ||
            result.policy_conflict || !result.policy.empty() || !result.session.empty() ||
            !result.events_jsonl.empty() || !result.state_dir.empty() || !result.task_id.empty() ||
            result.root_specified || result.interaction_jsonl || !result.cancel_file.empty()) {
            throw std::invalid_argument(
                "provider 和 provider test 只接受 --config、--json 和日志选项");
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
             result.approve_each_changeset || result.config_specified || result.interaction_jsonl ||
             !result.cancel_file.empty())) {
            throw std::invalid_argument("init/status 不接受模型、执行或恢复选项");
        }
    }
    if (result.interaction_jsonl && !result.json_output) {
        throw std::invalid_argument("--interaction-jsonl 必须与 --json 一起使用");
    }
    if (!result.cancel_file.empty() && !result.interaction_jsonl) {
        throw std::invalid_argument("--cancel-file 必须与 --interaction-jsonl 一起使用");
    }
    if (!result.cancel_file.empty()) {
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(result.cancel_file, status_error);
        if ((!status_error && status.type() != std::filesystem::file_type::not_found) ||
            (status_error && status_error != std::errc::no_such_file_or_directory)) {
            throw std::invalid_argument(
                "--cancel-file 启动时必须不存在；Mint 不会覆盖或删除已有路径");
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

bool requested_interaction_output(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--interaction-jsonl") {
            return true;
        }
    }
    return false;
}

} // namespace mint::cli
