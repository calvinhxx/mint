#pragma once

#if defined(_WIN32)
#error "command_process_tree.hpp is only available on POSIX platforms"
#endif

#include <cstddef>
#include <memory>

#include <sys/types.h>

namespace mint::command_detail {

class ProcessTreeMonitor final {
  public:
    explicit ProcessTreeMonitor(pid_t root);
    ~ProcessTreeMonitor();

    ProcessTreeMonitor(const ProcessTreeMonitor&) = delete;
    ProcessTreeMonitor& operator=(const ProcessTreeMonitor&) = delete;

    [[nodiscard]] std::size_t refresh();
    void signal_all(int signal);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace mint::command_detail
