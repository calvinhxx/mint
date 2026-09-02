#include "mint/domain/change_journal.hpp"

#include "mint/localization/localization.hpp"

#include <algorithm>
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

constexpr std::size_t context_lines = 3;

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> result;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto newline = text.find('\n', begin);
        if (newline == std::string_view::npos) {
            result.push_back(text.substr(begin));
            break;
        }
        result.push_back(text.substr(begin, newline - begin + 1));
        begin = newline + 1;
    }
    return result;
}

void append_diff_line(std::string& output, char prefix, std::string_view line) {
    output.push_back(prefix);
    output.append(line);
    if (line.empty() || line.back() != '\n') {
        output += "\n\\ No newline at end of file\n";
    }
}

void append_created_diff(std::string& output, const std::string& path,
                         const std::string& contents) {
    const auto lines = split_lines(contents);
    output += "--- /dev/null\n+++ b/" + path + "\n";
    if (lines.empty()) {
        output += "@@ -0,0 +0,0 @@\n";
        return;
    }
    output += "@@ -0,0 +1," + std::to_string(lines.size()) + " @@\n";
    for (const auto line : lines) {
        append_diff_line(output, '+', line);
    }
}

void append_deleted_diff(std::string& output, const std::string& path,
                         const std::string& contents) {
    const auto lines = split_lines(contents);
    output += "--- a/" + path + "\n+++ /dev/null\n";
    if (lines.empty()) {
        output += "@@ -0,0 +0,0 @@\n";
        return;
    }
    output += "@@ -1," + std::to_string(lines.size()) + " +0,0 @@\n";
    for (const auto line : lines) {
        append_diff_line(output, '-', line);
    }
}

void append_modified_diff(std::string& output, const std::string& path, const std::string& before,
                          const std::string& after) {
    const auto old_lines = split_lines(before);
    const auto new_lines = split_lines(after);

    std::size_t prefix = 0;
    while (prefix < old_lines.size() && prefix < new_lines.size() &&
           old_lines[prefix] == new_lines[prefix]) {
        ++prefix;
    }

    std::size_t suffix = 0;
    while (suffix < old_lines.size() - prefix && suffix < new_lines.size() - prefix &&
           old_lines[old_lines.size() - suffix - 1] == new_lines[new_lines.size() - suffix - 1]) {
        ++suffix;
    }

    const auto leading_context = std::min(prefix, context_lines);
    const auto trailing_context = std::min(suffix, context_lines);
    const auto old_hunk_begin = prefix - leading_context;
    const auto new_hunk_begin = prefix - leading_context;
    const auto old_change_end = old_lines.size() - suffix;
    const auto new_change_end = new_lines.size() - suffix;
    const auto old_hunk_end = old_change_end + trailing_context;
    const auto new_hunk_end = new_change_end + trailing_context;
    const auto old_count = old_hunk_end - old_hunk_begin;
    const auto new_count = new_hunk_end - new_hunk_begin;
    const auto old_start = old_count == 0 ? old_hunk_begin : old_hunk_begin + 1;
    const auto new_start = new_count == 0 ? new_hunk_begin : new_hunk_begin + 1;

    output += "--- a/" + path + "\n+++ b/" + path + "\n";
    output += "@@ -" + std::to_string(old_start) + ',' + std::to_string(old_count) + " +" +
              std::to_string(new_start) + ',' + std::to_string(new_count) + " @@\n";

    for (std::size_t index = old_hunk_begin; index < prefix; ++index) {
        append_diff_line(output, ' ', old_lines[index]);
    }
    for (std::size_t index = prefix; index < old_change_end; ++index) {
        append_diff_line(output, '-', old_lines[index]);
    }
    for (std::size_t index = prefix; index < new_change_end; ++index) {
        append_diff_line(output, '+', new_lines[index]);
    }
    for (std::size_t index = old_change_end; index < old_hunk_end; ++index) {
        append_diff_line(output, ' ', old_lines[index]);
    }
}

void truncate_utf8_at_boundary(std::string& value, std::size_t byte_limit) {
    if (value.size() <= byte_limit) {
        return;
    }
    auto boundary = byte_limit;
    while (boundary > 0 && (static_cast<unsigned char>(value[boundary]) & 0xC0U) == 0x80U) {
        --boundary;
    }
    value.resize(boundary);
    value += "\n... diff truncated ...\n";
}

} // namespace

void ChangeJournal::record_created(std::string path, std::string contents) {
    record_transition(std::move(path), false, {}, true, std::move(contents));
}

void ChangeJournal::record_modified(std::string path, std::string before, std::string after) {
    record_transition(std::move(path), true, std::move(before), true, std::move(after));
}

void ChangeJournal::record_deleted(std::string path, std::string contents) {
    record_transition(std::move(path), true, std::move(contents), false, {});
}

void ChangeJournal::record_transition(std::string path, bool before_exists, std::string before,
                                      bool after_exists, std::string after) {
    if (path.empty()) {
        throw std::invalid_argument(message(Message::journal_path_empty));
    }
    std::scoped_lock lock(mutex_);
    auto existing = entries_.find(path);
    if (existing == entries_.end()) {
        if (before_exists == after_exists && before == after) {
            return;
        }
        entries_.emplace(std::move(path), Entry{.before_exists = before_exists,
                                                .before = std::move(before),
                                                .after_exists = after_exists,
                                                .after = std::move(after)});
        return;
    }

    if (existing->second.after_exists != before_exists || existing->second.after != before) {
        throw std::invalid_argument(
            message(Message::journal_state_mismatch, {arg(Placeholder::path, path)}));
    }
    existing->second.after_exists = after_exists;
    existing->second.after = std::move(after);
    if (existing->second.before_exists == existing->second.after_exists &&
        existing->second.before == existing->second.after) {
        entries_.erase(existing);
    }
}

bool ChangeJournal::has_changes() const {
    std::scoped_lock lock(mutex_);
    return !entries_.empty();
}

Json ChangeJournal::snapshot(std::size_t diff_byte_limit) const {
    std::scoped_lock lock(mutex_);
    Json files = Json::array();
    std::string diff;

    for (const auto& [path, entry] : entries_) {
        files.push_back(
            {{"path", path},
             {"status",
              !entry.before_exists ? "created" : (!entry.after_exists ? "deleted" : "modified")},
             {"bytes_before", entry.before_exists ? entry.before.size() : 0},
             {"bytes_after", entry.after_exists ? entry.after.size() : 0}});
        if (!entry.before_exists) {
            append_created_diff(diff, path, entry.after);
        } else if (!entry.after_exists) {
            append_deleted_diff(diff, path, entry.before);
        } else {
            append_modified_diff(diff, path, entry.before, entry.after);
        }
    }

    const bool truncated = diff.size() > diff_byte_limit;
    truncate_utf8_at_boundary(diff, diff_byte_limit);
    return {{"ok", true},
            {"changed_files", std::move(files)},
            {"diff", std::move(diff)},
            {"diff_truncated", truncated}};
}

Json ChangeJournal::state() const {
    std::scoped_lock lock(mutex_);
    Json entries = Json::array();
    for (const auto& [path, entry] : entries_) {
        entries.push_back({{"path", path},
                           {"before_exists", entry.before_exists},
                           {"before", entry.before},
                           {"after_exists", entry.after_exists},
                           {"after", entry.after}});
    }
    return {{"schema_version", 2}, {"entries", std::move(entries)}};
}

void ChangeJournal::restore(const Json& state) {
    const auto schema_version = state.is_object() ? state.value("schema_version", 0) : 0;
    if (!state.is_object() || (schema_version != 1 && schema_version != 2) ||
        !state.contains("entries") || !state.at("entries").is_array()) {
        throw std::invalid_argument(message(Message::journal_invalid_snapshot));
    }

    std::map<std::string, Entry> restored;
    for (const auto& item : state.at("entries")) {
        if (!item.is_object() || !item.contains("path") || !item.at("path").is_string() ||
            !item.contains("before") || !item.at("before").is_string() || !item.contains("after") ||
            !item.at("after").is_string()) {
            throw std::invalid_argument(message(Message::journal_invalid_entry));
        }
        const auto path = item.at("path").get<std::string>();
        bool before_exists = true;
        bool after_exists = true;
        if (schema_version == 1) {
            if (!item.contains("created") || !item.at("created").is_boolean()) {
                throw std::invalid_argument(message(Message::journal_v1_missing_created));
            }
            before_exists = !item.at("created").get<bool>();
        } else {
            if (!item.contains("before_exists") || !item.at("before_exists").is_boolean() ||
                !item.contains("after_exists") || !item.at("after_exists").is_boolean()) {
                throw std::invalid_argument(message(Message::journal_v2_missing_state));
            }
            before_exists = item.at("before_exists").get<bool>();
            after_exists = item.at("after_exists").get<bool>();
        }
        auto before = item.at("before").get<std::string>();
        auto after = item.at("after").get<std::string>();
        if (path.empty() || (!before_exists && !before.empty()) ||
            (!after_exists && !after.empty()) ||
            (before_exists == after_exists && before == after)) {
            throw std::invalid_argument(message(Message::journal_invalid_entry_state));
        }
        if (!restored
                 .emplace(path, Entry{.before_exists = before_exists,
                                      .before = std::move(before),
                                      .after_exists = after_exists,
                                      .after = std::move(after)})
                 .second) {
            throw std::invalid_argument(
                message(Message::journal_duplicate_path, {arg(Placeholder::path, path)}));
        }
    }

    std::scoped_lock lock(mutex_);
    entries_ = std::move(restored);
}

} // namespace mint
