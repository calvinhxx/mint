#include "workspace_change_tracker.hpp"

#include "mint/localization/localization.hpp"

#include "workspace/file_support.hpp"
#include "workspace/workspace_support.hpp"

#include <array>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace mint::tools::detail {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

void add_bytes(std::uintmax_t bytes, std::uintmax_t limit, std::uintmax_t& total,
               const std::filesystem::path& path) {
    if (bytes > limit - total) {
        throw std::runtime_error(
            message(Message::tools_tracker_byte_limit, {arg(Placeholder::path, path.string())}));
    }
    total += bytes;
}

WorkspaceEntrySnapshot snapshot_regular_file(const std::filesystem::directory_entry& entry,
                                             const WorkspaceSnapshotLimits& limits,
                                             std::uintmax_t& total_bytes, std::size_t& text_bytes) {
    std::error_code error;
    const auto size = entry.file_size(error);
    if (error) {
        throw std::runtime_error(message(Message::tools_tracker_size_failed,
                                         {arg(Placeholder::path, entry.path().string())}));
    }
    if (size > limits.max_bytes - total_bytes) {
        throw std::runtime_error(message(Message::tools_tracker_byte_limit,
                                         {arg(Placeholder::path, entry.path().string())}));
    }

    std::ifstream input(entry.path(), std::ios::binary);
    if (!input) {
        throw std::runtime_error(message(Message::tools_tracker_open_failed,
                                         {arg(Placeholder::path, entry.path().string())}));
    }
    std::string contents;
    contents.reserve(static_cast<std::size_t>(size));
    std::uintmax_t actual_size = 0;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) {
            continue;
        }
        const auto accepted = static_cast<std::size_t>(count);
        add_bytes(accepted, limits.max_bytes, total_bytes, entry.path());
        actual_size += accepted;
        contents.append(buffer.data(), accepted);
    }
    if (!input.eof()) {
        throw std::runtime_error(message(Message::tools_tracker_read_failed,
                                         {arg(Placeholder::path, entry.path().string())}));
    }
    if (contents.size() <= runtime_bounds::max_edit_file_bytes && !contains_nul(contents) &&
        is_valid_utf8(contents)) {
        if (contents.size() > limits.max_text_bytes - text_bytes) {
            throw std::runtime_error(message(Message::tools_tracker_text_limit,
                                             {arg(Placeholder::path, entry.path().string())}));
        }
        text_bytes += contents.size();
        return {
            .kind = WorkspaceEntryKind::text, .contents = std::move(contents), .size = actual_size};
    }
    return {
        .kind = WorkspaceEntryKind::regular, .contents = std::move(contents), .size = actual_size};
}

WorkspaceEntrySnapshot snapshot_entry(const std::filesystem::directory_entry& entry,
                                      const std::filesystem::file_status& status,
                                      const WorkspaceSnapshotLimits& limits,
                                      std::uintmax_t& total_bytes, std::size_t& text_bytes) {
    if (std::filesystem::is_regular_file(status)) {
        auto result = snapshot_regular_file(entry, limits, total_bytes, text_bytes);
        result.permissions = status.permissions();
        return result;
    }
    if (std::filesystem::is_directory(status)) {
        return {.kind = WorkspaceEntryKind::directory, .permissions = status.permissions()};
    }
    if (std::filesystem::is_symlink(status)) {
        std::error_code error;
        const auto target = std::filesystem::read_symlink(entry.path(), error);
        if (error) {
            throw std::runtime_error(message(Message::tools_tracker_symlink_failed,
                                             {arg(Placeholder::path, entry.path().string())}));
        }
        const auto label = target.generic_string();
        add_bytes(label.size(), limits.max_bytes, total_bytes, entry.path());
        return {
            .kind = WorkspaceEntryKind::symlink,
            .permissions = status.permissions(),
            .contents = label,
            .size = label.size(),
        };
    }
    return {.permissions = status.permissions()};
}

bool is_text(const WorkspaceEntrySnapshot& entry) {
    return entry.kind == WorkspaceEntryKind::text;
}

bool same_entry(const WorkspaceEntrySnapshot& left, const WorkspaceEntrySnapshot& right) {
    if (left.kind != right.kind || left.permissions != right.permissions) {
        return false;
    }
    if (left.kind == WorkspaceEntryKind::text || left.kind == WorkspaceEntryKind::regular ||
        left.kind == WorkspaceEntryKind::symlink) {
        return left.contents == right.contents;
    }
    return left.size == right.size;
}

} // namespace

WorkspaceSnapshot capture_workspace_snapshot(const std::filesystem::path& root,
                                             const WorkspacePathFilter& include_path,
                                             const WorkspaceSnapshotLimits& limits) {
    if (limits.max_entries == 0 ||
        limits.max_entries > runtime_bounds::max_workspace_snapshot_entries ||
        limits.max_bytes == 0 || limits.max_bytes > runtime_bounds::max_workspace_snapshot_bytes ||
        limits.max_text_bytes == 0 ||
        limits.max_text_bytes > runtime_bounds::max_workspace_snapshot_text_bytes ||
        limits.max_text_bytes > limits.max_bytes) {
        throw std::invalid_argument(message(Message::tools_tracker_invalid_limits));
    }
    WorkspaceSnapshot result;
    std::uintmax_t total_bytes = 0;
    std::size_t text_bytes = 0;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(root, error);
    if (error) {
        throw std::runtime_error(
            message(Message::tools_tracker_scan_failed, {arg(Placeholder::path, root.string())}));
    }

    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        const auto path = iterator->path();
        const auto label = display_path(root, path);
        if (!is_valid_utf8(label)) {
            throw std::runtime_error(message(Message::tools_tracker_unsafe_path));
        }
        error.clear();
        const auto status = iterator->symlink_status(error);
        if (error) {
            throw std::runtime_error(
                message(Message::tools_tracker_inspect_failed, {arg(Placeholder::path, label)}));
        }
        if (std::filesystem::is_directory(status) && is_ignored_directory(path)) {
            iterator.disable_recursion_pending();
        } else if (include_path(path)) {
            if (result.size() == limits.max_entries) {
                throw std::runtime_error(
                    message(Message::tools_tracker_entry_limit, {arg(Placeholder::path, label)}));
            }
            result.emplace(label,
                           snapshot_entry(*iterator, status, limits, total_bytes, text_bytes));
        }

        error.clear();
        iterator.increment(error);
        if (error) {
            throw std::runtime_error(message(Message::tools_tracker_scan_continue_failed,
                                             {arg(Placeholder::path, label)}));
        }
    }
    return result;
}

WorkspaceTransitionReport workspace_file_transitions(const WorkspaceSnapshot& before,
                                                     const WorkspaceSnapshot& after) {
    WorkspaceTransitionReport result;
    auto old_entry = before.begin();
    auto new_entry = after.begin();
    while (old_entry != before.end() || new_entry != after.end()) {
        if (new_entry == after.end() ||
            (old_entry != before.end() && old_entry->first < new_entry->first)) {
            if (is_text(old_entry->second)) {
                result.files.push_back({.path = old_entry->first,
                                        .before_exists = true,
                                        .before = old_entry->second.contents});
            } else if (old_entry->second.kind == WorkspaceEntryKind::directory) {
                result.added_or_removed_directories.push_back(old_entry->first);
            } else {
                result.unsupported_paths.push_back(old_entry->first);
            }
            ++old_entry;
            continue;
        }
        if (old_entry == before.end() || new_entry->first < old_entry->first) {
            if (is_text(new_entry->second)) {
                result.files.push_back({.path = new_entry->first,
                                        .after_exists = true,
                                        .after = new_entry->second.contents});
            } else if (new_entry->second.kind == WorkspaceEntryKind::directory) {
                result.added_or_removed_directories.push_back(new_entry->first);
            } else {
                result.unsupported_paths.push_back(new_entry->first);
            }
            ++new_entry;
            continue;
        }

        if (!same_entry(old_entry->second, new_entry->second)) {
            const bool permissions_changed =
                old_entry->second.permissions != new_entry->second.permissions;
            if (!permissions_changed && is_text(old_entry->second) && is_text(new_entry->second)) {
                result.files.push_back({.path = old_entry->first,
                                        .before_exists = true,
                                        .before = old_entry->second.contents,
                                        .after_exists = true,
                                        .after = new_entry->second.contents});
            } else {
                result.unsupported_paths.push_back(old_entry->first);
            }
        }
        ++old_entry;
        ++new_entry;
    }
    return result;
}

} // namespace mint::tools::detail
