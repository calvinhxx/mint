#include "project_commands.hpp"

#include "mint/application/project_service.hpp"

#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace mint::cli {
namespace {

std::string status_question(std::string question) {
    for (auto& character : question) {
        if (std::iscntrl(static_cast<unsigned char>(character)) != 0) {
            character = ' ';
        }
    }
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

    output << "项目已初始化\n"
           << "类型: " << suggestion.project_kind << '\n'
           << "工作区: " << store.workspace_root().generic_string() << '\n'
           << "状态目录: " << store.project_directory().generic_string() << '\n'
           << "Policy: " << store.project_policy_path().generic_string() << '\n'
           << "检测依据:";
    for (const auto& evidence : suggestion.evidence) {
        output << ' ' << evidence;
    }
    output << "\n写路径:";
    for (const auto& path : suggestion.policy.at("write_paths")) {
        output << ' ' << path.get<std::string>();
    }
    if (suggestion.policy.at("write_paths").empty()) {
        output << " 只读";
    }
    output << "\nRecipes:";
    for (const auto& recipe : suggestion.policy.at("recipes")) {
        output << ' ' << recipe.at("name").get<std::string>();
        if (recipe.value("verification", false)) {
            output << "(verification)";
        }
    }
    if (suggestion.policy.at("recipes").empty()) {
        output << " 无";
    }
    output << "\n\n下一步: mint run --root \"" << store.workspace_root().generic_string()
           << "\" \"你的任务\"\n";
    return 0;
}

int handle_status_command(const CommandLine& command_line, ProjectStore& store, Console& console) {
    auto& output = console.output_stream();
    const auto profile = store.load_profile();
    std::vector<ManagedTaskSummary> summaries;
    if (!command_line.task_id.empty()) {
        const auto summary = store.task_summary(command_line.task_id);
        if (!summary.has_value()) {
            throw std::runtime_error("找不到任务: " + command_line.task_id);
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

    output << "项目: " << store.workspace_root().generic_string() << " ("
           << profile.at("project_kind").get<std::string>() << ")\n"
           << "状态目录: " << store.project_directory().generic_string() << '\n';
    if (summaries.empty()) {
        output << "暂无任务。\n";
        return 0;
    }
    output << "\n任务:\n";
    for (const auto& summary : summaries) {
        output << "  " << summary.id << "  " << summary.status
               << "  mode=" << managed_task_mode_name(summary.mode) << "  turns=" << summary.turns
               << "  verify=" << summary.verification_status << '\n'
               << "    " << status_question(summary.question) << '\n';
    }
    return 0;
}

} // namespace mint::cli
