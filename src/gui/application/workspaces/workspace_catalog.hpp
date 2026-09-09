#pragma once

#include "application/desktop_types.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace mint::desktop {

[[nodiscard]] std::filesystem::path normalize_workspace_root(const std::filesystem::path& root);

struct WorkspaceCatalogState {
    std::vector<std::filesystem::path> roots;
    std::vector<WorkspaceTaskCounts> task_counts;
    std::filesystem::path last_active_root;

    bool operator==(const WorkspaceCatalogState&) const = default;
};

[[nodiscard]] bool is_usable_workspace_root(const std::filesystem::path& root,
                                            const std::filesystem::path& state_root);
[[nodiscard]] std::filesystem::path
select_startup_workspace(const std::filesystem::path& requested_root,
                         const WorkspaceCatalogState& catalog,
                         const std::filesystem::path& state_root);

class WorkspaceCatalogRepository {
  public:
    virtual ~WorkspaceCatalogRepository() = default;

    [[nodiscard]] virtual std::optional<WorkspaceCatalogState> load() const = 0;
    [[nodiscard]] virtual bool save(const WorkspaceCatalogState& catalog) = 0;
};

class WorkspaceCatalog final {
  public:
    explicit WorkspaceCatalog(WorkspaceCatalogRepository* repository = nullptr);

    [[nodiscard]] bool initialize(const std::filesystem::path& initial_root,
                                  const std::filesystem::path& state_root = {});
    [[nodiscard]] bool add(const std::filesystem::path& root);
    [[nodiscard]] bool remove(const std::filesystem::path& root);
    [[nodiscard]] bool set_task_counts(const std::filesystem::path& root, std::size_t unarchived,
                                       std::size_t archived);
    [[nodiscard]] const WorkspaceCatalogState& state() const noexcept;

  private:
    [[nodiscard]] bool commit(WorkspaceCatalogState next);

    WorkspaceCatalogRepository* repository_ = nullptr;
    WorkspaceCatalogState state_;
};

} // namespace mint::desktop
