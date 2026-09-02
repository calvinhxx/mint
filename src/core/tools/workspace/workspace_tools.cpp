#include "mint/tools/tool_registry.hpp"

#include "mint/localization/localization.hpp"

#include "file_support.hpp"
#include "path_identity.hpp"
#include "registry/tool_arguments.hpp"
#include "registry/tool_contract.hpp"
#include "registry/tool_names.hpp"
#include "sensitive_path.hpp"
#include "workspace_support.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
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
using tools::detail::is_entry_within;
using tools::detail::is_ignored_directory;
using tools::detail::is_sensitive_path;
using tools::detail::require_only_fields;
using tools::detail::require_string;
using tools::detail::shorten_line;

std::filesystem::path ToolRegistry::resolve_inside_root(const std::string& requested) const {
    const std::filesystem::path input = requested.empty() ? "." : requested;
    const auto candidate = input.is_absolute() ? input : root_ / input;

    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(candidate, error);
    if (error) {
        throw std::runtime_error(
            message(Message::tools_path_resolve_failed, {arg(Placeholder::path, requested)}));
    }
    if (!is_entry_within(root_, resolved)) {
        throw std::runtime_error(
            message(Message::tools_path_outside_workspace, {arg(Placeholder::path, requested)}));
    }
    return tools::detail::path_entry_identity(resolved);
}

Json ToolRegistry::list_files(const Json& arguments) const {
    require_only_fields(arguments, tools::name::list_files, {"path", "max_depth"});
    std::string requested = ".";
    if (arguments.contains("path")) {
        requested = require_string(arguments, "path");
    }

    int max_depth = tools::contract::default_list_depth;
    if (arguments.contains("max_depth")) {
        if (!arguments.at("max_depth").is_number_integer()) {
            throw std::invalid_argument(
                message(Message::tools_argument_integer, {arg(Placeholder::name, "max_depth")}));
        }
        max_depth = arguments.at("max_depth").get<int>();
    }
    if (max_depth < tools::contract::min_list_depth ||
        max_depth > tools::contract::max_list_depth) {
        throw std::invalid_argument(
            message(Message::tools_argument_range,
                    {arg(Placeholder::name, "max_depth"), arg(Placeholder::minimum, 1),
                     arg(Placeholder::maximum, 4)}));
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

    Json entries = Json::array();
    if (std::filesystem::is_regular_file(target, error)) {
        entries.push_back({{"path", display_path(root_, target)},
                           {"type", "file"},
                           {"size", std::filesystem::file_size(target, error)}});
        return {{"ok", true}, {"entries", entries}, {"truncated", false}};
    }
    if (!std::filesystem::is_directory(target, error)) {
        return error_result(message(Message::tools_path_not_file_or_directory,
                                    {arg(Placeholder::path, requested)}));
    }

    bool truncated = false;
    std::filesystem::recursive_directory_iterator iterator(
        target, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;

    while (iterator != end) {
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }

        const auto path = iterator->path();
        if (is_protected(path) || is_sensitive_path(root_, path)) {
            if (std::filesystem::is_directory(path, error) && !error) {
                iterator.disable_recursion_pending();
            }
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

        const bool is_directory = std::filesystem::is_directory(status);
        if (is_directory && is_ignored_directory(path)) {
            iterator.disable_recursion_pending();
            iterator.increment(error);
            continue;
        }
        if (is_directory && iterator.depth() + 1 >= max_depth) {
            iterator.disable_recursion_pending();
        }

        std::string type = "other";
        if (std::filesystem::is_symlink(status)) {
            type = "symlink";
            iterator.disable_recursion_pending();
        } else if (is_directory) {
            type = "directory";
        } else if (std::filesystem::is_regular_file(status)) {
            type = "file";
        }

        Json entry = {{"path", display_path(root_, path)}, {"type", type}};
        if (type == "file") {
            const auto size = std::filesystem::file_size(path, error);
            if (!error) {
                entry["size"] = size;
            }
            error.clear();
        }
        entries.push_back(std::move(entry));

        if (entries.size() >= runtime_.list_max_entries) {
            truncated = true;
            break;
        }
        iterator.increment(error);
    }

    std::sort(entries.begin(), entries.end(), [](const Json& left, const Json& right) {
        return left.at("path").get_ref<const std::string&>() <
               right.at("path").get_ref<const std::string&>();
    });
    return {{"ok", true},
            {"root", display_path(root_, target)},
            {"entries", std::move(entries)},
            {"truncated", truncated}};
}

Json ToolRegistry::read_file(const Json& arguments) const {
    require_only_fields(arguments, tools::name::read_file, {"path", "offset", "max_bytes"});
    const auto requested = require_string(arguments, "path");
    const auto path = resolve_inside_root(requested);
    if (is_protected(path) || is_sensitive_path(root_, path)) {
        return error_result(
            message(Message::tools_path_protected, {arg(Placeholder::path, requested)}));
    }
    if (contains_ignored_component(root_, path)) {
        return error_result(
            message(Message::tools_path_ignored, {arg(Placeholder::path, requested)}));
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return error_result(
            message(Message::tools_file_missing, {arg(Placeholder::path, requested)}));
    }

    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return error_result(
            message(Message::tools_file_size_failed, {arg(Placeholder::path, requested)}));
    }

    std::uintmax_t offset = 0;
    if (arguments.contains("offset")) {
        if (!arguments.at("offset").is_number_integer()) {
            throw std::invalid_argument(message(Message::tools_argument_nonnegative_integer,
                                                {arg(Placeholder::name, "offset")}));
        }
        const auto parsed = arguments.at("offset").get<long long>();
        if (parsed < 0) {
            throw std::invalid_argument(message(Message::tools_argument_nonnegative_integer,
                                                {arg(Placeholder::name, "offset")}));
        }
        offset = static_cast<std::uintmax_t>(parsed);
    }
    if (offset > size) {
        return error_result(
            message(Message::tools_file_offset_out_of_range, {arg(Placeholder::path, requested)}));
    }

    std::uintmax_t requested_bytes = runtime_.read_file_bytes;
    if (arguments.contains("max_bytes")) {
        if (!arguments.at("max_bytes").is_number_integer()) {
            throw std::invalid_argument(
                message(Message::tools_argument_integer, {arg(Placeholder::name, "max_bytes")}));
        }
        const auto parsed = arguments.at("max_bytes").get<long long>();
        if (parsed < static_cast<long long>(runtime_bounds::min_read_file_bytes) ||
            parsed > static_cast<long long>(runtime_bounds::max_read_file_bytes)) {
            throw std::invalid_argument(
                message(Message::tools_argument_range,
                        {arg(Placeholder::name, "max_bytes"),
                         arg(Placeholder::minimum, runtime_bounds::min_read_file_bytes),
                         arg(Placeholder::maximum, runtime_bounds::max_read_file_bytes)}));
        }
        requested_bytes = static_cast<std::uintmax_t>(parsed);
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return error_result(
            message(Message::tools_file_open_failed, {arg(Placeholder::path, requested)}));
    }
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        return error_result(
            message(Message::tools_file_seek_failed, {arg(Placeholder::path, requested)}));
    }

    const auto bytes_to_read = static_cast<std::size_t>(std::min(size - offset, requested_bytes));
    std::string content(bytes_to_read, '\0');
    input.read(content.data(), static_cast<std::streamsize>(bytes_to_read));
    content.resize(static_cast<std::size_t>(input.gcount()));
    if (contains_nul(content)) {
        return error_result(
            message(Message::tools_file_binary, {arg(Placeholder::path, requested)}));
    }

    if (offset + content.size() < size &&
        content.size() > tools::contract::newline_alignment_min_bytes) {
        const auto last_newline = content.rfind('\n');
        if (last_newline != std::string::npos && last_newline + 1 >= content.size() / 2) {
            content.resize(last_newline + 1);
        }
    }
    const auto next_offset = offset + content.size();
    const auto truncated = next_offset < size;

    return {{"ok", true},
            {"path", display_path(root_, path)},
            {"size", size},
            {"offset", offset},
            {"next_offset", truncated ? Json(next_offset) : Json(nullptr)},
            {"truncated", truncated},
            {"content", std::move(content)}};
}

Json ToolRegistry::search_text(const Json& arguments) const {
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
        std::string probe(tools::contract::binary_probe_bytes, '\0');
        input.read(probe.data(), static_cast<std::streamsize>(probe.size()));
        probe.resize(static_cast<std::size_t>(input.gcount()));
        if (contains_nul(probe)) {
            return;
        }
        input.clear();
        input.seekg(0);
        const auto path_label = display_path(root_, path);
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(input, line)) {
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
        while (iterator != end && !truncated) {
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
                if (is_ignored_directory(iterator->path()) ||
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
