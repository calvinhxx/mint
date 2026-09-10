#include "../command_sandbox_backend.hpp"
#include "command_appcontainer_windows.hpp"

#include "mint/localization/localization.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mint::command_detail::native {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

bool contains_nul(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool is_windows_executable_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::filesystem::path canonical_executable(const std::filesystem::path& candidate,
                                           const std::string& requested) {
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(candidate, error);
    if (error || !is_windows_executable_file(resolved)) {
        throw std::invalid_argument(message(Message::command_program_not_executable,
                                            {arg(Placeholder::program, requested)}));
    }
    return resolved;
}

} // namespace

void validate_program_file(const std::filesystem::path& input, const std::string& requested) {
    const auto extension = lowercase_ascii(input.extension().string());
    if (extension == ".bat" || extension == ".cmd" || extension == ".ps1" || extension == ".vbs" ||
        extension == ".js" || extension == ".wsf" || extension == ".hta" || extension == ".lnk") {
        throw std::invalid_argument(message(Message::command_program_script_forbidden,
                                            {arg(Placeholder::program, requested)}));
    }
}

std::filesystem::path find_executable(const std::string& requested) {
    if (requested.empty() || contains_nul(requested)) {
        throw std::invalid_argument(message(Message::command_program_empty));
    }

    const std::filesystem::path input(requested);
    if (input.has_parent_path()) {
        if (!input.is_absolute()) {
            throw std::invalid_argument(
                message(Message::command_program_absolute, {arg(Placeholder::program, requested)}));
        }
        return canonical_executable(input, requested);
    }

    for (const auto character : requested) {
        const auto value = static_cast<unsigned char>(character);
        if (!std::isalnum(value) && character != '_' && character != '-' && character != '+' &&
            character != '.') {
            throw std::invalid_argument(message(Message::command_program_invalid_characters,
                                                {arg(Placeholder::program, requested)}));
        }
    }

    const auto extension = lowercase_ascii(input.extension().string());
    const std::vector<std::string> suffixes = extension.empty()
                                                  ? std::vector<std::string>{"", ".exe", ".com"}
                                                  : std::vector<std::string>{""};
    const char* path_value = std::getenv("PATH");
    const std::string search_path = path_value == nullptr ? std::string{} : path_value;
    std::size_t begin = 0;
    while (begin <= search_path.size()) {
        const auto end = search_path.find(';', begin);
        const auto length = end == std::string::npos ? search_path.size() - begin : end - begin;
        auto directory = search_path.substr(begin, length);
        if (directory.size() >= 2 && directory.front() == '"' && directory.back() == '"') {
            directory = directory.substr(1, directory.size() - 2);
        }
        if (!directory.empty()) {
            for (const auto& suffix : suffixes) {
                const auto candidate = std::filesystem::path(directory) / (requested + suffix);
                if (is_windows_executable_file(candidate)) {
                    return canonical_executable(candidate, requested);
                }
            }
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    throw std::invalid_argument(
        message(Message::command_program_not_found, {arg(Placeholder::program, requested)}));
}

SandboxConfig build_sandbox_config(
    const std::filesystem::path& root,
    const std::unordered_map<std::string, std::filesystem::path>& resolved_programs,
    std::vector<std::filesystem::path> read_only_paths,
    std::vector<std::filesystem::path> denied_read_paths) {
    std::vector<std::filesystem::path> allowed_executables;
    allowed_executables.reserve(resolved_programs.size());
    for (const auto& [label, executable] : resolved_programs) {
        (void)label;
        allowed_executables.push_back(executable);
    }
    return {.backend = "windows-appcontainer",
            .native_sandbox = NativeProcessSandbox::create(root, std::move(allowed_executables),
                                                           std::move(read_only_paths),
                                                           std::move(denied_read_paths))};
}

} // namespace mint::command_detail::native

namespace mint::command_detail {

ProcessResult execute_sandboxed_process(ProcessRequest request, const SandboxConfig&) {
    return execute_process(std::move(request));
}

} // namespace mint::command_detail
