#include "mint/tools/tool_registry.hpp"

#include "mint/domain/change_journal.hpp"
#include "mint/infrastructure/change_transaction_store.hpp"
#include "mint/localization/localization.hpp"
#include "mint/runtime/task_control.hpp"

#include "change_transaction.hpp"
#include "registry/tool_arguments.hpp"
#include "registry/tool_contract.hpp"
#include "registry/tool_names.hpp"
#include "workspace/file_support.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mint {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

using tools::transaction_detail::document;
using tools::transaction_detail::FileState;
using tools::transaction_detail::next_id;
using tools::transaction_detail::restore_originals;

struct PlannedChange {
    std::string operation;
    std::filesystem::path path;
    std::string path_label;
    std::filesystem::path destination;
    std::string destination_label;
    std::string before;
    std::string after;
};

std::string required_string(const Json& object, std::string_view key) {
    const std::string name(key);
    if (!object.contains(name) || !object.at(name).is_string()) {
        throw std::invalid_argument(
            message(Message::tools_changeset_argument_string, {arg(Placeholder::name, name)}));
    }
    return object.at(name).get<std::string>();
}

std::string read_text_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > runtime_bounds::max_edit_file_bytes) {
        throw std::invalid_argument(message(Message::tools_changeset_file_too_large));
    }
    std::ifstream input(path, std::ios::binary);
    std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if ((!input.eof() && input.fail()) || tools::detail::contains_nul(content) ||
        !tools::detail::is_valid_utf8(content)) {
        throw std::invalid_argument(message(Message::tools_changeset_binary));
    }
    return content;
}

void validate_text(std::string_view field, const std::string& value, std::size_t& total_payload) {
    if (value.size() > runtime_bounds::max_edit_file_bytes || tools::detail::contains_nul(value) ||
        !tools::detail::is_valid_utf8(value)) {
        throw std::invalid_argument(
            message(Message::tools_changeset_invalid_text, {arg(Placeholder::field, field)}));
    }
    total_payload += value.size();
    if (total_payload > tools::contract::max_changeset_payload_bytes) {
        throw std::invalid_argument(message(Message::tools_changeset_payload_too_large));
    }
}

Json unapproved_changeset_result(const ApprovalDecision& decision) {
    const bool cancelled = decision.cancels_run();
    const auto status =
        decision.rejected_by_user() ? "denied" : (cancelled ? "cancelled" : "failed");
    const auto error = message(decision.rejected_by_user()
                                   ? Message::tools_changeset_denied
                                   : (cancelled ? Message::tools_changeset_cancelled
                                                : Message::tools_changeset_approval_failed));
    return {{"ok", false},
            {"status", status},
            {"operation", "changeset"},
            {"changed_files", Json::array()},
            {"rollback_performed", false},
            {"cancelled", cancelled},
            {"approval_decision_source", std::string(decision.source())},
            {"error", error}};
}

} // namespace

Json ToolRegistry::apply_changeset(const Json& arguments) const {
    tools::detail::require_only_fields(arguments, tools::name::apply_changeset, {"changes"});
    if (!arguments.contains("changes") || !arguments.at("changes").is_array()) {
        throw std::invalid_argument(message(Message::tools_changeset_changes_array));
    }
    const auto& changes = arguments.at("changes");
    if (changes.empty() || changes.size() > tools::contract::max_changes) {
        throw std::invalid_argument(message(Message::tools_changeset_operation_count));
    }

    std::vector<PlannedChange> plan;
    std::map<std::filesystem::path, FileState> originals;
    std::map<std::filesystem::path, FileState> final_states;
    std::set<std::filesystem::path> touched_paths;
    std::size_t total_payload = 0;

    const auto resolve_write_path = [&](const std::string& requested,
                                        bool require_existing_parent) {
        const std::filesystem::path input(requested);
        if (input.empty() || input == "." || input.is_absolute()) {
            throw std::invalid_argument(message(Message::tools_changeset_relative_path));
        }
        std::error_code error;
        const auto unresolved = root_ / input;
        const auto status = std::filesystem::symlink_status(unresolved, error);
        if (!error && std::filesystem::is_symlink(status)) {
            throw std::invalid_argument(
                message(Message::tools_changeset_symlink, {arg(Placeholder::path, requested)}));
        }
        const auto resolved = resolve_inside_root(requested);
        if (!is_write_allowed(resolved)) {
            throw std::invalid_argument(message(Message::tools_changeset_unauthorized,
                                                {arg(Placeholder::path, requested)}));
        }
        if (is_protected(resolved)) {
            throw std::invalid_argument(
                message(Message::tools_changeset_protected, {arg(Placeholder::path, requested)}));
        }
        if (require_existing_parent &&
            (!std::filesystem::is_directory(resolved.parent_path(), error) || error)) {
            throw std::invalid_argument(message(Message::tools_changeset_parent_missing,
                                                {arg(Placeholder::path, requested)}));
        }
        return resolved;
    };

    for (std::size_t index = 0; index < changes.size(); ++index) {
        const auto& item = changes.at(index);
        if (!item.is_object()) {
            throw std::invalid_argument(message(Message::tools_changeset_operation_object));
        }
        PlannedChange change;
        change.operation = required_string(item, "operation");
        if (change.operation == "create") {
            tools::detail::require_only_fields(item, "apply_changeset create",
                                               {"operation", "path", "new_text"});
        } else if (change.operation == "replace") {
            tools::detail::require_only_fields(item, "apply_changeset replace",
                                               {"operation", "path", "old_text", "new_text"});
        } else if (change.operation == "delete") {
            tools::detail::require_only_fields(item, "apply_changeset delete",
                                               {"operation", "path", "old_text"});
        } else if (change.operation == "move") {
            tools::detail::require_only_fields(item, "apply_changeset move",
                                               {"operation", "path", "old_text", "destination"});
        } else {
            throw std::invalid_argument(message(Message::tools_changeset_operation));
        }
        const auto requested = required_string(item, "path");
        change.path = resolve_write_path(requested, true);
        change.path_label = tools::detail::display_path(root_, change.path);
        if (!touched_paths.insert(change.path).second) {
            throw std::invalid_argument(message(Message::tools_changeset_duplicate_path,
                                                {arg(Placeholder::path, requested)}));
        }

        std::error_code error;
        const bool source_exists = std::filesystem::exists(change.path, error);
        if (error) {
            throw std::runtime_error(message(Message::tools_changeset_inspect_failed,
                                             {arg(Placeholder::path, requested)}));
        }
        if (source_exists && !std::filesystem::is_regular_file(change.path, error)) {
            throw std::invalid_argument(message(Message::tools_changeset_regular_files_only,
                                                {arg(Placeholder::path, requested)}));
        }
        FileState original;
        original.exists = source_exists;
        if (source_exists) {
            original.content = read_text_file(change.path);
        }
        originals.emplace(change.path, original);

        if (change.operation == "create") {
            if (source_exists) {
                throw std::invalid_argument(message(Message::tools_changeset_create_exists,
                                                    {arg(Placeholder::path, requested)}));
            }
            change.after = required_string(item, "new_text");
            validate_text("new_text", change.after, total_payload);
            final_states[change.path] = {true, change.after};
        } else if (change.operation == "replace") {
            if (!source_exists) {
                throw std::invalid_argument(message(Message::tools_changeset_replace_missing,
                                                    {arg(Placeholder::path, requested)}));
            }
            const auto old_text = required_string(item, "old_text");
            const auto new_text = required_string(item, "new_text");
            validate_text("old_text", old_text, total_payload);
            validate_text("new_text", new_text, total_payload);
            if (old_text.empty()) {
                throw std::invalid_argument(message(Message::tools_changeset_old_text_empty));
            }
            const auto position = original.content.find(old_text);
            if (position == std::string::npos ||
                original.content.find(old_text, position + 1) != std::string::npos) {
                throw std::invalid_argument(message(Message::tools_changeset_old_text_match,
                                                    {arg(Placeholder::path, requested)}));
            }
            change.before = original.content;
            change.after = original.content;
            change.after.replace(position, old_text.size(), new_text);
            if (change.after == change.before) {
                throw std::invalid_argument(message(Message::tools_changeset_no_change,
                                                    {arg(Placeholder::path, requested)}));
            }
            if (change.after.size() > runtime_bounds::max_edit_file_bytes) {
                throw std::invalid_argument(message(Message::tools_changeset_result_too_large,
                                                    {arg(Placeholder::path, requested)}));
            }
            final_states[change.path] = {true, change.after};
        } else if (change.operation == "delete" || change.operation == "move") {
            if (!source_exists) {
                throw std::invalid_argument(message(Message::tools_changeset_target_missing,
                                                    {arg(Placeholder::operation, change.operation),
                                                     arg(Placeholder::path, requested)}));
            }
            const auto old_text = required_string(item, "old_text");
            validate_text("old_text", old_text, total_payload);
            if (old_text != original.content) {
                throw std::invalid_argument(message(Message::tools_changeset_old_text_full_file,
                                                    {arg(Placeholder::operation, change.operation),
                                                     arg(Placeholder::path, requested)}));
            }
            change.before = original.content;
            final_states[change.path] = {false, {}};
            if (change.operation == "move") {
                const auto destination = required_string(item, "destination");
                change.destination = resolve_write_path(destination, true);
                change.destination_label = tools::detail::display_path(root_, change.destination);
                if (!touched_paths.insert(change.destination).second) {
                    throw std::invalid_argument(
                        message(Message::tools_changeset_destination_conflict,
                                {arg(Placeholder::path, destination)}));
                }
                error.clear();
                const bool destination_exists = std::filesystem::exists(change.destination, error);
                if (error || destination_exists) {
                    throw std::invalid_argument(message(Message::tools_changeset_destination_exists,
                                                        {arg(Placeholder::path, destination)}));
                }
                originals.emplace(change.destination, FileState{});
                final_states[change.destination] = {true, original.content};
            }
        }
        plan.push_back(std::move(change));
    }

    ChangeJournal preview;
    for (const auto& change : plan) {
        if (change.operation == "create") {
            preview.record_created(change.path_label, change.after);
        } else if (change.operation == "replace") {
            preview.record_modified(change.path_label, change.before, change.after);
        } else if (change.operation == "delete") {
            preview.record_deleted(change.path_label, change.before);
        } else {
            preview.record_deleted(change.path_label, change.before);
            preview.record_created(change.destination_label, change.before);
        }
    }
    const auto preview_snapshot = preview.snapshot(tools::contract::changeset_preview_bytes);
    std::vector<std::string> paths;
    for (const auto& item : preview_snapshot.at("changed_files")) {
        paths.push_back(item.at("path").get<std::string>());
    }
    if (change_set_approval_) {
        const auto decision = change_set_approval_(ChangeSetApprovalRequest{
            .paths = paths,
            .unified_diff = preview_snapshot.value("diff", ""),
            .diff_truncated = preview_snapshot.value("diff_truncated", false)});
        if (!decision.approved()) {
            if (decision.cancels_run() && task_control_ != nullptr) {
                task_control_->request_cancel();
            }
            return unapproved_changeset_result(decision);
        }
    }

    const auto journal_before = change_journal_->state();
    bool durable_transaction_started = false;
    if (change_transaction_store_ != nullptr) {
        if (pending_change_transaction_id_.has_value() || change_transaction_store_->exists()) {
            throw std::runtime_error(message(Message::tools_changeset_pending_transaction));
        }
        const auto transaction_id = next_id();
        change_transaction_store_->save(document(transaction_id, root_, originals, final_states));
        pending_change_transaction_id_ = transaction_id;
        durable_transaction_started = true;
    }
    try {
        for (const auto& [path, desired] : final_states) {
            const auto original = originals.at(path);
            if (desired.exists) {
                tools::detail::replace_file_safely(path, desired.content, original.exists);
            } else {
                tools::detail::remove_file_safely(path);
            }
        }
        for (const auto& change : plan) {
            if (change.operation == "create") {
                change_journal_->record_created(change.path_label, change.after);
            } else if (change.operation == "replace") {
                change_journal_->record_modified(change.path_label, change.before, change.after);
            } else if (change.operation == "delete") {
                change_journal_->record_deleted(change.path_label, change.before);
            } else {
                change_journal_->record_deleted(change.path_label, change.before);
                change_journal_->record_created(change.destination_label, change.before);
            }
        }
    } catch (const std::exception& commit_error) {
        std::string commit_message = commit_error.what();
        pending_change_transaction_id_.reset();
        try {
            restore_originals(originals);
            change_journal_->restore(journal_before);
            if (durable_transaction_started) {
                change_transaction_store_->clear();
            }
        } catch (const std::exception& rollback_error) {
            throw std::runtime_error(
                message(Message::tools_changeset_commit_and_rollback_failed,
                        {arg(Placeholder::commit_error, commit_message),
                         arg(Placeholder::rollback_error, rollback_error.what())}));
        }
        throw std::runtime_error(message(Message::tools_changeset_commit_rolled_back,
                                         {arg(Placeholder::error, commit_message)}));
    }

    return {{"ok", true},
            {"status", "committed"},
            {"operation", "changeset"},
            {"operation_count", plan.size()},
            {"changed_files", preview_snapshot.at("changed_files")},
            {"diff", preview_snapshot.value("diff", "")},
            {"diff_truncated", preview_snapshot.value("diff_truncated", false)},
            {"rollback_performed", false}};
}

} // namespace mint
