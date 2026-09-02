#include "mint/tools/tool_registry.hpp"

#include "mint/domain/change_journal.hpp"
#include "mint/localization/localization.hpp"

#include "file_support.hpp"
#include "registry/tool_arguments.hpp"
#include "registry/tool_names.hpp"
#include "workspace_support.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace mint {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

using tools::detail::contains_ignored_component;
using tools::detail::contains_nul;
using tools::detail::display_path;
using tools::detail::error_result;
using tools::detail::is_valid_utf8;
using tools::detail::replace_file_safely;
using tools::detail::require_only_fields;
using tools::detail::require_string;

Json ToolRegistry::apply_patch(const Json& arguments) const {
    require_only_fields(arguments, tools::name::apply_patch,
                        {"path", "operation", "old_text", "new_text"});
    const auto requested = require_string(arguments, "path");
    const auto operation = require_string(arguments, "operation");
    const auto new_text = require_string(arguments, "new_text");
    if (operation == "create") {
        require_only_fields(arguments, "apply_patch create", {"path", "operation", "new_text"});
    } else if (operation == "replace") {
        require_only_fields(arguments, "apply_patch replace",
                            {"path", "operation", "old_text", "new_text"});
    } else {
        throw std::invalid_argument(message(Message::tools_patch_operation));
    }

    const std::filesystem::path input(requested);
    if (input.empty() || input == "." || input.is_absolute()) {
        throw std::invalid_argument(message(Message::tools_patch_relative_path));
    }
    if (new_text.size() > runtime_bounds::max_edit_file_bytes) {
        throw std::invalid_argument(message(Message::tools_patch_new_text_too_large));
    }
    if (contains_nul(new_text) || !is_valid_utf8(new_text)) {
        throw std::invalid_argument(message(Message::tools_patch_invalid_text));
    }

    const auto unresolved = root_ / input;
    std::error_code status_error;
    const auto unresolved_status = std::filesystem::symlink_status(unresolved, status_error);
    if (!status_error && std::filesystem::is_symlink(unresolved_status)) {
        throw std::runtime_error(
            message(Message::tools_patch_symlink, {arg(Placeholder::path, requested)}));
    }

    const auto target = resolve_inside_root(requested);
    if (!is_write_allowed(target)) {
        return error_result(
            message(Message::tools_patch_unauthorized, {arg(Placeholder::path, requested)}));
    }
    if (is_protected(target)) {
        return error_result(
            message(Message::tools_patch_protected, {arg(Placeholder::path, requested)}));
    }
    if (contains_ignored_component(root_, target)) {
        return error_result(
            message(Message::tools_patch_ignored, {arg(Placeholder::path, requested)}));
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(target, error);
    if (error) {
        throw std::runtime_error(
            message(Message::tools_patch_inspect_failed, {arg(Placeholder::path, requested)}));
    }

    if (operation == "create") {
        if (exists) {
            return error_result(
                message(Message::tools_patch_create_exists, {arg(Placeholder::path, requested)}));
        }
        if (!std::filesystem::is_directory(target.parent_path(), error) || error) {
            return error_result(
                message(Message::tools_patch_parent_missing, {arg(Placeholder::path, requested)}));
        }

        replace_file_safely(target, new_text, false);
        const auto relative_path = display_path(root_, target);
        change_journal_->record_created(relative_path, new_text);
        return {{"ok", true},
                {"operation", "create"},
                {"path", relative_path},
                {"bytes_before", 0},
                {"bytes_after", new_text.size()}};
    }

    const auto old_text = require_string(arguments, "old_text");
    if (old_text.empty()) {
        throw std::invalid_argument(message(Message::tools_patch_old_text_empty));
    }
    if (old_text.size() > runtime_bounds::max_edit_file_bytes) {
        throw std::invalid_argument(message(Message::tools_patch_old_text_too_large));
    }
    if (!exists || !std::filesystem::is_regular_file(target, error) || error) {
        return error_result(
            message(Message::tools_patch_target_missing, {arg(Placeholder::path, requested)}));
    }

    const auto size = std::filesystem::file_size(target, error);
    if (error) {
        return error_result(
            message(Message::tools_file_size_failed, {arg(Placeholder::path, requested)}));
    }
    if (size > runtime_bounds::max_edit_file_bytes) {
        return error_result(
            message(Message::tools_patch_target_too_large, {arg(Placeholder::path, requested)}));
    }

    std::ifstream input_stream(target, std::ios::binary);
    if (!input_stream) {
        return error_result(
            message(Message::tools_patch_open_failed, {arg(Placeholder::path, requested)}));
    }
    std::string original((std::istreambuf_iterator<char>(input_stream)),
                         std::istreambuf_iterator<char>());
    if (!input_stream.eof() && input_stream.fail()) {
        return error_result(
            message(Message::tools_patch_read_failed, {arg(Placeholder::path, requested)}));
    }
    input_stream.close();
    if (contains_nul(original) || !is_valid_utf8(original)) {
        return error_result(
            message(Message::tools_patch_binary, {arg(Placeholder::path, requested)}));
    }

    const auto match = original.find(old_text);
    if (match == std::string::npos) {
        return error_result(message(Message::tools_patch_old_text_missing));
    }
    if (original.find(old_text, match + 1) != std::string::npos) {
        return error_result(message(Message::tools_patch_old_text_ambiguous));
    }

    std::string updated = original;
    updated.replace(match, old_text.size(), new_text);
    if (updated == original) {
        return error_result(message(Message::tools_patch_no_change));
    }
    if (updated.size() > runtime_bounds::max_edit_file_bytes) {
        return error_result(message(Message::tools_patch_result_too_large));
    }

    replace_file_safely(target, updated, true);
    const auto relative_path = display_path(root_, target);
    change_journal_->record_modified(relative_path, original, updated);
    return {{"ok", true},
            {"operation", "replace"},
            {"path", relative_path},
            {"bytes_before", original.size()},
            {"bytes_after", updated.size()}};
}

} // namespace mint
