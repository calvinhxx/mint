#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "aiagent/domain/model.hpp"
#include "aiagent/domain/task_policy.hpp"

namespace aiagent {

class TaskControl;

struct CommandApprovalRequest {
    std::string program;
    std::vector<std::string> args;
    std::string cwd;
    long timeout_seconds = 0;
};

using CommandApproval = std::function<bool(const CommandApprovalRequest&)>;

struct CommandRunnerOptions {
    std::filesystem::path root;
    std::vector<std::string> allowed_programs;
    std::vector<CommandRecipe> recipes;
    long default_timeout_seconds = 60;
    long max_timeout_seconds = 120;
    std::size_t max_output_bytes = 128 * 1024;
    std::shared_ptr<TaskControl> task_control;
    CommandApproval approval;
    bool require_os_sandbox = false;
    std::vector<std::filesystem::path> denied_read_paths;
};

class CommandRunner {
  public:
    explicit CommandRunner(CommandRunnerOptions options);

    [[nodiscard]] const std::vector<std::string>& allowed_programs() const noexcept;
    [[nodiscard]] const std::vector<std::string>& recipe_names() const noexcept;
    [[nodiscard]] bool uses_recipes() const noexcept;
    [[nodiscard]] bool requires_approval() const noexcept;
    [[nodiscard]] bool is_os_sandboxed() const noexcept;
    [[nodiscard]] const std::string& sandbox_backend() const noexcept;
    [[nodiscard]] Json definition() const;
    [[nodiscard]] Json run(const Json& arguments) const;

  private:
    [[nodiscard]] Json run_command(const Json& arguments) const;

    std::filesystem::path root_;
    std::vector<std::string> allowed_programs_;
    std::unordered_map<std::string, std::filesystem::path> resolved_programs_;
    std::vector<CommandRecipe> recipes_;
    std::unordered_map<std::string, std::size_t> recipe_indices_;
    std::vector<std::string> recipe_names_;
    long default_timeout_seconds_ = 60;
    long max_timeout_seconds_ = 120;
    std::size_t max_output_bytes_ = 128 * 1024;
    std::shared_ptr<TaskControl> task_control_;
    CommandApproval approval_;
    std::filesystem::path sandbox_executable_;
    std::string sandbox_profile_;
    std::string sandbox_backend_ = "none";
};

} // namespace aiagent
