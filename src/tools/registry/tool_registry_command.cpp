#include "tool_registry_command.hpp"

#include "mint/localization/localization.hpp"

#include "mint/domain/change_journal.hpp"
#include "mint/infrastructure/command_runner.hpp"

#include "tool_arguments.hpp"
#include "tool_contract.hpp"
#include "tool_names.hpp"
#include "workspace/file_support.hpp"
#include "workspace/workspace_support.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mint {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

void ensure_journal_matches_workspace(const ChangeJournal& journal,
                                      const tools::detail::WorkspaceSnapshot& workspace) {
    const auto journal_state = journal.state();
    for (const auto& entry : journal_state.at("entries")) {
        const auto path = entry.at("path").get<std::string>();
        const auto current = workspace.find(path);
        const bool expected_exists = entry.at("after_exists").get<bool>();
        const bool current_exists = current != workspace.end() &&
                                    current->second.kind == tools::detail::WorkspaceEntryKind::text;
        if (current_exists != expected_exists ||
            (expected_exists &&
             current->second.contents != entry.at("after").get_ref<const std::string&>())) {
            throw std::runtime_error(message(Message::tools_command_file_changed_before_run,
                                             {arg(Placeholder::path, path)}));
        }
    }
}

Json record_command_changes(
    ChangeJournal& journal,
    const std::vector<tools::detail::WorkspaceFileTransition>& transitions) {
    Json changed_files = Json::array();
    const auto journal_before = journal.state();
    try {
        for (const auto& change : transitions) {
            const auto status =
                !change.before_exists ? "created" : (!change.after_exists ? "deleted" : "modified");
            changed_files.push_back(
                {{"path", change.path},
                 {"status", status},
                 {"bytes_before", change.before_exists ? change.before.size() : 0},
                 {"bytes_after", change.after_exists ? change.after.size() : 0}});
            if (!change.before_exists) {
                journal.record_created(change.path, change.after);
            } else if (!change.after_exists) {
                journal.record_deleted(change.path, change.before);
            } else {
                journal.record_modified(change.path, change.before, change.after);
            }
        }
        const auto journal_state = journal.state();
        if (journal_state.at("entries").size() > tools::contract::max_restored_changes) {
            throw std::runtime_error(message(Message::tools_command_change_limit));
        }
    } catch (...) {
        journal.restore(journal_before);
        throw;
    }
    return changed_files;
}

void append_unique_string(std::vector<std::string>& values, const std::string& value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

} // namespace

Json ToolRegistry::execute_command(const ToolCall& call) const {
    if (call.name == tools::name::run_recipe) {
        tools::detail::require_only_fields(call.arguments, tools::name::run_recipe, {"recipe"});
    } else {
        tools::detail::require_only_fields(call.arguments, tools::name::run_command,
                                           {"program", "args", "cwd", "timeout_seconds"});
    }
    if (command_runner_ == nullptr) {
        return tools::detail::error_result(message(Message::tools_command_disabled));
    }
    if ((call.name == tools::name::run_recipe) != command_runner_->uses_recipes()) {
        return tools::detail::error_result(message(Message::tools_command_mode_mismatch));
    }
    if (change_journal_ == nullptr) {
        return command_runner_->run(call.arguments);
    }
    if (!unauditable_command_paths_.empty() || !command_policy_violation_paths_.empty() ||
        workspace_tracking_error_.has_value()) {
        return {{"ok", false},
                {"status", "workspace_tracking_failed"},
                {"verification_eligible", false},
                {"error", message(Message::tools_command_prior_audit_failed)}};
    }
    for (const auto& protected_path : protected_paths_) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(protected_path, error);
        if (status.type() == std::filesystem::file_type::not_found) {
            continue;
        }
        if (error) {
            workspace_tracking_error_ = std::string(tools::detail::workspace_tracking_error_code);
            return {{"ok", false},
                    {"status", "workspace_tracking_failed"},
                    {"workspace_changed", true},
                    {"verification_eligible", false},
                    {"error", message(Message::tools_command_protected_link_status)}};
        }
        if (!std::filesystem::is_regular_file(status)) {
            continue;
        }
        const auto links = std::filesystem::hard_link_count(protected_path, error);
        if (error || links > 1) {
            workspace_tracking_error_ = std::string(tools::detail::workspace_tracking_error_code);
            return {{"ok", false},
                    {"status", "workspace_tracking_failed"},
                    {"workspace_changed", true},
                    {"verification_eligible", false},
                    {"error", message(Message::tools_command_protected_link)}};
        }
    }

    const auto include_path = [this](const std::filesystem::path& path) {
        return !is_snapshot_entry_protected(path) &&
               !tools::detail::contains_ignored_component(root_, path);
    };
    const tools::detail::WorkspaceSnapshotLimits snapshot_limits{
        .max_entries = runtime_.workspace_snapshot_entries,
        .max_bytes = runtime_.workspace_snapshot_bytes,
        .max_text_bytes = runtime_.workspace_snapshot_text_bytes};
    tools::detail::WorkspaceSnapshot before;
    if (command_workspace_state_ != nullptr && command_workspace_state_->baseline.has_value()) {
        before = std::move(*command_workspace_state_->baseline);
        command_workspace_state_->baseline.reset();
    } else {
        before = tools::detail::capture_workspace_snapshot(root_, include_path, snapshot_limits);
    }
    ensure_journal_matches_workspace(*change_journal_, before);
    std::vector<std::string> protected_aliases_before;
    for (const auto& [path, entry] : before) {
        if ((entry.kind == tools::detail::WorkspaceEntryKind::text ||
             entry.kind == tools::detail::WorkspaceEntryKind::regular) &&
            is_protected(root_ / path)) {
            protected_aliases_before.push_back(path);
        }
    }

    const auto reconcile_result = [&](Json result) {
        try {
            auto after =
                tools::detail::capture_workspace_snapshot(root_, include_path, snapshot_limits);
            const auto report = tools::detail::workspace_file_transitions(before, after);
            if (command_workspace_state_ != nullptr) {
                command_workspace_state_->baseline = std::move(after);
            }
            std::vector<tools::detail::WorkspaceFileTransition> managed_files;
            std::vector<std::string> policy_violations;
            for (const auto& change : report.files) {
                const bool protected_alias =
                    std::find(protected_aliases_before.begin(), protected_aliases_before.end(),
                              change.path) != protected_aliases_before.end() ||
                    is_protected(root_ / change.path);
                if (protected_alias) {
                    policy_violations.push_back(change.path);
                } else if (is_write_allowed(root_ / change.path)) {
                    managed_files.push_back(change);
                } else {
                    policy_violations.push_back(change.path);
                }
            }
            std::vector<std::string> unsupported_paths;
            for (const auto& path : report.unsupported_paths) {
                if (is_write_allowed(root_ / path)) {
                    unsupported_paths.push_back(path);
                } else {
                    policy_violations.push_back(path);
                }
            }
            for (const auto& path : report.added_or_removed_directories) {
                if (!is_write_allowed(root_ / path)) {
                    policy_violations.push_back(path);
                }
            }
            if (unsupported_paths.size() + policy_violations.size() >
                tools::contract::max_restored_changes) {
                throw std::runtime_error(message(Message::tools_command_unauditable_limit));
            }
            if (!managed_files.empty() || !unsupported_paths.empty() ||
                !policy_violations.empty()) {
                auto changed_files = record_command_changes(*change_journal_, managed_files);
                for (const auto& path : unsupported_paths) {
                    append_unique_string(unauditable_command_paths_, path);
                    changed_files.push_back({{"path", path},
                                             {"status", "unauditable"},
                                             {"bytes_before", nullptr},
                                             {"bytes_after", nullptr}});
                }
                for (const auto& path : policy_violations) {
                    append_unique_string(command_policy_violation_paths_, path);
                    changed_files.push_back({{"path", path},
                                             {"status", "policy_violation"},
                                             {"bytes_before", nullptr},
                                             {"bytes_after", nullptr}});
                }
                result["changed_files"] = std::move(changed_files);
                result["workspace_changed"] = true;
                result["verification_eligible"] = false;
            }
            if (!unsupported_paths.empty() || !policy_violations.empty()) {
                result["ok"] = false;
                result["status"] =
                    policy_violations.empty() ? "unauditable_workspace_change" : "policy_violation";
                result["error"] =
                    message(policy_violations.empty() ? Message::tools_command_unsafe_diff
                                                      : Message::tools_command_policy_violation);
            }
        } catch (const std::exception&) {
            workspace_tracking_error_ = std::string(tools::detail::workspace_tracking_error_code);
            result["ok"] = false;
            result["status"] = "workspace_tracking_failed";
            result["workspace_changed"] = true;
            result["verification_eligible"] = false;
            result["changed_files"] = Json::array({{{"path", "<workspace>"},
                                                    {"status", "unauditable"},
                                                    {"bytes_before", nullptr},
                                                    {"bytes_after", nullptr}}});
            result["error"] = message(Message::tools_command_archive_failed);
        }
        return result;
    };

    try {
        return reconcile_result(command_runner_->run(call.arguments));
    } catch (const std::exception& error) {
        return reconcile_result(tools::detail::error_result(error.what()));
    }
}

} // namespace mint
