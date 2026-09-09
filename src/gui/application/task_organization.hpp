#pragma once

#include "application/desktop_types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace mint::desktop {

struct TaskOrganization {
    std::unordered_set<std::string> pinned_task_ids;
    std::unordered_set<std::string> archived_task_ids;

    bool operator==(const TaskOrganization&) const = default;
};

class TaskOrganizationRepository {
  public:
    virtual ~TaskOrganizationRepository() = default;

    [[nodiscard]] virtual std::optional<TaskOrganization>
    load(std::string_view project_scope) const = 0;
    [[nodiscard]] virtual bool save(std::string_view project_scope,
                                    const TaskOrganization& organization) = 0;
};

class TaskOrganizer final {
  public:
    explicit TaskOrganizer(TaskOrganizationRepository* repository = nullptr);

    [[nodiscard]] bool open_project(const std::filesystem::path& root);
    [[nodiscard]] bool reconcile(const std::vector<TaskSummary>& tasks);
    [[nodiscard]] bool toggle_pin(std::string_view task_id);
    [[nodiscard]] bool archive(std::string_view task_id);
    [[nodiscard]] bool unarchive(std::string_view task_id);

    [[nodiscard]] const TaskOrganization& state() const noexcept;
    [[nodiscard]] bool is_pinned(std::string_view task_id) const;
    [[nodiscard]] bool is_archived(std::string_view task_id) const;

  private:
    [[nodiscard]] bool commit(TaskOrganization organization);

    TaskOrganizationRepository* repository_ = nullptr;
    bool project_open_ = false;
    std::string project_scope_;
    TaskOrganization state_;
};

} // namespace mint::desktop
