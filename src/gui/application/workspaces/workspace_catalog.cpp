#include "workspace_catalog.hpp"

#include <algorithm>
#include <utility>

namespace mint::desktop {
std::filesystem::path normalize_workspace_root(const std::filesystem::path& root) {
    if (root.empty()) {
        return {};
    }
    std::error_code error;
    const auto absolute = std::filesystem::absolute(root, error);
    if (error) {
        return root.lexically_normal();
    }
    const auto canonical = std::filesystem::weakly_canonical(absolute, error);
    return error ? absolute.lexically_normal() : canonical.lexically_normal();
}

namespace {

bool contains_root(const WorkspaceCatalogState& catalog, const std::filesystem::path& root) {
    return std::find(catalog.roots.begin(), catalog.roots.end(), root) != catalog.roots.end();
}

bool contains_path(const std::filesystem::path& parent, const std::filesystem::path& child) {
    const auto mismatch = std::mismatch(parent.begin(), parent.end(), child.begin(), child.end());
    return mismatch.first == parent.end();
}

} // namespace

bool is_usable_workspace_root(const std::filesystem::path& root,
                              const std::filesystem::path& state_root) {
    const auto candidate = normalize_workspace_root(root);
    std::error_code error;
    if (candidate.empty() || !candidate.is_absolute() || candidate == candidate.root_path() ||
        !std::filesystem::is_directory(candidate, error) || error) {
        return false;
    }
    const auto state = normalize_workspace_root(state_root);
    return state.empty() || (!contains_path(candidate, state) && !contains_path(state, candidate));
}

std::filesystem::path select_startup_workspace(const std::filesystem::path& requested_root,
                                               const WorkspaceCatalogState& catalog,
                                               const std::filesystem::path& state_root) {
    if (!requested_root.empty()) {
        // EN: An invalid explicit choice must never silently open a different project.
        // ZH-CN: 显式选择无效时，绝不静默切换到其他项目。
        return is_usable_workspace_root(requested_root, state_root)
                   ? normalize_workspace_root(requested_root)
                   : std::filesystem::path{};
    }
    const auto restorable = [&](const auto& root) {
        // EN: Legacy relative paths lack their original working directory and cannot be restored.
        // ZH-CN: 旧的相对路径缺少原工作目录，不能据此自动恢复项目。
        return root.is_absolute() && is_usable_workspace_root(root, state_root);
    };
    if (restorable(catalog.last_active_root)) {
        const auto recent = normalize_workspace_root(catalog.last_active_root);
        if (std::any_of(catalog.roots.begin(), catalog.roots.end(), [&](const auto& root) {
                return root.is_absolute() && normalize_workspace_root(root) == recent;
            })) {
            return recent;
        }
    }
    for (auto root = catalog.roots.rbegin(); root != catalog.roots.rend(); ++root) {
        if (restorable(*root)) {
            return normalize_workspace_root(*root);
        }
    }
    return {};
}

WorkspaceCatalog::WorkspaceCatalog(WorkspaceCatalogRepository* repository)
    : repository_(repository) {}

bool WorkspaceCatalog::initialize(const std::filesystem::path& initial_root,
                                  const std::filesystem::path& state_root) {
    WorkspaceCatalogState next;
    if (repository_ != nullptr) {
        const auto stored = repository_->load();
        if (!stored.has_value()) {
            return false;
        }
        next = *stored;
    }

    std::vector<std::filesystem::path> unique_roots;
    unique_roots.reserve(next.roots.size() + 1);
    for (const auto& stored_root : next.roots) {
        if (!state_root.empty() &&
            (!stored_root.is_absolute() || !is_usable_workspace_root(stored_root, state_root))) {
            continue;
        }
        const auto candidate = normalize_workspace_root(stored_root);
        if (!candidate.empty() &&
            std::find(unique_roots.begin(), unique_roots.end(), candidate) == unique_roots.end()) {
            unique_roots.push_back(candidate);
        }
    }
    next.roots = std::move(unique_roots);

    std::vector<WorkspaceTaskCounts> known_task_counts;
    known_task_counts.reserve(next.task_counts.size());
    for (auto counts : next.task_counts) {
        counts.root = normalize_workspace_root(counts.root);
        const auto duplicate =
            std::find_if(known_task_counts.begin(), known_task_counts.end(),
                         [&](const auto& known) { return known.root == counts.root; });
        if (!counts.root.empty() && contains_root(next, counts.root) &&
            duplicate == known_task_counts.end()) {
            known_task_counts.push_back(std::move(counts));
        }
    }
    next.task_counts = std::move(known_task_counts);
    next.last_active_root = normalize_workspace_root(next.last_active_root);
    if (!contains_root(next, next.last_active_root)) {
        next.last_active_root.clear();
    }

    const auto candidate = normalize_workspace_root(initial_root);
    if (!candidate.empty() && !contains_root(next, candidate)) {
        next.roots.push_back(candidate);
    }
    if (!candidate.empty()) {
        next.last_active_root = candidate;
    }
    return commit(std::move(next));
}

bool WorkspaceCatalog::add(const std::filesystem::path& root) {
    const auto candidate = normalize_workspace_root(root);
    if (candidate.empty()) {
        return false;
    }
    if (contains_root(state_, candidate) && state_.last_active_root == candidate) {
        return true;
    }
    auto next = state_;
    if (!contains_root(next, candidate)) {
        next.roots.push_back(candidate);
    }
    next.last_active_root = candidate;
    return commit(std::move(next));
}

bool WorkspaceCatalog::remove(const std::filesystem::path& root) {
    const auto candidate = normalize_workspace_root(root);
    const auto found = std::find(state_.roots.begin(), state_.roots.end(), candidate);
    if (candidate.empty() || found == state_.roots.end() || state_.roots.size() <= 1) {
        return false;
    }
    auto next = state_;
    const auto position = static_cast<std::size_t>(std::distance(state_.roots.begin(), found));
    next.roots.erase(next.roots.begin() + static_cast<std::ptrdiff_t>(position));
    if (next.last_active_root == candidate) {
        next.last_active_root = next.roots[std::min(position, next.roots.size() - 1)];
    }
    next.task_counts.erase(
        std::remove_if(next.task_counts.begin(), next.task_counts.end(),
                       [&](const auto& counts) { return counts.root == candidate; }),
        next.task_counts.end());
    return commit(std::move(next));
}

bool WorkspaceCatalog::set_task_counts(const std::filesystem::path& root, std::size_t unarchived,
                                       std::size_t archived) {
    const auto candidate = normalize_workspace_root(root);
    if (candidate.empty() || !contains_root(state_, candidate)) {
        return false;
    }
    auto next = state_;
    const auto found = std::find_if(next.task_counts.begin(), next.task_counts.end(),
                                    [&](const auto& counts) { return counts.root == candidate; });
    if (found == next.task_counts.end()) {
        next.task_counts.push_back(
            {.root = candidate, .unarchived = unarchived, .archived = archived});
    } else {
        found->unarchived = unarchived;
        found->archived = archived;
    }
    return commit(std::move(next));
}

const WorkspaceCatalogState& WorkspaceCatalog::state() const noexcept {
    return state_;
}

bool WorkspaceCatalog::commit(WorkspaceCatalogState next) {
    if (repository_ != nullptr && !repository_->save(next)) {
        return false;
    }
    state_ = std::move(next);
    return true;
}

} // namespace mint::desktop
