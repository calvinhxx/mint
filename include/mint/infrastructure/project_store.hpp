#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mint/domain/model.hpp"

namespace mint {

enum class ManagedTaskMode { model, demo };

[[nodiscard]] std::string_view managed_task_mode_name(ManagedTaskMode mode) noexcept;

struct ManagedTaskPaths {
    std::string id;
    std::filesystem::path directory;
    std::filesystem::path metadata;
    std::filesystem::path policy;
    std::filesystem::path session;
    std::filesystem::path events;
};

struct ManagedTaskSummary {
    std::string id;
    std::string question;
    std::string created_at;
    std::string status = "created";
    std::string verification_status = "not_required";
    ManagedTaskMode mode = ManagedTaskMode::model;
    std::size_t turns = 0;
    bool completed = false;
    bool resumable = false;
};

[[nodiscard]] std::filesystem::path default_mint_state_directory();
[[nodiscard]] Json managed_task_summary_to_json(const ManagedTaskSummary& summary);

class ProjectStore {
  public:
    ProjectStore(std::filesystem::path workspace_root, std::filesystem::path state_root = {});

    [[nodiscard]] const std::filesystem::path& workspace_root() const noexcept;
    [[nodiscard]] const std::filesystem::path& state_root() const noexcept;
    [[nodiscard]] const std::filesystem::path& project_directory() const noexcept;
    [[nodiscard]] std::filesystem::path profile_path() const;
    [[nodiscard]] std::filesystem::path project_policy_path() const;
    [[nodiscard]] bool initialized() const;
    [[nodiscard]] Json load_profile() const;

    void initialize(const std::string& project_kind, const Json& policy, bool force = false) const;
    [[nodiscard]] ManagedTaskPaths create_task(const std::string& question,
                                               ManagedTaskMode mode = ManagedTaskMode::model) const;
    [[nodiscard]] ManagedTaskPaths task_paths(const std::string& id) const;
    [[nodiscard]] std::vector<ManagedTaskSummary> list_tasks() const;
    [[nodiscard]] std::optional<ManagedTaskSummary> task_summary(const std::string& id) const;
    [[nodiscard]] std::optional<ManagedTaskPaths> latest_resumable_task() const;

  private:
    std::filesystem::path workspace_root_;
    std::filesystem::path state_root_;
    std::filesystem::path project_directory_;
};

} // namespace mint
