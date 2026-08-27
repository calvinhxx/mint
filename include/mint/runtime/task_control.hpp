#pragma once

#include <atomic>
#include <chrono>
#include <csignal>
#include <string>

namespace mint {

class TaskControl {
  public:
    explicit TaskControl(std::chrono::milliseconds total_budget = std::chrono::milliseconds::zero(),
                         const volatile std::sig_atomic_t* external_cancel_flag = nullptr);

    void request_cancel() noexcept;

    [[nodiscard]] bool cancellation_requested() const noexcept;
    [[nodiscard]] bool budget_exhausted() const noexcept;
    [[nodiscard]] bool should_stop() const noexcept;
    [[nodiscard]] std::string stop_reason() const;
    [[nodiscard]] long long elapsed_milliseconds() const noexcept;
    [[nodiscard]] long long remaining_milliseconds() const noexcept;

  private:
    std::chrono::steady_clock::time_point started_at_;
    std::chrono::milliseconds total_budget_;
    const volatile std::sig_atomic_t* external_cancel_flag_ = nullptr;
    std::atomic_bool cancel_requested_{false};
};

} // namespace mint
