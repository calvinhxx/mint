#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mint::desktop {

inline constexpr std::size_t maximum_retained_run_events = 5'000;

struct UsageSummary {
    std::size_t calls = 0;
    std::size_t total_tokens = 0;
    std::size_t prompt_tokens = 0;
    std::size_t cached_tokens = 0;
    std::size_t usage_reports = 0;
    std::size_t cache_usage_reports = 0;

    [[nodiscard]] std::optional<std::size_t> cache_hit_percent() const noexcept {
        if (prompt_tokens == 0 || calls == 0 || usage_reports != calls ||
            cache_usage_reports != usage_reports) {
            return std::nullopt;
        }
        const auto rate = static_cast<long double>(cached_tokens) * 100.0L /
                          static_cast<long double>(prompt_tokens);
        return static_cast<std::size_t>(rate > 100.0L ? 100.0L : rate);
    }
};

struct TaskSummary {
    std::string id;
    std::string question;
    std::string status;
    std::string created_at;
    std::string verification_status;
    bool completed = false;
    bool resumable = false;
};

struct WorkspaceTaskCounts {
    std::filesystem::path root;
    std::size_t unarchived = 0;
    std::size_t archived = 0;

    bool operator==(const WorkspaceTaskCounts&) const = default;
};

enum class RunEventKind {
    task_started,
    model_requested,
    model_completed,
    model_progress,
    tool_started,
    tool_completed,
    approval_requested,
    approval_resolved,
    verification_ready,
    verification_blocked,
    context_compacted,
    task_finished,
    unknown,
};

struct RunEvent {
    std::size_t sequence = 0;
    std::string timestamp;
    RunEventKind kind = RunEventKind::unknown;
    std::string raw_type;
    std::string name;
    std::string target;
    std::string status;
    std::string verification_status;
    std::string evidence;
    std::optional<std::size_t> turn;
    std::size_t affected_paths = 0;
    UsageSummary usage;
};

enum class ApprovalKind { command, changeset };

struct ApprovalRequest {
    ApprovalKind kind = ApprovalKind::command;
    std::vector<std::string> paths;
    std::string program;
    std::string working_directory;
    std::vector<std::string> arguments;
    std::string unified_diff;
    bool diff_truncated = false;
};

struct TaskResult {
    std::string status;
    std::string answer;
    std::string unified_diff;
    UsageSummary usage;
};

struct TaskHistory {
    std::vector<RunEvent> events;
    std::optional<TaskResult> result;
    std::size_t ignored_event_records = 0;
    bool invalid_snapshot = false;
};

enum class WorkspaceNotice { none, incomplete_history, invalid_workspace };

struct RunRequest {
    std::filesystem::path root;
    std::filesystem::path config;
    std::string question;
    std::string language;
    bool demo = false;
};

struct ResumeRequest {
    std::filesystem::path root;
    std::filesystem::path config;
    std::string task_id;
    std::string language;
};

enum class RuntimeOperation { task_history, task_run, approval };

struct RuntimeFailure {
    RuntimeOperation operation = RuntimeOperation::task_run;
    std::string detail;
};

} // namespace mint::desktop
