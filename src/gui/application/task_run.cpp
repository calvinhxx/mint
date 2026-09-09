#include "task_run.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace mint::desktop {
namespace {

void make_event_room(std::vector<RunEvent>& events, std::size_t incoming) {
    const auto remaining = maximum_retained_run_events - incoming;
    if (events.size() > remaining) {
        events.erase(events.begin(), events.end() - static_cast<std::ptrdiff_t>(remaining));
    }
}

} // namespace

void append_retained_events(std::vector<RunEvent>& target, std::span<const RunEvent> events) {
    const auto retained = events.last(std::min(events.size(), maximum_retained_run_events));
    make_event_room(target, retained.size());
    target.insert(target.end(), retained.begin(), retained.end());
}

TaskRun::TaskRun(std::filesystem::path run_root, std::string id, std::string run_title,
                 std::vector<RunEvent> history)
    : root(std::move(run_root)), task_id(std::move(id)), title(std::move(run_title)),
      events(std::move(history)) {
    for (const auto& event : events) {
        last_event_sequence_ = std::max(last_event_sequence_, event.sequence);
    }
    make_event_room(events, 0);
}

bool TaskRun::running() const noexcept {
    return running_;
}

bool TaskRun::resumable() const noexcept {
    return !running_ && (status == "max_turns" || status == "cancelled" || status == "timed_out");
}

void TaskRun::ready(std::string id) {
    task_id = std::move(id);
    status = "running";
}

void TaskRun::request_approval(ApprovalRequest request) {
    approval = std::move(request);
    status = "waiting";
}

void TaskRun::resolve_approval() {
    approval.reset();
    error.clear();
    status = "running";
}

std::span<const RunEvent> TaskRun::append_events(std::vector<RunEvent> batch) {
    // EN: Resumed processes replay the existing log; only newer numbered records advance it.
    // ZH-CN: 恢复进程会回放旧日志，只有序号更大的记录才推进运行日志。
    std::erase_if(batch, [this](const RunEvent& event) {
        if (event.sequence == 0) {
            return false;
        }
        if (event.sequence <= last_event_sequence_) {
            return true;
        }
        last_event_sequence_ = event.sequence;
        return false;
    });
    const auto finished = std::find_if(batch.rbegin(), batch.rend(), [](const RunEvent& event) {
        return event.kind == RunEventKind::task_finished;
    });
    if (finished != batch.rend()) {
        status = finished->status;
    }
    const auto count = std::min(batch.size(), maximum_retained_run_events);
    make_event_room(events, count);
    const auto begin = events.size();
    events.insert(events.end(),
                  std::make_move_iterator(batch.end() - static_cast<std::ptrdiff_t>(count)),
                  std::make_move_iterator(batch.end()));
    return std::span<const RunEvent>(events).subspan(begin);
}

void TaskRun::finish(TaskResult value) {
    if (!value.status.empty()) {
        status = value.status;
    }
    result = std::move(value);
    approval.reset();
    error.clear();
    running_ = false;
}

void TaskRun::fail(RuntimeFailure failure) {
    error = std::move(failure.detail);
    if (failure.operation == RuntimeOperation::task_run) {
        status = "failed";
        approval.reset();
        running_ = false;
    }
}

} // namespace mint::desktop
