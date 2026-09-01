#include "project_commands.hpp"

#include "mint/application/project_service.hpp"
#include "mint/localization/localization.hpp"
#include "mint/runtime/terminal_text.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace mint::cli {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

std::string status_question(std::string question) {
    question = escape_terminal_field(question);
    constexpr std::size_t display_limit = 160;
    if (question.size() > display_limit) {
        std::size_t end = display_limit;
        while (end > 0 && (static_cast<unsigned char>(question.at(end)) & 0xC0U) == 0x80U) {
            --end;
        }
        question.resize(end);
        question += "...";
    }
    return question;
}

} // namespace

int handle_init_command(const CommandLine& command_line, ProjectStore& store, Console& console) {
    auto& output = console.output_stream();
    const auto suggestion = suggest_project_policy(store.workspace_root());
    store.initialize(suggestion.project_kind, suggestion.policy, command_line.force);
    const Json result = {{"schema_version", 1},
                         {"status", "initialized"},
                         {"workspace_root", store.workspace_root().generic_string()},
                         {"state_directory", store.project_directory().generic_string()},
                         {"project_kind", suggestion.project_kind},
                         {"evidence", suggestion.evidence},
                         {"policy_path", store.project_policy_path().generic_string()},
                         {"policy", suggestion.policy}};
    if (command_line.json_output) {
        output << result.dump() << '\n';
        return 0;
    }

    output << message(Message::cli_project_initialized)
           << message(Message::cli_project_type,
                      {arg(Placeholder::value, escape_terminal_field(suggestion.project_kind))})
           << message(Message::cli_project_workspace,
                      {arg(Placeholder::path,
                           escape_terminal_field(store.workspace_root().generic_string()))})
           << message(Message::cli_project_state_directory,
                      {arg(Placeholder::path,
                           escape_terminal_field(store.project_directory().generic_string()))})
           << message(Message::cli_project_policy,
                      {arg(Placeholder::path,
                           escape_terminal_field(store.project_policy_path().generic_string()))})
           << message(Message::cli_project_evidence);
    for (const auto& evidence : suggestion.evidence) {
        output << ' ' << escape_terminal_field(evidence);
    }
    output << message(Message::cli_project_write_paths);
    for (const auto& path : suggestion.policy.at("write_paths")) {
        output << ' ' << escape_terminal_field(path.get<std::string>());
    }
    if (suggestion.policy.at("write_paths").empty()) {
        output << message(Message::cli_project_read_only);
    }
    output << message(Message::cli_project_recipes);
    for (const auto& recipe : suggestion.policy.at("recipes")) {
        output << ' ' << escape_terminal_field(recipe.at("name").get<std::string>());
        if (recipe.value("verification", false)) {
            output << "(verification)";
        }
    }
    if (suggestion.policy.at("recipes").empty()) {
        output << message(Message::cli_project_none);
    }
    output << message(
        Message::cli_project_next_step,
        {arg(Placeholder::root, escape_terminal_field(store.workspace_root().generic_string()))});
    return 0;
}

int handle_status_command(const CommandLine& command_line, ProjectStore& store, Console& console) {
    auto& output = console.output_stream();
    const auto profile = store.load_profile();
    std::vector<ManagedTaskSummary> summaries;
    if (!command_line.task_id.empty()) {
        const auto summary = store.task_summary(command_line.task_id);
        if (!summary.has_value()) {
            throw std::runtime_error(
                message(Message::cli_task_not_found, {arg(Placeholder::id, command_line.task_id)}));
        }
        summaries.push_back(*summary);
    } else {
        summaries = store.list_tasks();
    }

    Json tasks = Json::array();
    for (const auto& summary : summaries) {
        tasks.push_back(managed_task_summary_to_json(summary));
    }
    const Json result = {{"schema_version", 1},
                         {"status", "ok"},
                         {"workspace_root", store.workspace_root().generic_string()},
                         {"state_directory", store.project_directory().generic_string()},
                         {"project_kind", profile.at("project_kind")},
                         {"tasks", std::move(tasks)}};
    if (command_line.json_output) {
        output << result.dump() << '\n';
        return 0;
    }

    output << message(
        Message::cli_project_status_header,
        {arg(Placeholder::root, escape_terminal_field(store.workspace_root().generic_string())),
         arg(Placeholder::type,
             escape_terminal_field(profile.at("project_kind").get<std::string>())),
         arg(Placeholder::state,
             escape_terminal_field(store.project_directory().generic_string()))});
    if (summaries.empty()) {
        output << message(Message::cli_project_no_tasks);
        return 0;
    }
    output << message(Message::cli_project_tasks);
    for (const auto& summary : summaries) {
        output << "  " << escape_terminal_field(summary.id) << "  "
               << escape_terminal_field(summary.status)
               << "  mode=" << escape_terminal_field(managed_task_mode_name(summary.mode))
               << "  turns=" << summary.turns
               << "  verify=" << escape_terminal_field(summary.verification_status) << '\n'
               << "    " << status_question(summary.question) << '\n';
    }
    return 0;
}

} // namespace mint::cli
