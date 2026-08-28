#pragma once

#include "mint/domain/runtime_settings.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mint {

class TaskControl;

namespace command_detail {

struct ProcessRequest {
    std::filesystem::path executable;
    std::vector<std::string> argv;
    std::filesystem::path cwd;
    long timeout_seconds = 0;
    std::size_t max_output_bytes = 0;
    CommandResourceLimits resource_limits{};
    std::shared_ptr<TaskControl> task_control;
};

struct ProcessResult {
    long long duration_ms = 0;
    std::string status;
    std::optional<int> exit_code;
    std::optional<int> signal;
    bool timed_out = false;
    bool task_timed_out = false;
    bool cancelled = false;
    bool resource_limited = false;
    std::string resource_limit;
    bool output_truncated = false;
    std::string output;
};

void validate_process_resource_support(const CommandResourceLimits& limits);
[[nodiscard]] ProcessResult execute_process(ProcessRequest request);

} // namespace command_detail
} // namespace mint
