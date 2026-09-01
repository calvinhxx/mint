#include "agent/agent_command.hpp"
#include "command_line.hpp"
#include "provider/provider_command.hpp"
#include "support/console.hpp"
#include "support/diagnostic_logging.hpp"

#include "mint/domain/model.hpp"
#include "mint/infrastructure/diagnostic_log.hpp"
#include "mint/localization/localization.hpp"
#include "mint/runtime/terminal_text.hpp"
#include "mint/version.hpp"

#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

namespace localization = mint::localization;
using localization::Message;
using localization::Placeholder;

int run(int argc, char** argv) {
    const bool error_as_json = mint::cli::requested_json_output(argc, argv);
    const bool interaction_requested = mint::cli::requested_interaction_output(argc, argv);
    std::optional<mint::ManagedTaskPaths> managed_task;
    auto console = mint::cli::system_console();
    const auto finish = [](int exit_code) {
        mint::diagnostics::emit(mint::diagnostics::Level::info, "process.finished",
                                {{"exit_code", exit_code}});
        mint::diagnostics::flush();
        mint::diagnostics::shutdown();
        return exit_code;
    };
    try {
        mint::cli::configure_language(argc, argv);
        auto command_line = mint::cli::parse_arguments(argc, argv);
        mint::diagnostics::configure(interaction_requested ? "off" : command_line.log_level);
        mint::diagnostics::validate_level(command_line.log_file_level);
        if (command_line.help) {
            if (command_line.mode == mint::cli::CommandMode::exec) {
                mint::cli::print_exec_help(console, argv[0]);
            } else {
                mint::cli::print_help(console, argv[0]);
            }
            return finish(0);
        }
        if (command_line.version) {
            console.write_line("mint ", mint::version);
            return finish(0);
        }

        (void)mint::cli::configure_diagnostic_logging(command_line, console);

        if (command_line.mode == mint::cli::CommandMode::provider) {
            return finish(mint::cli::run_provider_command(command_line, console));
        }
        return finish(mint::cli::run_agent_command(std::move(command_line), managed_task, console));
    } catch (const std::exception& error) {
        auto log_status = mint::diagnostics::current_status();
        if (log_status.file_enabled) {
            mint::diagnostics::emit(
                mint::diagnostics::Level::error, "process.failed",
                {{"exit_code", 1},
                 {"has_task", managed_task.has_value()},
                 {"task_id", managed_task.has_value() ? managed_task->id : std::string{}}});
            log_status = mint::diagnostics::current_status();
        }
        if (error_as_json) {
            mint::Json result = {{"schema_version", 1},
                                 {"status", "error"},
                                 {"completed", false},
                                 {"error", error.what()}};
            if (managed_task.has_value()) {
                result["task_id"] = managed_task->id;
                result["task_directory"] = managed_task->directory.generic_string();
                result["events_path"] = managed_task->events.generic_string();
            }
            if (log_status.file_enabled) {
                result["diagnostic_log"] = log_status.file_path.generic_string();
            }
            console.write_line(result.dump());
        } else {
            console.write_error_line(mint::localization::message(
                Message::cli_output_error,
                {mint::localization::arg(Placeholder::message,
                                         mint::escape_terminal_field(error.what()))}));
            if (managed_task.has_value()) {
                console.write_error_line(mint::localization::message(
                    Message::cli_output_task_id,
                    {mint::localization::arg(Placeholder::id,
                                             mint::escape_terminal_field(managed_task->id))}));
            }
            if (log_status.file_enabled) {
                console.write_error_line(mint::localization::message(
                    Message::cli_output_diagnostic_log,
                    {mint::localization::arg(
                        Placeholder::path,
                        mint::escape_terminal_field(log_status.file_path.generic_string()))}));
            }
        }
        return finish(1);
    }
}

#if defined(_WIN32)
std::string to_utf8(std::wstring_view argument) {
    if (argument.empty()) {
        return {};
    }
    if (argument.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(
            mint::localization::message(Message::cli_windows_argument_too_long));
    }

    const auto input_size = static_cast<int>(argument.size());
    const auto output_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument.data(),
                                                 input_size, nullptr, 0, nullptr, nullptr);
    if (output_size <= 0) {
        throw std::runtime_error(
            mint::localization::message(Message::cli_windows_utf8_conversion_failed));
    }

    std::string result(static_cast<std::size_t>(output_size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument.data(), input_size,
                            result.data(), output_size, nullptr, nullptr) != output_size) {
        throw std::runtime_error(
            mint::localization::message(Message::cli_windows_utf8_conversion_failed));
    }
    return result;
}
#endif

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> storage;
        storage.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            storage.push_back(to_utf8(argv[index]));
        }

        std::vector<char*> arguments;
        arguments.reserve(storage.size());
        for (auto& argument : storage) {
            arguments.push_back(argument.data());
        }
        return run(argc, arguments.data());
    } catch (const std::exception& error) {
        auto console = mint::cli::system_console();
        console.write_error_line(mint::localization::message(
            Message::cli_output_error,
            {mint::localization::arg(Placeholder::message,
                                     mint::escape_terminal_field(error.what()))}));
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
