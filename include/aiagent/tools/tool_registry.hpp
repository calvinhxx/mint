#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aiagent/domain/model.hpp"
#include "aiagent/infrastructure/command_runner.hpp"

namespace aiagent {

class ChangeJournal;
class TaskControl;

struct ChangeSetApprovalRequest {
    std::vector<std::string> paths;
    std::string unified_diff;
    bool diff_truncated = false;
};

using ChangeSetApproval = std::function<bool(const ChangeSetApprovalRequest&)>;

struct ToolRegistryOptions {
    std::vector<std::filesystem::path> protected_paths;
    bool allow_write = false;
    std::vector<std::filesystem::path> allowed_write_paths;
    std::vector<std::string> allowed_programs;
    std::vector<CommandRecipe> command_recipes;
    std::string policy_fingerprint;
    long default_command_timeout_seconds = 60;
    long max_command_timeout_seconds = 120;
    std::size_t max_command_output_bytes = 128 * 1024;
    std::shared_ptr<TaskControl> task_control;
    CommandApproval command_approval;
    ChangeSetApproval change_set_approval;
    bool require_command_sandbox = false;
};

class ToolRegistry {
  public:
    explicit ToolRegistry(std::filesystem::path root, ToolRegistryOptions options = {});
    ~ToolRegistry();

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] bool can_write() const noexcept;
    [[nodiscard]] const std::vector<std::string>& allowed_write_paths() const noexcept;
    [[nodiscard]] bool can_run_commands() const noexcept;
    [[nodiscard]] bool requires_command_approval() const noexcept;
    [[nodiscard]] bool requires_change_set_approval() const noexcept;
    [[nodiscard]] bool commands_are_os_sandboxed() const noexcept;
    [[nodiscard]] const std::string& command_sandbox_backend() const noexcept;
    [[nodiscard]] const std::vector<std::string>& allowed_programs() const noexcept;
    [[nodiscard]] const std::vector<std::string>& command_recipe_names() const noexcept;
    [[nodiscard]] bool uses_command_recipes() const noexcept;
    [[nodiscard]] const std::string& policy_fingerprint() const noexcept;
    [[nodiscard]] bool has_workspace_changes() const;
    [[nodiscard]] Json workspace_change_snapshot() const;
    [[nodiscard]] Json workspace_change_state() const;
    void restore_workspace_change_state(const Json& state);
    [[nodiscard]] Json definitions() const;
    [[nodiscard]] std::string describe_call(const ToolCall& call) const;
    [[nodiscard]] std::string execute(const ToolCall& call) const;

  private:
    [[nodiscard]] std::filesystem::path resolve_inside_root(const std::string& requested) const;
    [[nodiscard]] bool is_protected(const std::filesystem::path& path) const;
    [[nodiscard]] bool is_write_allowed(const std::filesystem::path& path) const;
    [[nodiscard]] Json list_files(const Json& arguments) const;
    [[nodiscard]] Json read_file(const Json& arguments) const;
    [[nodiscard]] Json search_text(const Json& arguments) const;
    [[nodiscard]] Json apply_patch(const Json& arguments) const;
    [[nodiscard]] Json apply_changeset(const Json& arguments) const;

    std::filesystem::path root_;
    std::vector<std::filesystem::path> protected_paths_;
    bool allow_write_ = false;
    std::vector<std::filesystem::path> allowed_write_paths_;
    std::vector<bool> recursive_write_paths_;
    std::vector<std::string> write_path_labels_;
    std::unique_ptr<ChangeJournal> change_journal_;
    std::unique_ptr<CommandRunner> command_runner_;
    ChangeSetApproval change_set_approval_;
    std::string policy_fingerprint_;
};

} // namespace aiagent
