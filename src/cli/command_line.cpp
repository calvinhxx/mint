#include "command_line.hpp"
#include "support/console.hpp"

#include "mint/localization/localization.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mint::cli {
namespace {

using localization::arg;
using localization::Message;
using localization::Placeholder;

std::string text(localization::Message message,
                 std::initializer_list<localization::Argument> arguments = {}) {
    return localization::message(message, arguments);
}

[[noreturn]] void missing_value(std::string_view option, std::string_view value) {
    throw std::invalid_argument(
        text(Message::cli_error_option_requires,
             {arg(Placeholder::option, option), arg(Placeholder::value, value)}));
}

unsigned long parse_unsigned(const std::string& text, const std::string& option) {
    std::size_t consumed = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(text, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument(localization::message(
            Message::cli_error_positive_integer_required, {arg(Placeholder::option, option)}));
    }
    if (consumed != text.size()) {
        throw std::invalid_argument(localization::message(
            Message::cli_error_positive_integer_required, {arg(Placeholder::option, option)}));
    }
    return value;
}

std::optional<CommandMode> parse_mode(const std::string& argument) {
    if (argument == "exec") {
        return CommandMode::exec;
    }
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
    return std::nullopt;
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
    console.output_stream() << text(Message::cli_help_general,
                                    {arg(Placeholder::program, program)});
}

void print_exec_help(Console& console, const char* program) {
    console.output_stream() << text(
        Message::cli_help_exec,
        {arg(Placeholder::program, program), arg(Placeholder::min_turns, runtime_bounds::min_turns),
         arg(Placeholder::max_turns, runtime_bounds::max_turns),
         arg(Placeholder::max_seconds, runtime_bounds::max_seconds),
         arg(Placeholder::min_context_bytes, runtime_bounds::min_context_bytes),
         arg(Placeholder::max_context_bytes, runtime_bounds::max_context_bytes),
         arg(Placeholder::max_total_tokens, runtime_bounds::max_total_tokens)});
}

std::filesystem::path normalized_path(std::filesystem::path path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(std::move(path), error);
    if (error) {
        throw std::invalid_argument(text(Message::cli_error_runtime_path));
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
    int command_index = 1;
    while (command_index < argc && std::string_view(argv[command_index]) == "--lang") {
        if (++command_index >= argc) {
            missing_value("--lang", text(Message::cli_value_language));
        }
        result.language = argv[command_index++];
    }

    int begin = command_index;
    if (command_index >= argc) {
        result.help = true;
        return result;
    } else {
        const std::string first = argv[command_index];
        if (const auto mode = parse_mode(first)) {
            result.mode = *mode;
            begin = command_index + 1;
        } else if (first == "-h" || first == "--help" || first == "--version") {
            if (argc != command_index + 1) {
                throw std::invalid_argument(text(Message::cli_error_global_option_count));
            }
            result.help = first != "--version";
            result.version = first == "--version";
            return result;
        } else if (!first.starts_with('-')) {
            throw std::invalid_argument(
                text(Message::cli_error_unknown_command, {arg(Placeholder::value, first)}));
        } else {
            throw std::invalid_argument(
                text(Message::cli_error_unknown_global_option, {arg(Placeholder::value, first)}));
        }
    }
    if (result.mode == CommandMode::resume) {
        result.resume_session = true;
    }
    if (result.mode == CommandMode::provider && begin < argc &&
        std::string(argv[begin]) == "test") {
        result.provider_action = ProviderCommandAction::test;
        ++begin;
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
                missing_value("--allow-write-path", text(Message::cli_value_relative_path));
            }
            result.allowed_write_paths.emplace_back(utf8_path(argv[index]));
            result.policy_conflict = true;
        } else if (argument == "--allow-command") {
            if (++index >= argc) {
                missing_value("--allow-command", text(Message::cli_value_program_or_absolute_path));
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
                missing_value("--cancel-file", text(Message::cli_value_file_path));
            }
            result.cancel_file = utf8_path(argv[index]);
        } else if (argument == "--resume") {
            result.resume_session = true;
        } else if (argument == "--retry-inflight") {
            result.retry_inflight = true;
        } else if (argument == "-h" || argument == "--help") {
            result.help = true;
        } else if (argument == "--version") {
            result.version = true;
        } else if (argument == "--lang") {
            if (++index >= argc) {
                missing_value("--lang", text(Message::cli_value_language));
            }
            result.language = argv[index];
        } else if (argument == "--config") {
            if (++index >= argc) {
                missing_value("--config", text(Message::cli_value_json_path));
            }
            result.config = utf8_path(argv[index]);
            result.config_specified = true;
        } else if (argument == "--log-level") {
            if (++index >= argc) {
                missing_value("--log-level", text(Message::cli_value_log_level));
            }
            result.log_level = argv[index];
        } else if (argument == "--log-file-level") {
            if (++index >= argc) {
                missing_value("--log-file-level", text(Message::cli_value_log_level));
            }
            result.log_file_level = argv[index];
        } else if (argument == "--log-dir") {
            if (++index >= argc) {
                missing_value("--log-dir", text(Message::cli_value_directory_path));
            }
            result.log_dir = utf8_path(argv[index]);
        } else if (argument == "--policy") {
            if (++index >= argc) {
                missing_value("--policy", text(Message::cli_value_json_path));
            }
            result.policy = utf8_path(argv[index]);
        } else if (argument == "--root") {
            if (++index >= argc) {
                missing_value("--root", text(Message::cli_value_path));
            }
            result.root = utf8_path(argv[index]);
            result.root_specified = true;
        } else if (argument == "--state-dir") {
            if (++index >= argc) {
                missing_value("--state-dir", text(Message::cli_value_path));
            }
            result.state_dir = utf8_path(argv[index]);
        } else if (argument == "--task") {
            if (++index >= argc) {
                missing_value("--task", text(Message::cli_value_task_id));
            }
            result.task_id = argv[index];
        } else if (argument == "--events-jsonl") {
            if (++index >= argc) {
                missing_value("--events-jsonl", text(Message::cli_value_file_path));
            }
            result.events_jsonl = utf8_path(argv[index]);
        } else if (argument == "--session") {
            if (++index >= argc) {
                missing_value("--session", text(Message::cli_value_file_path));
            }
            result.session = utf8_path(argv[index]);
        } else if (argument == "--max-turns") {
            if (++index >= argc) {
                missing_value("--max-turns", text(Message::cli_value_number));
            }
            const auto parsed = parse_unsigned(argv[index], "--max-turns");
            if (parsed < runtime_bounds::min_turns || parsed > runtime_bounds::max_turns) {
                throw std::invalid_argument(text(Message::cli_error_option_out_of_range,
                                                 {arg(Placeholder::option, "--max-turns")}));
            }
            result.max_turns = parsed;
            result.policy_conflict = true;
        } else if (argument == "--max-seconds") {
            if (++index >= argc) {
                missing_value("--max-seconds", text(Message::cli_value_number));
            }
            const auto parsed = parse_unsigned(argv[index], "--max-seconds");
            if (parsed == 0 || parsed > static_cast<unsigned long>(runtime_bounds::max_seconds)) {
                throw std::invalid_argument(text(Message::cli_error_option_out_of_range,
                                                 {arg(Placeholder::option, "--max-seconds")}));
            }
            result.max_seconds = static_cast<long>(parsed);
            result.policy_conflict = true;
        } else if (argument == "--max-context-bytes") {
            if (++index >= argc) {
                missing_value("--max-context-bytes", text(Message::cli_value_number));
            }
            const auto parsed = parse_unsigned(argv[index], "--max-context-bytes");
            if (parsed < runtime_bounds::min_context_bytes ||
                parsed > runtime_bounds::max_context_bytes) {
                throw std::invalid_argument(
                    text(Message::cli_error_option_out_of_range,
                         {arg(Placeholder::option, "--max-context-bytes")}));
            }
            result.max_context_bytes = parsed;
            result.policy_conflict = true;
        } else if (argument == "--max-total-tokens") {
            if (++index >= argc) {
                missing_value("--max-total-tokens", text(Message::cli_value_number));
            }
            const auto parsed = parse_unsigned(argv[index], "--max-total-tokens");
            if (parsed > runtime_bounds::max_total_tokens) {
                throw std::invalid_argument(text(Message::cli_error_option_out_of_range,
                                                 {arg(Placeholder::option, "--max-total-tokens")}));
            }
            result.max_total_tokens = parsed;
            result.policy_conflict = true;
        } else if (argument.starts_with('-')) {
            throw std::invalid_argument(
                text(Message::cli_error_unknown_option, {arg(Placeholder::value, argument)}));
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

    if (result.mode == CommandMode::exec) {
        if (!result.state_dir.empty() || !result.task_id.empty() || !result.cancel_file.empty() ||
            result.force || result.interaction_jsonl) {
            throw std::invalid_argument(text(Message::cli_error_exec_managed_options));
        }
    } else if (result.mode == CommandMode::provider) {
        if (!result.question.empty() || result.demo || result.force || result.resume_session ||
            result.retry_inflight || result.approve_each_command || result.approve_each_changeset ||
            result.policy_conflict || !result.policy.empty() || !result.session.empty() ||
            !result.events_jsonl.empty() || !result.state_dir.empty() || !result.task_id.empty() ||
            result.root_specified || result.interaction_jsonl || !result.cancel_file.empty()) {
            throw std::invalid_argument(text(Message::cli_error_provider_options));
        }
    } else {
        if (!result.policy.empty() || !result.session.empty() || !result.events_jsonl.empty() ||
            (result.mode != CommandMode::resume && result.resume_session)) {
            throw std::invalid_argument(text(Message::cli_error_managed_artifacts));
        }
        if (result.policy_conflict) {
            throw std::invalid_argument(text(Message::cli_error_managed_capabilities));
        }
        if (result.force && result.mode != CommandMode::init) {
            throw std::invalid_argument(text(Message::cli_error_force_mode));
        }
        if (!result.task_id.empty() && result.mode != CommandMode::resume &&
            result.mode != CommandMode::status) {
            throw std::invalid_argument(text(Message::cli_error_task_mode));
        }
        if ((result.mode == CommandMode::init || result.mode == CommandMode::status ||
             result.mode == CommandMode::resume) &&
            !result.question.empty()) {
            throw std::invalid_argument(text(Message::cli_error_command_rejects_task));
        }
        if ((result.mode == CommandMode::init || result.mode == CommandMode::status) &&
            (result.demo || result.retry_inflight || result.approve_each_command ||
             result.approve_each_changeset || result.config_specified || result.interaction_jsonl ||
             !result.cancel_file.empty())) {
            throw std::invalid_argument(text(Message::cli_error_init_status_options));
        }
    }
    if (result.interaction_jsonl && !result.json_output) {
        throw std::invalid_argument(text(Message::cli_error_interaction_requires_json));
    }
    if (!result.cancel_file.empty() && !result.interaction_jsonl) {
        throw std::invalid_argument(text(Message::cli_error_cancel_requires_interaction));
    }
    if (!result.cancel_file.empty()) {
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(result.cancel_file, status_error);
        if ((!status_error && status.type() != std::filesystem::file_type::not_found) ||
            (status_error && status_error != std::errc::no_such_file_or_directory)) {
            throw std::invalid_argument(text(Message::cli_error_cancel_file_exists));
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

void configure_language(int argc, char** argv) {
    localization::use_environment_language();
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) != "--lang") {
            continue;
        }
        if (++index >= argc) {
            missing_value("--lang", text(Message::cli_value_language));
        }
        if (!localization::set_language(argv[index])) {
            throw std::invalid_argument(text(Message::localization_unsupported_language,
                                             {arg(Placeholder::language, argv[index])}));
        }
    }
}

} // namespace mint::cli
