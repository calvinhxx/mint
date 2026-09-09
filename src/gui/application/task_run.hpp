#pragma once

#include "application/desktop_types.hpp"

#include <span>

namespace mint::desktop {

void append_retained_events(std::vector<RunEvent>& target, std::span<const RunEvent> events);

struct TaskRun {
    TaskRun(std::filesystem::path root, std::string task_id, std::string title,
            std::vector<RunEvent> history = {});

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool resumable() const noexcept;
    void ready(std::string id);
    void request_approval(ApprovalRequest request);
    void resolve_approval();
    // EN: The returned view contains retained new records and lasts until the next log mutation.
    // ZH-CN: 返回视图包含本批新保留的记录，在下一次日志修改前有效。
    [[nodiscard]] std::span<const RunEvent> append_events(std::vector<RunEvent> batch);
    void finish(TaskResult value);
    void fail(RuntimeFailure failure);

    std::filesystem::path root;
    std::string task_id;
    std::string title;
    std::string status = "running";
    std::vector<RunEvent> events;
    std::optional<ApprovalRequest> approval;
    std::optional<TaskResult> result;
    std::string error;

  private:
    bool running_ = true;
    std::size_t last_event_sequence_ = 0;
};

} // namespace mint::desktop
