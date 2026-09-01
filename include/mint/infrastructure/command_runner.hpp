#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mint/domain/model.hpp"
#include "mint/domain/task_policy.hpp"
#include "mint/runtime/command_approval.hpp"

namespace mint {

class TaskControl;

struct CommandRunnerOptions {
    std::filesystem::path root{};
    std::vector<std::string> allowed_programs{};
    std::vector<CommandRecipe> recipes{};
    long default_timeout_seconds = runtime_defaults::command_timeout_seconds;
    long max_timeout_seconds = runtime_defaults::max_command_timeout_seconds;
    std::size_t max_output_bytes = runtime_defaults::command_output_bytes;
    CommandResourceLimits resource_limits{};
    std::shared_ptr<TaskControl> task_control{};
    CommandApproval approval{};
    bool require_os_sandbox = false;
    std::vector<std::filesystem::path> read_only_paths{};
    std::vector<std::filesystem::path> denied_read_paths{};
};

class CommandRunner {
  public:
    explicit CommandRunner(CommandRunnerOptions options);
    ~CommandRunner();

    CommandRunner(const CommandRunner&) = delete;
    CommandRunner& operator=(const CommandRunner&) = delete;

    [[nodiscard]] const std::vector<std::string>& allowed_programs() const noexcept;
    [[nodiscard]] const std::vector<std::string>& recipe_names() const noexcept;
    [[nodiscard]] bool uses_recipes() const noexcept;
    [[nodiscard]] bool requires_approval() const noexcept;
    [[nodiscard]] bool is_os_sandboxed() const noexcept;
    [[nodiscard]] const std::string& sandbox_backend() const noexcept;
    [[nodiscard]] Json definition() const;
    [[nodiscard]] Json run(const Json& arguments) const;

  private:
    struct NativeSandboxState;

    [[nodiscard]] Json run_command(const Json& arguments) const;

    std::filesystem::path root_;
    std::vector<std::string> allowed_programs_;
    std::unordered_map<std::string, std::filesystem::path> resolved_programs_;
    std::vector<CommandRecipe> recipes_;
    std::unordered_map<std::string, std::size_t> recipe_indices_;
    std::vector<std::string> recipe_names_;
    long default_timeout_seconds_ = runtime_defaults::command_timeout_seconds;
    long max_timeout_seconds_ = runtime_defaults::max_command_timeout_seconds;
    std::size_t max_output_bytes_ = runtime_defaults::command_output_bytes;
    CommandResourceLimits resource_limits_{};
    std::shared_ptr<TaskControl> task_control_;
    CommandApproval approval_;
    std::filesystem::path sandbox_executable_;
    std::vector<std::string> sandbox_arguments_;
    std::string sandbox_backend_ = "none";
    bool sandbox_sets_working_directory_ = false;
    bool sandbox_wraps_command_ = false;
    std::unique_ptr<NativeSandboxState> native_sandbox_;
};

} // namespace mint
