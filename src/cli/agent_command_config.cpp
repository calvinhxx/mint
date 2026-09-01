#include "agent_command_internal.hpp"

#include "mint/infrastructure/change_transaction_store.hpp"

#include <stdexcept>
#include <utility>

namespace mint::cli::command_detail {

std::optional<TaskPolicy> apply_task_policy(CommandLine& command_line) {
    if (command_line.policy.empty()) {
        return std::nullopt;
    }
    if (command_line.policy_conflict) {
        throw std::invalid_argument(
            "--policy 不能与 --allow-write、--allow-command、--require-verification、"
            "--unsafe-no-command-sandbox 或预算参数混用");
    }

    auto policy = load_task_policy(command_line.policy);
    command_line.allow_write = !policy.write_paths.empty();
    command_line.allowed_write_paths = policy.write_paths;
    command_line.command_read_paths = policy.command_read_paths;
    command_line.command_recipes = policy.recipes;
    command_line.require_verification = policy.require_verification;
    command_line.max_turns = policy.max_turns;
    command_line.max_context_bytes = policy.max_context_bytes;
    command_line.max_seconds = policy.max_seconds;
    command_line.tool_limits = policy.tool_limits;
    command_line.policy_fingerprint = policy.fingerprint;
    return policy;
}

void force_managed_demo_read_only(CommandLine& command_line, bool managed_demo) {
    if (!managed_demo) {
        return;
    }
    command_line.allow_write = false;
    command_line.allowed_write_paths.clear();
    command_line.command_read_paths.clear();
    command_line.command_recipes.clear();
    command_line.require_verification = false;
    command_line.policy_fingerprint.clear();
}

bool commands_enabled(const CommandLine& command_line) {
    return !command_line.allowed_programs.empty() || !command_line.command_recipes.empty();
}

void validate_execution_options(const CommandLine& command_line, bool has_commands) {
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
    if (command_line.approve_each_command && !has_commands) {
        throw std::invalid_argument("--approve-each-command 需要 --allow-command 或 policy recipe");
    }
    if (!command_line.allowed_write_paths.empty() && !command_line.allow_write) {
        throw std::invalid_argument("--allow-write-path 需要同时传入 --allow-write");
    }
    if (command_line.approve_each_changeset && !command_line.allow_write) {
        throw std::invalid_argument("--approve-each-changeset 需要启用写入能力");
    }
    if (command_line.unsafe_no_command_sandbox && !has_commands) {
        throw std::invalid_argument("--unsafe-no-command-sandbox 只在同时授权命令时有意义");
    }
    if (!command_line.command_read_paths.empty() && !has_commands) {
        throw std::invalid_argument("command_read_paths 需要至少一个命令或 recipe");
    }
    if (command_line.require_verification && (!command_line.allow_write || !has_commands)) {
        throw std::invalid_argument("验证门禁需要同时启用写入和至少一个命令或 recipe");
    }
}

void validate_runtime_paths(const CommandLine& command_line) {
    const auto config = normalized_path(command_line.config);
    const auto policy = command_line.policy.empty() ? std::filesystem::path{}
                                                    : normalized_path(command_line.policy);
    const auto session = command_line.session.empty() ? std::filesystem::path{}
                                                      : normalized_path(command_line.session);
    const auto events = command_line.events_jsonl.empty()
                            ? std::filesystem::path{}
                            : normalized_path(command_line.events_jsonl);
    const auto transaction =
        session.empty() ? std::filesystem::path{} : change_transaction_path_for_session(session);
    const auto transaction_lock =
        transaction.empty() ? std::filesystem::path{} : change_transaction_lock_path(transaction);

    if (!policy.empty() && policy == config) {
        throw std::invalid_argument("task policy 与模型配置必须使用不同文件");
    }
    if ((!session.empty() && session == config) || (!events.empty() && events == config)) {
        throw std::invalid_argument("会话或事件路径不能与模型配置文件相同");
    }
    if (!session.empty() && session == events) {
        throw std::invalid_argument("会话快照与事件日志必须使用不同文件");
    }
    if ((!policy.empty() && policy == session) || (!policy.empty() && policy == events)) {
        throw std::invalid_argument("task policy 不能与会话或事件文件共用路径");
    }
    if ((!transaction.empty() && (transaction == config || transaction_lock == config)) ||
        (!policy.empty() && (policy == transaction || policy == transaction_lock)) ||
        (!events.empty() && (events == transaction || events == transaction_lock))) {
        throw std::invalid_argument("changeset 事务文件不能与其他运行时文件共用路径");
    }
}

RuntimeFiles open_runtime_files(const CommandLine& command_line) {
    RuntimeFiles files;
    if (!command_line.session.empty()) {
        files.session = std::make_unique<SessionStore>(command_line.session);
        if (!command_line.resume_session && files.session->exists()) {
            throw std::invalid_argument("会话快照已存在；请使用 --resume 或选择新的路径");
        }
        files.change_transaction = change_transaction_path_for_session(files.session->path());
    }
    if (!command_line.events_jsonl.empty()) {
        files.events =
            std::make_unique<EventLog>(command_line.events_jsonl, command_line.resume_session);
    }
    return files;
}

std::vector<std::filesystem::path>
protected_paths(const CommandLine& command_line, const std::optional<TaskPolicy>& task_policy,
                const RuntimeFiles& files, const std::optional<ProjectStore>& project_store,
                const std::optional<ManagedTaskPaths>& managed_task) {
    std::vector<std::filesystem::path> paths = {command_line.config};
    if (task_policy.has_value()) {
        paths.push_back(task_policy->source_path);
    }
    if (files.session != nullptr) {
        paths.push_back(files.session->path());
    }
    if (files.events != nullptr) {
        paths.push_back(files.events->path());
    }
    if (!files.change_transaction.empty()) {
        paths.push_back(files.change_transaction);
        paths.push_back(change_transaction_lock_path(files.change_transaction));
    }
    if (managed_task.has_value()) {
        paths.push_back(project_store->project_directory());
    }
    return paths;
}

ToolRegistryOptions tool_options(const CommandLine& command_line,
                                 std::vector<std::filesystem::path> protected_files,
                                 const std::shared_ptr<TaskControl>& task_control,
                                 bool has_commands, std::filesystem::path change_transaction_path,
                                 EventLog* event_log, Console& console) {
    return {
        .protected_paths = std::move(protected_files),
        .allow_write = command_line.allow_write,
        .allowed_write_paths = command_line.allowed_write_paths,
        .command_read_paths = command_line.command_read_paths,
        .allowed_programs = command_line.allowed_programs,
        .command_recipes = command_line.command_recipes,
        .policy_fingerprint = command_line.policy_fingerprint,
        .task_control = task_control,
        .command_approval =
            command_line.approve_each_command
                ? command_approval(console, command_line.interaction_jsonl, event_log, task_control)
                : CommandApproval{},
        .change_set_approval = command_line.approve_each_changeset
                                   ? change_set_approval(console, command_line.interaction_jsonl,
                                                         event_log, task_control)
                                   : ChangeSetApproval{},
        .require_command_sandbox = has_commands && !command_line.unsafe_no_command_sandbox,
        .runtime = command_line.tool_limits,
        .change_transaction_path = std::move(change_transaction_path)};
}

} // namespace mint::cli::command_detail
