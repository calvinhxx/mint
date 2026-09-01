#include "mint/application/project_service.hpp"

#include "mint/domain/runtime_settings.hpp"
#include "mint/version.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace mint {
namespace {

constexpr std::uintmax_t max_package_json_bytes = 1024 * 1024;

bool is_plain_regular_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && std::filesystem::is_regular_file(status) &&
           !std::filesystem::is_symlink(status);
}

std::filesystem::path canonical_directory(const std::filesystem::path& root) {
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(root, error);
    if (error || !std::filesystem::is_directory(resolved)) {
        throw std::invalid_argument("项目根目录不存在或不是目录");
    }
    return resolved;
}

void add_existing_path(const std::filesystem::path& root, const std::string& relative,
                       Json& write_paths, std::set<std::string>& seen) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(root / relative, error);
    if (!error &&
        (std::filesystem::is_directory(status) || std::filesystem::is_regular_file(status)) &&
        !std::filesystem::is_symlink(status) && seen.insert(relative).second) {
        write_paths.push_back(relative);
    }
}

Json recipe(std::string name, std::string description, std::string program,
            std::vector<std::string> args, bool verification = false) {
    return {{"name", std::move(name)},
            {"description", std::move(description)},
            {"program", std::move(program)},
            {"args", std::move(args)},
            {"timeout_seconds", runtime_defaults::managed_recipe_timeout_seconds},
            {"verification", verification}};
}

Json read_package_json(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > max_package_json_bytes) {
        throw std::invalid_argument("package.json 无法读取或超过 1 MiB");
    }
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if ((!input.eof() && input.fail()) || text.find('\0') != std::string::npos) {
        throw std::invalid_argument("package.json 读取失败或包含 NUL");
    }
    try {
        return Json::parse(text);
    } catch (const Json::exception& error_message) {
        throw std::invalid_argument("package.json 不是有效 JSON: " +
                                    std::string(error_message.what()));
    }
}

bool has_nonempty_script(const Json& scripts, const char* name) {
    if (!scripts.contains(name) || !scripts.at(name).is_string()) {
        return false;
    }
    const auto& script = scripts.at(name).get_ref<const std::string&>();
    return std::any_of(script.begin(), script.end(),
                       [](unsigned char character) { return std::isspace(character) == 0; });
}

} // namespace

ProjectSuggestion suggest_project_policy(const std::filesystem::path& root) {
    const auto resolved = canonical_directory(root);
    ProjectSuggestion suggestion;
    suggestion.project_kind = "generic-read-only";

    Json write_paths = Json::array();
    Json recipes = Json::array();
    bool require_verification = false;
    std::set<std::string> seen_paths;

    if (is_plain_regular_file(resolved / "CMakeLists.txt")) {
        suggestion.project_kind = "cmake";
        suggestion.evidence.push_back("CMakeLists.txt");
        for (const auto& path :
             {"src", "include", "tests", "cmake", "docs", "CMakeLists.txt", "README.md"}) {
            add_existing_path(resolved, path, write_paths, seen_paths);
        }
        recipes.push_back(recipe("configure", "Configure the managed CMake build", "cmake",
                                 {"-S", ".", "-B", "build/mint-managed"}));
        recipes.push_back(recipe("build", "Build the managed CMake tree", "cmake",
                                 {"--build", "build/mint-managed"}));
        recipes.push_back(recipe("test", "Run CTest in the managed build tree", "ctest",
                                 {"--test-dir", "build/mint-managed", "--output-on-failure"},
                                 true));
        require_verification = true;
    } else if (is_plain_regular_file(resolved / "Cargo.toml")) {
        suggestion.project_kind = "cargo";
        suggestion.evidence.push_back("Cargo.toml");
        for (const auto& path :
             {"src", "tests", "benches", "examples", "Cargo.toml", "README.md"}) {
            add_existing_path(resolved, path, write_paths, seen_paths);
        }
        recipes.push_back(recipe("build", "Build the Cargo workspace", "cargo", {"build"}));
        recipes.push_back(recipe("test", "Run Cargo tests", "cargo", {"test"}, true));
        require_verification = true;
    } else if (is_plain_regular_file(resolved / "package.json")) {
        suggestion.project_kind = "npm";
        suggestion.evidence.push_back("package.json");
        const auto package = read_package_json(resolved / "package.json");
        const Json scripts =
            package.is_object() && package.contains("scripts") && package.at("scripts").is_object()
                ? package.at("scripts")
                : Json::object();
        const bool has_test = has_nonempty_script(scripts, "test");
        const bool has_build = has_nonempty_script(scripts, "build");
        if (has_test || has_build) {
            for (const auto& path :
                 {"src", "test", "tests", "app", "packages", "package.json", "README.md"}) {
                add_existing_path(resolved, path, write_paths, seen_paths);
            }
        }
        if (has_build) {
            recipes.push_back(recipe("build", "Run the package build script", "npm",
                                     {"run", "build"}, !has_test));
        }
        if (has_test) {
            recipes.push_back(recipe("test", "Run the package test script", "npm", {"test"}, true));
        }
        require_verification = has_test || has_build;
        if (!require_verification) {
            suggestion.project_kind = "npm-read-only";
        }
    } else {
        suggestion.evidence.push_back("no supported build manifest");
    }

    ToolRuntimeSettings tool_limits;
    if (!recipes.empty()) {
        tool_limits.command_resources.cpu_seconds = runtime_defaults::managed_command_cpu_seconds;
        tool_limits.command_resources.max_processes =
            runtime_defaults::managed_command_max_processes;
        tool_limits.command_resources.workspace_disk_bytes =
            runtime_defaults::managed_command_workspace_disk_bytes;
    }

    suggestion.policy = {{"schema_version", policy_schema_version},
                         {"write_paths", std::move(write_paths)},
                         {"recipes", std::move(recipes)},
                         {"require_verification", require_verification},
                         {"max_turns", runtime_defaults::managed_max_turns},
                         {"max_context_bytes", runtime_defaults::managed_max_context_bytes},
                         {"max_total_tokens", runtime_defaults::managed_max_total_tokens},
                         {"max_seconds", runtime_defaults::managed_max_seconds},
                         {"tool_limits", tool_runtime_settings_to_json(tool_limits)}};
    return suggestion;
}

} // namespace mint
