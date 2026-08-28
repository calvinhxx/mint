#pragma once

#include "mint/application/agent.hpp"
#include "mint/domain/task_policy.hpp"
#include "mint/infrastructure/event_log.hpp"
#include "mint/infrastructure/model_provider_client.hpp"
#include "mint/infrastructure/project_store.hpp"
#include "mint/infrastructure/session_store.hpp"
#include "mint/runtime/task_control.hpp"
#include "mint/tools/tool_registry.hpp"

#include "command_line.hpp"
#include "console.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace mint::cli::command_detail {

struct RuntimeFiles {
    std::unique_ptr<SessionStore> session;
    std::unique_ptr<EventLog> events;
    std::filesystem::path change_transaction;
};

[[nodiscard]] int exit_code_for(const AgentResult& result);
void ensure_question(CommandLine& command_line, Console& console);
[[nodiscard]] CommandApproval command_approval(Console& console);
[[nodiscard]] ChangeSetApproval change_set_approval(Console& console);

[[nodiscard]] std::optional<TaskPolicy> apply_task_policy(CommandLine& command_line);
void force_managed_demo_read_only(CommandLine& command_line, bool managed_demo);
[[nodiscard]] bool commands_enabled(const CommandLine& command_line);
void validate_execution_options(const CommandLine& command_line, bool has_commands);
void validate_runtime_paths(const CommandLine& command_line);
[[nodiscard]] RuntimeFiles open_runtime_files(const CommandLine& command_line);
[[nodiscard]] std::vector<std::filesystem::path>
protected_paths(const CommandLine& command_line, const std::optional<TaskPolicy>& task_policy,
                const RuntimeFiles& files, const std::optional<ProjectStore>& project_store,
                const std::optional<ManagedTaskPaths>& managed_task);
[[nodiscard]] ToolRegistryOptions
tool_options(const CommandLine& command_line, std::vector<std::filesystem::path> protected_files,
             const std::shared_ptr<TaskControl>& task_control, bool has_commands,
             std::filesystem::path change_transaction_path, Console& console);

[[nodiscard]] std::unique_ptr<ModelClient>
create_model(const CommandLine& command_line, const std::shared_ptr<TaskControl>& task_control,
             EventLog* event_log, Console& console);
void print_run_configuration(const CommandLine& command_line, const ToolRegistry& tools,
                             const std::optional<TaskPolicy>& task_policy,
                             const std::optional<ManagedTaskPaths>& managed_task, bool managed_demo,
                             Console& console);

} // namespace mint::cli::command_detail
