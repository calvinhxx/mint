#include "mint/tools/tool_registry.hpp"

#include "mint/domain/change_journal.hpp"
#include "mint/localization/localization.hpp"
#include "mint/version.hpp"

#include "registry/tool_contract.hpp"
#include "registry/tool_registry_command.hpp"
#include "workspace/file_support.hpp"
#include "workspace/workspace_support.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mint {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

void upsert_workspace_risk(Json& changed_files, const std::string& path, std::string_view status) {
    for (auto entry = changed_files.begin(); entry != changed_files.end();) {
        if (entry->is_object() && entry->value("path", "") == path) {
            entry = changed_files.erase(entry);
        } else {
            ++entry;
        }
    }
    changed_files.push_back(
        {{"path", path}, {"status", status}, {"bytes_before", nullptr}, {"bytes_after", nullptr}});
}

} // namespace

using tools::detail::contains_ignored_component;
using tools::detail::contains_nul;
using tools::detail::is_valid_utf8;

bool ToolRegistry::has_workspace_changes() const {
    return workspace_tracking_error_.has_value() || !unauditable_command_paths_.empty() ||
           !command_policy_violation_paths_.empty() ||
           (change_journal_ != nullptr && change_journal_->has_changes());
}

bool ToolRegistry::workspace_integrity_failed() const noexcept {
    return workspace_tracking_error_.has_value() || !unauditable_command_paths_.empty() ||
           !command_policy_violation_paths_.empty();
}

Json ToolRegistry::workspace_change_snapshot() const {
    if (change_journal_ == nullptr) {
        return {{"ok", true},
                {"changed_files", Json::array()},
                {"diff", ""},
                {"diff_truncated", false}};
    }
    auto result = change_journal_->snapshot();
    for (const auto& path : unauditable_command_paths_) {
        upsert_workspace_risk(result["changed_files"], path, "unauditable");
    }
    for (const auto& path : command_policy_violation_paths_) {
        upsert_workspace_risk(result["changed_files"], path, "policy_violation");
    }
    if (workspace_tracking_error_.has_value()) {
        upsert_workspace_risk(result["changed_files"], "<workspace>", "unauditable");
    }
    return result;
}

Json ToolRegistry::workspace_change_state() const {
    if (change_journal_ == nullptr) {
        return {{"schema_version", workspace_change_schema_version}, {"entries", Json::array()}};
    }
    auto result = change_journal_->state();
    result["schema_version"] = workspace_change_schema_version;
    if (!unauditable_command_paths_.empty()) {
        result["unauditable_command_paths"] = unauditable_command_paths_;
    }
    if (!command_policy_violation_paths_.empty()) {
        result["command_policy_violation_paths"] = command_policy_violation_paths_;
    }
    if (workspace_tracking_error_.has_value()) {
        result["workspace_tracking_error"] = *workspace_tracking_error_;
    }
    return result;
}

void ToolRegistry::restore_workspace_change_state(const Json& state) {
    const auto journal_schema = state.is_object() ? state.value("schema_version", 0) : 0;
    if (!state.is_object() ||
        (journal_schema != 1 && journal_schema != 2 &&
         journal_schema != workspace_change_schema_version) ||
        !state.contains("entries") || !state.at("entries").is_array()) {
        throw std::invalid_argument(message(Message::tools_restore_invalid_journal));
    }
    const bool has_safety_state = state.contains("unauditable_command_paths") ||
                                  state.contains("command_policy_violation_paths") ||
                                  state.contains("workspace_tracking_error");
    if (journal_schema < workspace_change_schema_version && has_safety_state) {
        throw std::invalid_argument(message(Message::tools_restore_taint_schema));
    }
    if (state.at("entries").size() > tools::contract::max_restored_changes) {
        throw std::invalid_argument(message(Message::tools_restore_too_many_changes));
    }
    std::vector<std::string> unauditable_paths;
    std::vector<std::string> policy_violation_paths;
    std::optional<std::string> tracking_error;
    const auto restore_taint_paths = [&](const char* field, std::vector<std::string>& output) {
        if (!state.contains(field)) {
            return;
        }
        if (!state.at(field).is_array() ||
            state.at(field).size() > tools::contract::max_restored_changes) {
            throw std::invalid_argument(message(Message::tools_restore_invalid_taint_paths));
        }
        for (const auto& item : state.at(field)) {
            if (!item.is_string()) {
                throw std::invalid_argument(message(Message::tools_restore_invalid_taint_paths));
            }
            const auto requested = item.get<std::string>();
            const std::filesystem::path input(requested);
            if (input.empty() || input == "." || input.is_absolute()) {
                throw std::invalid_argument(message(Message::tools_restore_invalid_taint_path,
                                                    {arg(Placeholder::path, requested)}));
            }
            const auto target = resolve_inside_root(requested);
            if (is_protected(target) || contains_ignored_component(root_, target)) {
                throw std::invalid_argument(message(Message::tools_restore_unauditable_taint_path,
                                                    {arg(Placeholder::path, requested)}));
            }
            if (std::find(output.begin(), output.end(), requested) == output.end()) {
                output.push_back(requested);
            }
        }
    };
    restore_taint_paths("unauditable_command_paths", unauditable_paths);
    restore_taint_paths("command_policy_violation_paths", policy_violation_paths);
    if (state.contains("workspace_tracking_error")) {
        if (!state.at("workspace_tracking_error").is_string() ||
            state.at("workspace_tracking_error").get_ref<const std::string&>() !=
                tools::detail::workspace_tracking_error_code) {
            throw std::invalid_argument(message(Message::tools_restore_invalid_tracking_error));
        }
        tracking_error = state.at("workspace_tracking_error").get<std::string>();
    }
    if (change_journal_ == nullptr) {
        if (!state.at("entries").empty() || !unauditable_paths.empty() ||
            !policy_violation_paths.empty() || tracking_error.has_value()) {
            throw std::invalid_argument(message(Message::tools_restore_requires_capability));
        }
        return;
    }

    for (const auto& item : state.at("entries")) {
        if (!item.is_object() || !item.contains("path") || !item.at("path").is_string() ||
            !item.contains("before") || !item.at("before").is_string() || !item.contains("after") ||
            !item.at("after").is_string()) {
            throw std::invalid_argument(message(Message::tools_restore_invalid_journal_entry));
        }
        const auto requested = item.at("path").get<std::string>();
        const auto& before = item.at("before").get_ref<const std::string&>();
        const auto& expected = item.at("after").get_ref<const std::string&>();
        bool before_exists = true;
        bool after_exists = true;
        if (journal_schema == 1) {
            if (!item.contains("created") || !item.at("created").is_boolean()) {
                throw std::invalid_argument(message(Message::tools_restore_invalid_v1_entry));
            }
            before_exists = !item.at("created").get<bool>();
        } else {
            if (!item.contains("before_exists") || !item.at("before_exists").is_boolean() ||
                !item.contains("after_exists") || !item.at("after_exists").is_boolean()) {
                throw std::invalid_argument(message(Message::tools_restore_invalid_v2_entry));
            }
            before_exists = item.at("before_exists").get<bool>();
            after_exists = item.at("after_exists").get<bool>();
        }
        if (before.size() > runtime_bounds::max_edit_file_bytes ||
            expected.size() > runtime_bounds::max_edit_file_bytes || contains_nul(before) ||
            contains_nul(expected) || !is_valid_utf8(before) || !is_valid_utf8(expected) ||
            (!before_exists && !before.empty()) || (!after_exists && !expected.empty())) {
            throw std::invalid_argument(message(Message::tools_restore_invalid_contents,
                                                {arg(Placeholder::path, requested)}));
        }

        const std::filesystem::path input(requested);
        if (input.empty() || input == "." || input.is_absolute()) {
            throw std::invalid_argument(
                message(Message::tools_restore_invalid_path, {arg(Placeholder::path, requested)}));
        }
        std::error_code error;
        const auto unresolved = root_ / input;
        const auto status = std::filesystem::symlink_status(unresolved, error);
        if (!error && std::filesystem::is_symlink(status)) {
            throw std::invalid_argument(
                message(Message::tools_restore_symlink, {arg(Placeholder::path, requested)}));
        }
        error.clear();
        const auto target = resolve_inside_root(requested);
        if (is_protected(target) || contains_ignored_component(root_, target) ||
            !is_write_allowed(target)) {
            throw std::invalid_argument(
                message(Message::tools_restore_unrestorable, {arg(Placeholder::path, requested)}));
        }
        const bool current_exists = std::filesystem::exists(target, error);
        if (error || current_exists != after_exists) {
            throw std::invalid_argument(message(Message::tools_restore_existence_changed,
                                                {arg(Placeholder::path, requested)}));
        }
        if (!after_exists) {
            continue;
        }
        if (!std::filesystem::is_regular_file(target, error) || error) {
            throw std::invalid_argument(message(Message::tools_restore_not_regular_file,
                                                {arg(Placeholder::path, requested)}));
        }
        const auto size = std::filesystem::file_size(target, error);
        if (error || size > runtime_bounds::max_edit_file_bytes) {
            throw std::invalid_argument(
                message(Message::tools_restore_unreadable, {arg(Placeholder::path, requested)}));
        }
        std::ifstream input_stream(target, std::ios::binary);
        std::string current(expected.size() + 1, '\0');
        input_stream.read(current.data(), static_cast<std::streamsize>(current.size()));
        current.resize(static_cast<std::size_t>(input_stream.gcount()));
        if (input_stream.bad() || (!input_stream.eof() && input_stream.fail()) ||
            current != expected) {
            throw std::invalid_argument(
                message(Message::tools_restore_modified, {arg(Placeholder::path, requested)}));
        }
    }
    const Json journal_state = {{"schema_version", journal_schema == 1 ? 1 : 2},
                                {"entries", state.at("entries")}};
    change_journal_->restore(journal_state);
    unauditable_command_paths_ = std::move(unauditable_paths);
    command_policy_violation_paths_ = std::move(policy_violation_paths);
    workspace_tracking_error_ = std::move(tracking_error);
}

} // namespace mint
