#include "command_sandbox_posix.hpp"
#include "../command_sandbox_backend.hpp"

#include "mint/localization/localization.hpp"
#include "mint/runtime/path.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
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

} // namespace

void validate_program_file(const std::filesystem::path&, const std::string&) {}

std::vector<std::filesystem::path>
normalize_read_only_paths(const std::filesystem::path& workspace,
                          std::vector<std::filesystem::path> paths,
                          const std::vector<std::filesystem::path>& denied_paths) {
    std::vector<std::filesystem::path> normalized;
    normalized.reserve(paths.size());
    for (auto& path : paths) {
        std::error_code error;
        path = std::filesystem::weakly_canonical(std::move(path), error);
        if (error || path.empty() || !path.is_absolute() || !std::filesystem::exists(path, error) ||
            error) {
            throw std::invalid_argument(message(Message::command_sandbox_read_path_invalid));
        }
        if (is_path_within(workspace, path) || is_path_within(path, workspace)) {
            throw std::invalid_argument(
                message(Message::command_sandbox_read_path_workspace_overlap,
                        {arg(Placeholder::path, path.generic_string())}));
        }
        for (const auto& denied_input : denied_paths) {
            error.clear();
            const auto denied = std::filesystem::weakly_canonical(denied_input, error);
            if (!error && !denied.empty() &&
                (is_path_within(path, denied) || is_path_within(denied, path))) {
                throw std::invalid_argument(
                    message(Message::command_sandbox_read_path_protected_overlap,
                            {arg(Placeholder::path, path.generic_string())}));
            }
        }
        normalized.push_back(std::move(path));
    }
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    return normalized;
}

bool is_executable_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
           ::access(path.c_str(), X_OK) == 0;
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
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(input, error);
        if (error || !is_executable_file(resolved)) {
            throw std::invalid_argument(message(Message::command_program_not_executable,
                                                {arg(Placeholder::program, requested)}));
        }
        return resolved;
    }

    for (const auto character : requested) {
        const auto value = static_cast<unsigned char>(character);
        if (!std::isalnum(value) && character != '_' && character != '-' && character != '+' &&
            character != '.') {
            throw std::invalid_argument(message(Message::command_program_invalid_characters,
                                                {arg(Placeholder::program, requested)}));
        }
    }

    const char* path_value = std::getenv("PATH");
    const std::string search_path =
        path_value == nullptr ? "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin" : path_value;
    std::size_t begin = 0;
    while (begin <= search_path.size()) {
        const auto end = search_path.find(':', begin);
        const auto length = end == std::string::npos ? search_path.size() - begin : end - begin;
        auto directory = search_path.substr(begin, length);
        if (directory.empty()) {
            directory = ".";
        }
        const auto candidate = std::filesystem::path(directory) / requested;
        if (is_executable_file(candidate)) {
            std::error_code error;
            const auto resolved = std::filesystem::weakly_canonical(candidate, error);
            if (!error) {
                return resolved;
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

} // namespace mint::command_detail::native
