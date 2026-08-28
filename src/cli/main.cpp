#include "agent_command.hpp"
#include "command_line.hpp"
#include "console.hpp"
#include "provider_command.hpp"

#include "mint/domain/model.hpp"
#include "mint/version.hpp"

#include "../infrastructure/diagnostic_log.hpp"

#include <exception>
#include <optional>
#include <utility>

int main(int argc, char** argv) {
    const bool error_as_json = mint::cli::requested_json_output(argc, argv);
    std::optional<mint::ManagedTaskPaths> managed_task;
    auto console = mint::cli::system_console();
    try {
        auto command_line = mint::cli::parse_arguments(argc, argv);
        mint::diagnostics::configure(command_line.log_level);
        if (command_line.help) {
            mint::cli::print_help(console, argv[0]);
            return 0;
        }
        if (command_line.version) {
            console.write_line("mint ", mint::version);
            return 0;
        }
        if (command_line.mode == mint::cli::CommandMode::provider) {
            return mint::cli::run_provider_command(command_line, console);
        }
        return mint::cli::run_agent_command(std::move(command_line), managed_task, console);
    } catch (const std::exception& error) {
        if (error_as_json) {
            mint::Json result = {{"schema_version", 1},
                                 {"status", "error"},
                                 {"completed", false},
                                 {"error", error.what()}};
            if (managed_task.has_value()) {
                result["task_id"] = managed_task->id;
                result["task_directory"] = managed_task->directory.generic_string();
            }
            console.write_line(result.dump());
        } else {
            console.write_error_line("错误: ", error.what());
            if (managed_task.has_value()) {
                console.write_error_line("任务 ID: ", managed_task->id);
            }
        }
        return 1;
    }
}
