#include "change_transaction.hpp"

#include "mint/infrastructure/change_transaction_store.hpp"
#include "mint/localization/localization.hpp"
#include "mint/tools/tool_registry.hpp"

#include "registry/tool_contract.hpp"
#include "workspace/file_support.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace mint {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

using tools::transaction_detail::FileState;
using tools::transaction_detail::FileStates;

struct DurableChange {
    std::string path_label;
    FileState before;
    FileState after;
};

struct DurableTransaction {
    std::string id;
    std::string workspace_root;
    std::vector<DurableChange> changes;
};

constexpr int transaction_schema_version = 1;
std::atomic_uint64_t transaction_sequence{0};

bool valid_transaction_id(std::string_view id) {
    return !id.empty() && id.size() <= 128 &&
           std::all_of(id.begin(), id.end(), [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' || character == '_';
           });
}

bool same_state(const FileState& left, const FileState& right) {
    return left.exists == right.exists && (!left.exists || left.content == right.content);
}

FileState transaction_file_state(const Json& item, std::string_view exists_key,
                                 std::string_view content_key) {
    const std::string exists_name(exists_key);
    const std::string content_name(content_key);
    if (!item.contains(exists_name) || !item.at(exists_name).is_boolean() ||
        !item.contains(content_name) || !item.at(content_name).is_string()) {
        throw std::invalid_argument(message(Message::tools_transaction_invalid_file_state));
    }
    FileState state{.exists = item.at(exists_name).get<bool>(),
                    .content = item.at(content_name).get<std::string>()};
    if (state.content.size() > runtime_bounds::max_edit_file_bytes ||
        tools::detail::contains_nul(state.content) ||
        !tools::detail::is_valid_utf8(state.content) || (!state.exists && !state.content.empty())) {
        throw std::invalid_argument(message(Message::tools_transaction_invalid_file_content));
    }
    return state;
}

DurableTransaction parse_transaction(const Json& value) {
    if (!value.is_object() || value.value("schema_version", 0) != transaction_schema_version ||
        !value.contains("transaction_id") || !value.at("transaction_id").is_string() ||
        !value.contains("workspace_root") || !value.at("workspace_root").is_string() ||
        !value.contains("changes") || !value.at("changes").is_array()) {
        throw std::invalid_argument(message(Message::tools_transaction_invalid_journal));
    }

    DurableTransaction transaction;
    transaction.id = value.at("transaction_id").get<std::string>();
    transaction.workspace_root = value.at("workspace_root").get<std::string>();
    const auto& changes = value.at("changes");
    if (!valid_transaction_id(transaction.id) || transaction.workspace_root.empty() ||
        transaction.workspace_root.find('\0') != std::string::npos || changes.empty() ||
        changes.size() > tools::contract::max_changes * 2) {
        throw std::invalid_argument(message(Message::tools_transaction_invalid_state));
    }

    std::set<std::string> paths;
    for (const auto& item : changes) {
        if (!item.is_object() || !item.contains("path") || !item.at("path").is_string()) {
            throw std::invalid_argument(message(Message::tools_transaction_invalid_entry));
        }
        DurableChange change;
        change.path_label = item.at("path").get<std::string>();
        change.before = transaction_file_state(item, "before_exists", "before");
        change.after = transaction_file_state(item, "after_exists", "after");
        const std::filesystem::path relative(change.path_label);
        if (change.path_label.find('\0') != std::string::npos || relative.empty() ||
            relative == "." || relative.is_absolute() || !paths.insert(change.path_label).second ||
            same_state(change.before, change.after)) {
            throw std::invalid_argument(message(Message::tools_transaction_invalid_path_state));
        }
        transaction.changes.push_back(std::move(change));
    }
    return transaction;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > runtime_bounds::max_edit_file_bytes) {
        throw std::invalid_argument(message(Message::tools_transaction_file_too_large));
    }
    std::ifstream input(path, std::ios::binary);
    std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if ((!input.eof() && input.fail()) || tools::detail::contains_nul(content) ||
        !tools::detail::is_valid_utf8(content)) {
        throw std::invalid_argument(message(Message::tools_transaction_binary));
    }
    return content;
}

FileState read_current_state(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        throw std::runtime_error(message(Message::tools_transaction_inspect_failed,
                                         {arg(Placeholder::path, path.generic_string())}));
    }
    if (!exists) {
        return {};
    }
    if (!std::filesystem::is_regular_file(path, error) || error) {
        throw std::runtime_error(message(Message::tools_transaction_not_regular_file,
                                         {arg(Placeholder::path, path.generic_string())}));
    }
    return {.exists = true, .content = read_text_file(path)};
}

} // namespace

namespace tools::transaction_detail {

std::string next_id() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return "changeset-" + std::to_string(stamp) + '-' +
           std::to_string(transaction_sequence.fetch_add(1, std::memory_order_relaxed));
}

Json document(const std::string& id, const std::filesystem::path& root, const FileStates& originals,
              const FileStates& final_states) {
    Json changes = Json::array();
    for (const auto& [path, before] : originals) {
        const auto& after = final_states.at(path);
        changes.push_back({{"path", detail::display_path(root, path)},
                           {"before_exists", before.exists},
                           {"before", before.content},
                           {"after_exists", after.exists},
                           {"after", after.content}});
    }
    return {{"schema_version", transaction_schema_version},
            {"transaction_id", id},
            {"workspace_root", root.generic_string()},
            {"changes", std::move(changes)}};
}

void restore_originals(const FileStates& originals) {
    std::string rollback_errors;
    for (auto iterator = originals.rbegin(); iterator != originals.rend(); ++iterator) {
        const auto& [path, original] = *iterator;
        try {
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);
            if (error) {
                throw std::runtime_error(error.message());
            }
            if (original.exists) {
                detail::replace_file_safely(path, original.content, exists);
            } else if (exists) {
                detail::remove_file_safely(path);
            }
        } catch (const std::exception& error) {
            if (!rollback_errors.empty()) {
                rollback_errors += "; ";
            }
            rollback_errors += path.generic_string() + ": " + error.what();
        }
    }
    if (!rollback_errors.empty()) {
        throw std::runtime_error(message(Message::tools_transaction_rollback_incomplete,
                                         {arg(Placeholder::error, rollback_errors)}));
    }
}

} // namespace tools::transaction_detail

bool ToolRegistry::has_durable_change_transactions() const noexcept {
    return change_transaction_store_ != nullptr;
}

std::string ToolRegistry::change_transaction_path() const {
    return change_transaction_store_ == nullptr
               ? std::string{}
               : change_transaction_store_->path().generic_string();
}

std::optional<std::string> ToolRegistry::pending_change_transaction_id() const {
    return pending_change_transaction_id_;
}

ChangeTransactionRecovery ToolRegistry::reconcile_change_transaction(
    const std::optional<std::string>& checkpoint_transaction_id) {
    pending_change_transaction_id_.reset();
    if (change_transaction_store_ == nullptr) {
        if (checkpoint_transaction_id.has_value()) {
            throw std::invalid_argument(message(Message::tools_transaction_unconfigured_reference));
        }
        return ChangeTransactionRecovery::none;
    }
    if (!change_transaction_store_->exists()) {
        return ChangeTransactionRecovery::none;
    }

    const auto transaction = parse_transaction(change_transaction_store_->load());
    if (transaction.workspace_root != root_.generic_string()) {
        throw std::invalid_argument(message(Message::tools_transaction_workspace_mismatch));
    }

    struct ResolvedChange {
        std::filesystem::path path;
        FileState before;
        FileState after;
        FileState current;
    };
    std::vector<ResolvedChange> changes;
    changes.reserve(transaction.changes.size());
    for (const auto& change : transaction.changes) {
        const auto unresolved = root_ / std::filesystem::path(change.path_label);
        std::error_code error;
        const auto status = std::filesystem::symlink_status(unresolved, error);
        if (!error && std::filesystem::is_symlink(status)) {
            throw std::runtime_error(message(Message::tools_transaction_path_became_symlink,
                                             {arg(Placeholder::path, change.path_label)}));
        }
        const auto path = resolve_inside_root(change.path_label);
        if (!is_write_allowed(path) || is_protected(path)) {
            throw std::runtime_error(message(Message::tools_transaction_path_unauthorized,
                                             {arg(Placeholder::path, change.path_label)}));
        }
        changes.push_back({path, change.before, change.after, read_current_state(path)});
    }

    const bool checkpoint_committed =
        checkpoint_transaction_id.has_value() && *checkpoint_transaction_id == transaction.id;
    for (const auto& change : changes) {
        const bool valid = checkpoint_committed ? same_state(change.current, change.after)
                                                : (same_state(change.current, change.before) ||
                                                   same_state(change.current, change.after));
        if (!valid) {
            throw std::runtime_error(
                message(Message::tools_transaction_externally_modified,
                        {arg(Placeholder::path, tools::detail::display_path(root_, change.path))}));
        }
    }

    if (checkpoint_committed) {
        change_transaction_store_->clear();
        return ChangeTransactionRecovery::committed;
    }

    FileStates originals;
    for (const auto& change : changes) {
        if (!same_state(change.current, change.before)) {
            originals.emplace(change.path, change.before);
        }
    }
    if (!originals.empty()) {
        tools::transaction_detail::restore_originals(originals);
    }
    change_transaction_store_->clear();
    return ChangeTransactionRecovery::rolled_back;
}

void ToolRegistry::finalize_change_transaction() {
    if (!pending_change_transaction_id_.has_value()) {
        return;
    }
    if (change_transaction_store_ == nullptr || !change_transaction_store_->exists()) {
        throw std::runtime_error(message(Message::tools_transaction_confirmed_missing));
    }
    const auto transaction = parse_transaction(change_transaction_store_->load());
    if (transaction.id != *pending_change_transaction_id_ ||
        transaction.workspace_root != root_.generic_string()) {
        throw std::runtime_error(message(Message::tools_transaction_confirmed_mismatch));
    }
    for (const auto& change : transaction.changes) {
        const auto unresolved = root_ / std::filesystem::path(change.path_label);
        std::error_code error;
        const auto status = std::filesystem::symlink_status(unresolved, error);
        if (!error && std::filesystem::is_symlink(status)) {
            throw std::runtime_error(message(Message::tools_transaction_confirm_path_symlink,
                                             {arg(Placeholder::path, change.path_label)}));
        }
        const auto path = resolve_inside_root(change.path_label);
        if (!is_write_allowed(path) || is_protected(path) ||
            !same_state(read_current_state(path), change.after)) {
            throw std::runtime_error(message(Message::tools_transaction_confirm_path_modified,
                                             {arg(Placeholder::path, change.path_label)}));
        }
    }
    change_transaction_store_->clear();
    pending_change_transaction_id_.reset();
}

} // namespace mint
