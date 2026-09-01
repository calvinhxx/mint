#include "agent_command.hpp"

#include "agent_command_internal.hpp"
#include "agent_event_router.hpp"
#include "project_commands.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mint::cli {
namespace {

volatile std::sig_atomic_t interrupt_requested = 0;

void handle_interrupt(int) {
    interrupt_requested = 1;
}

class CancelFileWatcher final {
  public:
    CancelFileWatcher(const std::filesystem::path& path,
                      const std::shared_ptr<TaskControl>& control)
        : path_(path) {
        if (path_.empty()) {
            return;
        }
        worker_ = std::thread([this, weak_control = std::weak_ptr<TaskControl>(control)] {
            while (!stopping_.load(std::memory_order_relaxed)) {
                std::error_code error;
                if (std::filesystem::exists(path_, error)) {
                    if (const auto current = weak_control.lock()) {
                        current->request_cancel();
                    }
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
        });
    }

    ~CancelFileWatcher() {
        stopping_.store(true, std::memory_order_relaxed);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    CancelFileWatcher(const CancelFileWatcher&) = delete;
    CancelFileWatcher& operator=(const CancelFileWatcher&) = delete;

  private:
    std::filesystem::path path_;
    std::atomic_bool stopping_{false};
    std::thread worker_;
};

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
    if (command_line.interaction_jsonl) {
        command_detail::emit_interaction_message(
            console, "task_ready",
            {{"task_id", managed_task->id},
             {"task_directory", managed_task->directory.generic_string()},
             {"events_path", managed_task->events.generic_string()},
             {"resumed", command_line.resume_session}});
    }
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
#if defined(SIGTERM)
    std::signal(SIGTERM, handle_interrupt);
#endif
    const auto total_budget = command_line.max_seconds == 0
                                  ? std::chrono::milliseconds::zero()
                                  : std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::seconds(command_line.max_seconds));
    auto task_control = std::make_shared<TaskControl>(total_budget, &interrupt_requested);
    CancelFileWatcher cancel_watcher(command_line.cancel_file, task_control);

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
            files.events.get(), console));
    auto model =
        command_detail::create_model(command_line, task_control, files.events.get(), console);
    command_detail::print_run_configuration(command_line, tools, task_policy, managed_task,
                                            managed_demo, console);

    std::ostringstream machine_mode_log;
    std::ostream& agent_output = command_line.json_output
                                     ? static_cast<std::ostream&>(machine_mode_log)
                                     : console.output_stream();
    command_detail::AgentEventRouter event_router(files.events.get());
    Agent agent(*model, tools, agent_output,
                AgentOptions{.max_turns = command_line.max_turns,
                             .max_context_bytes = command_line.max_context_bytes,
                             .max_total_tokens = command_line.max_total_tokens,
                             .require_verification_after_write = command_line.require_verification,
                             .resume_session = command_line.resume_session,
                             .retry_in_flight_tool = command_line.retry_inflight},
                AgentServices{.stop_token = task_control.get(),
                              .event_sink = &event_router,
                              .session_repository = files.session.get()});
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
