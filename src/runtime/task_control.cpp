#include "aiagent/runtime/task_control.hpp"

#include <algorithm>

namespace aiagent {

TaskControl::TaskControl(std::chrono::milliseconds total_budget,
                         const volatile std::sig_atomic_t* external_cancel_flag)
    : started_at_(std::chrono::steady_clock::now()),
      total_budget_(std::max(total_budget, std::chrono::milliseconds::zero())),
      external_cancel_flag_(external_cancel_flag) {}

void TaskControl::request_cancel() noexcept {
    cancel_requested_.store(true, std::memory_order_relaxed);
}

bool TaskControl::cancellation_requested() const noexcept {
    return cancel_requested_.load(std::memory_order_relaxed) ||
           (external_cancel_flag_ != nullptr && *external_cancel_flag_ != 0);
}

bool TaskControl::budget_exhausted() const noexcept {
    return total_budget_ > std::chrono::milliseconds::zero() &&
           std::chrono::steady_clock::now() - started_at_ >= total_budget_;
}

bool TaskControl::should_stop() const noexcept {
    return cancellation_requested() || budget_exhausted();
}

std::string TaskControl::stop_reason() const {
    if (cancellation_requested()) {
        return "cancelled";
    }
    if (budget_exhausted()) {
        return "timed_out";
    }
    return {};
}

long long TaskControl::elapsed_milliseconds() const noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 started_at_)
        .count();
}

long long TaskControl::remaining_milliseconds() const noexcept {
    if (total_budget_ == std::chrono::milliseconds::zero()) {
        return -1;
    }
    return std::max<long long>(0, total_budget_.count() - elapsed_milliseconds());
}

} // namespace aiagent
