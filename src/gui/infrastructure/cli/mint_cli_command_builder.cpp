#include "mint_cli_command_builder.hpp"

#include <utility>

namespace mint::infrastructure::desktop {
namespace {

std::string path_text(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto value = path.u8string();
    return {value.begin(), value.end()};
#else
    return path.native();
#endif
}

} // namespace

MintCliCommandBuilder::MintCliCommandBuilder(std::filesystem::path state_root)
    : state_root_(state_root.empty() ? std::filesystem::path{}
                                     : std::filesystem::absolute(state_root).lexically_normal()) {}

std::vector<std::string> MintCliCommandBuilder::status(const std::filesystem::path& root,
                                                       const std::string& language) const {
    std::vector<std::string> arguments;
    append_common(arguments, language);
    arguments.insert(arguments.end(), {"status", "--root", path_text(root)});
    append_state_root(arguments);
    arguments.push_back("--json");
    return arguments;
}

std::vector<std::string> MintCliCommandBuilder::run(const mint::desktop::RunRequest& request,
                                                    const std::filesystem::path& cancel_file,
                                                    std::string_view api_key_environment) const {
    std::vector<std::string> arguments;
    append_common(arguments, request.language);
    arguments.insert(arguments.end(),
                     {"run", "--root", path_text(request.root), "--json", "--interaction-jsonl",
                      "--cancel-file", path_text(cancel_file)});
    append_state_root(arguments);
    if (request.demo) {
        arguments.push_back("--demo");
    } else if (!request.config.empty()) {
        arguments.insert(arguments.end(), {"--config", path_text(request.config)});
    }
    append_api_key_environment(arguments, api_key_environment);
    arguments.push_back(request.question);
    return arguments;
}

std::vector<std::string> MintCliCommandBuilder::resume(const mint::desktop::ResumeRequest& request,
                                                       const std::filesystem::path& cancel_file,
                                                       std::string_view api_key_environment) const {
    std::vector<std::string> arguments;
    append_common(arguments, request.language);
    arguments.insert(arguments.end(),
                     {"resume", "--root", path_text(request.root), "--task", request.task_id,
                      "--json", "--interaction-jsonl", "--cancel-file", path_text(cancel_file)});
    append_state_root(arguments);
    if (!request.config.empty()) {
        arguments.insert(arguments.end(), {"--config", path_text(request.config)});
    }
    append_api_key_environment(arguments, api_key_environment);
    return arguments;
}

void MintCliCommandBuilder::append_common(std::vector<std::string>& arguments,
                                          const std::string& language) const {
    if (!language.empty()) {
        arguments.insert(arguments.end(), {"--lang", language});
    }
}

void MintCliCommandBuilder::append_api_key_environment(std::vector<std::string>& arguments,
                                                       std::string_view environment) {
    if (!environment.empty()) {
        arguments.insert(arguments.end(), {"--api-key-env", std::string(environment)});
    }
}

void MintCliCommandBuilder::append_state_root(std::vector<std::string>& arguments) const {
    if (!state_root_.empty()) {
        arguments.insert(arguments.end(), {"--state-dir", path_text(state_root_)});
    }
}

} // namespace mint::infrastructure::desktop
