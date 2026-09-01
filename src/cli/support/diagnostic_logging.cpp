#include "diagnostic_logging.hpp"

#include "command_line.hpp"
#include "console.hpp"

#include "mint/infrastructure/project_store.hpp"
#include "mint/localization/localization.hpp"
#include "mint/runtime/path.hpp"
#include "mint/runtime/terminal_text.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mint::cli {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

std::string_view command_mode_name(CommandMode mode) noexcept {
    switch (mode) {
    case CommandMode::none:
        return "none";
    case CommandMode::exec:
        return "exec";
    case CommandMode::init:
        return "init";
    case CommandMode::run:
        return "run";
    case CommandMode::resume:
        return "resume";
    case CommandMode::status:
        return "status";
    case CommandMode::provider:
        return "provider";
    }
    return "unknown";
}

std::string effective_file_level(const CommandLine& command_line) {
    if (!command_line.log_file_level.empty()) {
        return command_line.log_file_level;
    }
    if (!command_line.log_level.empty()) {
        return command_line.log_level;
    }
    return std::string(diagnostics::default_file_level);
}

bool file_logging_requested(const CommandLine& command_line) {
    return effective_file_level(command_line) != "off";
}

std::filesystem::path absolute_lexical_path(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        throw std::invalid_argument(message(Message::cli_logging_directory_resolve_failed));
    }
    return absolute.lexically_normal();
}

void reject_explicit_log_symlink(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw std::invalid_argument(message(Message::cli_logging_directory_inspect_failed));
    }
    if (!error && std::filesystem::is_symlink(status)) {
        throw std::invalid_argument(message(Message::cli_logging_directory_symlink));
    }
}

diagnostics::LocalLogOptions local_log_options(const CommandLine& command_line) {
    diagnostics::LocalLogOptions options;
    options.console_enabled = !command_line.interaction_jsonl;
    options.console_level = command_line.log_level.empty() ? std::string(diagnostics::default_level)
                                                           : command_line.log_level;
    options.file_level = effective_file_level(command_line);
    if (!file_logging_requested(command_line)) {
        return options;
    }

    try {
        if (!command_line.log_dir.empty()) {
            reject_explicit_log_symlink(command_line.log_dir);
            options.directory = absolute_lexical_path(command_line.log_dir);
        } else {
            const auto state_root = command_line.state_dir.empty()
                                        ? default_mint_state_directory()
                                        : absolute_lexical_path(command_line.state_dir);
            options.managed_root = state_root;
            options.directory = state_root / "logs";
        }
    } catch (const std::exception&) {
        if (!command_line.log_dir.empty()) {
            throw;
        }
        options.directory.clear();
        options.managed_root.clear();
        options.initialization_error =
            message(Message::cli_logging_default_directory_resolve_failed);
    }

    if (!options.directory.empty()) {
        const auto root = normalized_path(command_line.root);
        const auto logs = normalized_path(options.directory);
        if (is_path_within(root, logs) || is_path_within(logs, root)) {
            if (!command_line.log_dir.empty()) {
                throw std::invalid_argument(message(Message::cli_logging_workspace_overlap));
            }
            options.directory.clear();
            options.managed_root.clear();
            options.initialization_error = message(Message::cli_logging_default_workspace_overlap);
            return options;
        }
        options.directory = logs;
        if (!options.managed_root.empty()) {
            options.managed_root = logs.parent_path();
        }
    }
    return options;
}

} // namespace

diagnostics::LogStatus configure_diagnostic_logging(const CommandLine& command_line,
                                                    Console& console) {
    const auto status = diagnostics::configure_local(local_log_options(command_line));
    if (!status.file_enabled && file_logging_requested(command_line)) {
        if (!command_line.log_dir.empty()) {
            throw std::runtime_error(message(Message::cli_logging_enable_failed,
                                             {arg(Placeholder::error, status.error)}));
        }
        if (!command_line.interaction_jsonl) {
            console.write_error_line(
                message(Message::cli_logging_unavailable_warning,
                        {arg(Placeholder::error, escape_terminal_field(status.error))}));
        }
    }
    diagnostics::emit(diagnostics::Level::info, "process.started",
                      {{"mode", command_mode_name(command_line.mode)},
                       {"json_output", command_line.json_output},
                       {"interaction_jsonl", command_line.interaction_jsonl},
                       {"file_enabled", status.file_enabled}});
    return status;
}

} // namespace mint::cli
