#pragma once

#include "application/desktop_types.hpp"

#include <functional>

namespace mint::desktop {

struct AgentRuntimeCallbacks {
    std::function<void(std::vector<TaskSummary>, std::filesystem::path)> tasks_loaded;
    std::function<void(std::string, TaskHistory)> task_loaded;
    std::function<void(std::string)> task_ready;
    std::function<void(RunEvent)> event_received;
    std::function<void(std::vector<RunEvent>)> events_received;
    std::function<void(ApprovalRequest)> approval_requested;
    std::function<void(TaskResult)> run_finished;
    std::function<void(RuntimeFailure)> error;
};

class AgentRuntime {
  public:
    virtual ~AgentRuntime() = default;

    virtual void set_callbacks(AgentRuntimeCallbacks callbacks) = 0;
    // EN: Browsing queries remain available during a run and publish only their latest selection.
    // ZH-CN: 浏览查询在运行期间仍可用，且只发布最新选择对应的结果。
    virtual void refresh_tasks(const std::filesystem::path& root, const std::string& language) = 0;
    virtual void load_task(const std::string& task_id) = 0;
    virtual void start_task(RunRequest request) = 0;
    virtual void resume_task(ResumeRequest request) = 0;
    [[nodiscard]] virtual bool resolve_approval(bool approved) = 0;
    virtual void cancel() = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
};

} // namespace mint::desktop
