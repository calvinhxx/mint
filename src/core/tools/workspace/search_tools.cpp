#include "mint/tools/tool_registry.hpp"

#include "mint/localization/localization.hpp"

#include "file_support.hpp"
#include "registry/tool_arguments.hpp"
#include "registry/tool_contract.hpp"
#include "registry/tool_names.hpp"
#include "sensitive_path.hpp"
#include "workspace_support.hpp"

#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mint {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

using tools::detail::contains_ignored_component;
using tools::detail::contains_nul;
using tools::detail::contains_text;
using tools::detail::display_path;
using tools::detail::error_result;
using tools::detail::is_ignored_directory;
using tools::detail::is_sensitive_path;
using tools::detail::require_only_fields;
using tools::detail::require_string;
using tools::detail::shorten_line;

Json ToolRegistry::search_text(const Json& arguments) const {
    check_task_control();
    require_only_fields(arguments, tools::name::search_text, {"query", "path", "case_sensitive"});
    const auto query = require_string(arguments, "query");
    if (query.empty()) {
        throw std::invalid_argument(message(Message::tools_search_empty));
    }
    if (query.size() > tools::contract::max_search_query_bytes) {
        throw std::invalid_argument(
            message(Message::tools_search_too_long,
                    {arg(Placeholder::maximum, tools::contract::max_search_query_bytes)}));
    }

    std::string requested = ".";
    if (arguments.contains("path")) {
        requested = require_string(arguments, "path");
    }
    bool case_sensitive = false;
    if (arguments.contains("case_sensitive")) {
        if (!arguments.at("case_sensitive").is_boolean()) {
            throw std::invalid_argument(message(Message::tools_argument_boolean,
                                                {arg(Placeholder::name, "case_sensitive")}));
        }
        case_sensitive = arguments.at("case_sensitive").get<bool>();
    }

    const auto target = resolve_inside_root(requested);
    if (is_protected(target) || is_sensitive_path(root_, target)) {
        return error_result(
            message(Message::tools_path_protected, {arg(Placeholder::path, requested)}));
    }
    if (contains_ignored_component(root_, target)) {
        return error_result(
            message(Message::tools_path_ignored, {arg(Placeholder::path, requested)}));
    }
    std::error_code error;
    if (!std::filesystem::exists(target, error)) {
        return error_result(
            message(Message::tools_path_missing, {arg(Placeholder::path, requested)}));
    }

    Json hits = Json::array();
    std::size_t scanned_files = 0;
    bool truncated = false;
    const auto scan_file = [&](const std::filesystem::path& path) {
        check_task_control();
        if (hits.size() >= runtime_.search_max_hits || scanned_files >= runtime_.search_max_files) {
            truncated = true;
            return;
        }
        if (is_protected(path) || is_sensitive_path(root_, path)) {
            return;
        }

        std::error_code file_error;
        if (!std::filesystem::is_regular_file(path, file_error)) {
            return;
        }
        ++scanned_files;
        const auto size = std::filesystem::file_size(path, file_error);
        if (file_error || size > runtime_.search_file_bytes) {
            return;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return;
        }
        std::string content(static_cast<std::size_t>(size) + 1, '\0');
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
        content.resize(static_cast<std::size_t>(input.gcount()));
        if (input.bad() || content.size() > size || contains_nul(content)) {
            return;
        }
        const auto path_label = display_path(root_, path);
        std::string_view remaining(content);
        std::size_t line_number = 0;
        while (!remaining.empty()) {
            check_task_control();
            const auto newline = remaining.find('\n');
            const auto line = remaining.substr(0, newline);
            remaining.remove_prefix(newline == std::string_view::npos ? remaining.size()
                                                                      : newline + 1);
            ++line_number;
            if (!contains_text(line, query, case_sensitive)) {
                continue;
            }
            hits.push_back(
                {{"path", path_label}, {"line", line_number}, {"text", shorten_line(line)}});
            if (hits.size() >= runtime_.search_max_hits) {
                truncated = true;
                return;
            }
        }
    };

    if (std::filesystem::is_regular_file(target, error)) {
        scan_file(target);
    } else if (std::filesystem::is_directory(target, error)) {
        std::filesystem::recursive_directory_iterator iterator(
            target, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        std::size_t visited = 0;
        while (iterator != end && !truncated) {
            check_task_control();
            if (visited++ >= runtime_.workspace_snapshot_entries) {
                truncated = true;
                break;
            }
            if (error) {
                error.clear();
                iterator.increment(error);
                continue;
            }

            const auto status = iterator->symlink_status(error);
            if (error) {
                error.clear();
                iterator.increment(error);
                continue;
            }
            if (std::filesystem::is_symlink(status)) {
                iterator.disable_recursion_pending();
            } else if (std::filesystem::is_directory(status)) {
                if (is_protected(iterator->path()) || is_ignored_directory(iterator->path()) ||
                    is_sensitive_path(root_, iterator->path())) {
                    iterator.disable_recursion_pending();
                }
            } else if (std::filesystem::is_regular_file(status)) {
                scan_file(iterator->path());
            }
            iterator.increment(error);
        }
    } else {
        return error_result(message(Message::tools_path_not_file_or_directory,
                                    {arg(Placeholder::path, requested)}));
    }

    return {{"ok", true},
            {"query", query},
            {"scanned_files", scanned_files},
            {"hits", std::move(hits)},
            {"truncated", truncated}};
}

} // namespace mint
