#include "agent_command.hpp"

#include "agent_command_internal.hpp"
#include "project_commands.hpp"

#include <chrono>
#include <csignal>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mint::cli {
namespace {

volatile std::sig_atomic_t interrupt_requested = 0;

void handle_interrupt(int) {
    interrupt_requested = 1;
}

std::optional<int> prepare_managed_mode(CommandLine& command_line,
                                        std::optional<ProjectStore>& project_store,
                                        std::optional<ManagedTaskPaths>& managed_task,
                                        Console& console) {
    if (!is_managed_mode(command_line.mode)) {
        return std::nullopt;
    }

    project_store.emplace(command_line.root, command_line.state_dir);
    if (command_line.mode == CommandMode::init) {
        return handle_init_command(command_line, *project_store, console);
    }
    if (command_line.mode == CommandMode::status) {
        return handle_status_command(command_line, *project_store, console);
    }

    (void)project_store->load_profile();
    if (command_line.mode == CommandMode::run) {
        command_detail::ensure_question(command_line, console);
        managed_task = project_store->create_task(command_line.question,
                                                  command_line.demo ? ManagedTaskMode::demo
                                                                    : ManagedTaskMode::model);
    } else if (!command_line.task_id.empty()) {
        const auto summary = project_store->task_summary(command_line.task_id);
        if (!summary.has_value()) {
            throw std::runtime_error("找不到任务: " + command_line.task_id);
        }
        if (summary->mode == ManagedTaskMode::demo) {
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

    command_line.policy = managed_task->policy;
    command_line.session = managed_task->session;
    command_line.events_jsonl = managed_task->events;
    command_line.resume_session = command_line.mode == CommandMode::resume;
    return std::nullopt;
}

} // namespace

int run_agent_command(CommandLine command_line, std::optional<ManagedTaskPaths>& managed_task,
                      Console& console) {
    std::optional<ProjectStore> project_store;
    if (const auto exit =
            prepare_managed_mode(command_line, project_store, managed_task, console)) {
        return *exit;
    }

    auto task_policy = command_detail::apply_task_policy(command_line);
    const bool managed_demo = managed_task.has_value() && command_line.demo;
    command_detail::force_managed_demo_read_only(command_line, managed_demo);

    const bool has_commands = command_detail::commands_enabled(command_line);
    command_detail::validate_execution_options(command_line, has_commands);
    if (!command_line.resume_session) {
        command_detail::ensure_question(command_line, console);
    }

    interrupt_requested = 0;
    std::signal(SIGINT, handle_interrupt);
    const auto total_budget = command_line.max_seconds == 0
                                  ? std::chrono::milliseconds::zero()
                                  : std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::seconds(command_line.max_seconds));
    auto task_control = std::make_shared<TaskControl>(total_budget, &interrupt_requested);

    command_detail::validate_runtime_paths(command_line);
    auto files = command_detail::open_runtime_files(command_line);
    ToolRegistry tools(
        command_line.root,
        command_detail::tool_options(
            command_line,
            command_detail::protected_paths(command_line, task_policy, files, project_store,
                                            managed_task),
            task_control, has_commands,
            command_line.allow_write ? files.change_transaction : std::filesystem::path{},
            console));
    auto model =
        command_detail::create_model(command_line, task_control, files.events.get(), console);
    command_detail::print_run_configuration(command_line, tools, task_policy, managed_task,
                                            managed_demo, console);

    std::ostringstream machine_mode_log;
    std::ostream& agent_output = command_line.json_output
                                     ? static_cast<std::ostream&>(machine_mode_log)
                                     : console.output_stream();
    Agent agent(*model, tools, agent_output,
                AgentOptions{.max_turns = command_line.max_turns,
                             .max_context_bytes = command_line.max_context_bytes,
                             .require_verification_after_write = command_line.require_verification,
                             .task_control = task_control,
                             .event_log = files.events.get(),
                             .session_store = files.session.get(),
                             .resume_session = command_line.resume_session,
                             .retry_in_flight_tool = command_line.retry_inflight});
    const auto result = agent.run(command_line.resume_session ? "" : command_line.question);
    if (command_line.json_output) {
        auto machine_result = agent_result_to_json(result);
        if (managed_task.has_value()) {
            machine_result["task_id"] = managed_task->id;
            machine_result["task_directory"] = managed_task->directory.generic_string();
        }
        console.write_line(machine_result.dump());
    }
    return command_detail::exit_code_for(result);
}

} // namespace mint::cli
