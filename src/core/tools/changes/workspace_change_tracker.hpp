#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "mint/domain/runtime_settings.hpp"

namespace mint::tools::detail {

enum class WorkspaceEntryKind { text, regular, directory, symlink, other };

struct WorkspaceEntrySnapshot {
    WorkspaceEntryKind kind = WorkspaceEntryKind::other;
    std::filesystem::perms permissions = std::filesystem::perms::unknown;
    std::string contents{};
    std::uintmax_t size = 0;
};

using WorkspaceSnapshot = std::map<std::string, WorkspaceEntrySnapshot>;
using WorkspacePathFilter = std::function<bool(const std::filesystem::path&)>;

struct WorkspaceSnapshotLimits {
    std::size_t max_entries = runtime_defaults::workspace_snapshot_entries;
    std::uintmax_t max_bytes = runtime_defaults::workspace_snapshot_bytes;
    std::size_t max_text_bytes = runtime_defaults::workspace_snapshot_text_bytes;
};

struct WorkspaceFileTransition {
    std::string path{};
    bool before_exists = false;
    std::string before{};
    bool after_exists = false;
    std::string after{};
};

struct WorkspaceTransitionReport {
    std::vector<WorkspaceFileTransition> files{};
    std::vector<std::string> added_or_removed_directories{};
    std::vector<std::string> unsupported_paths{};
};

[[nodiscard]] WorkspaceSnapshot
capture_workspace_snapshot(const std::filesystem::path& root,
                           const WorkspacePathFilter& include_path,
                           const WorkspaceSnapshotLimits& limits = {});

[[nodiscard]] WorkspaceTransitionReport workspace_file_transitions(const WorkspaceSnapshot& before,
                                                                   const WorkspaceSnapshot& after);

} // namespace mint::tools::detail
