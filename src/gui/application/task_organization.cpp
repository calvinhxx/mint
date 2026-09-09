#include "task_organization.hpp"

#include <algorithm>
#include <utility>

namespace mint::desktop {

TaskOrganizer::TaskOrganizer(TaskOrganizationRepository* repository) : repository_(repository) {}

bool TaskOrganizer::open_project(const std::filesystem::path& root) {
    const auto next_scope = root.lexically_normal().generic_string();
    if (project_open_ && next_scope == project_scope_) {
        return true;
    }

    TaskOrganization next_state;
    if (repository_ == nullptr) {
        next_state = {};
    } else {
        const auto stored = repository_->load(next_scope);
        if (!stored.has_value()) {
            return false;
        }
        next_state = *stored;
    }

    project_scope_ = next_scope;
    state_ = std::move(next_state);
    project_open_ = true;
    return true;
}

bool TaskOrganizer::reconcile(const std::vector<TaskSummary>& tasks) {
    std::unordered_set<std::string> available;
    available.reserve(tasks.size());
    for (const auto& task : tasks) {
        available.insert(task.id);
    }

    auto next = state_;
    std::erase_if(next.pinned_task_ids,
                  [&available](const auto& id) { return !available.contains(id); });
    std::erase_if(next.archived_task_ids,
                  [&available](const auto& id) { return !available.contains(id); });
    if (next == state_) {
        return true;
    }
    return commit(std::move(next));
}

bool TaskOrganizer::toggle_pin(std::string_view task_id) {
    if (task_id.empty() || is_archived(task_id)) {
        return false;
    }

    auto next = state_;
    const auto id = std::string(task_id);
    if (!next.pinned_task_ids.erase(id)) {
        next.pinned_task_ids.insert(id);
    }
    return commit(std::move(next));
}

bool TaskOrganizer::archive(std::string_view task_id) {
    if (task_id.empty() || is_archived(task_id)) {
        return false;
    }

    auto next = state_;
    const auto id = std::string(task_id);
    next.pinned_task_ids.erase(id);
    next.archived_task_ids.insert(id);
    return commit(std::move(next));
}

bool TaskOrganizer::unarchive(std::string_view task_id) {
    if (task_id.empty() || !is_archived(task_id)) {
        return false;
    }

    auto next = state_;
    next.archived_task_ids.erase(std::string(task_id));
    return commit(std::move(next));
}

const TaskOrganization& TaskOrganizer::state() const noexcept {
    return state_;
}

bool TaskOrganizer::is_pinned(std::string_view task_id) const {
    return state_.pinned_task_ids.contains(std::string(task_id));
}

bool TaskOrganizer::is_archived(std::string_view task_id) const {
    return state_.archived_task_ids.contains(std::string(task_id));
}

bool TaskOrganizer::commit(TaskOrganization organization) {
    if (repository_ != nullptr && !repository_->save(project_scope_, organization)) {
        return false;
    }
    state_ = std::move(organization);
    return true;
}

} // namespace mint::desktop
