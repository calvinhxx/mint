#pragma once

#include "application/agent_runtime.hpp"
#include "application/task_organization.hpp"
#include "application/task_run.hpp"
#include "application/workspaces/workspace_catalog.hpp"

#include <functional>
#include <string_view>
#include <unordered_map>

namespace mint::desktop {

struct WorkspaceOptions {
    std::filesystem::path root;
    std::filesystem::path config;
    std::string language;
    bool demo = false;
    TaskOrganizationRepository* task_organization_repository = nullptr;
    WorkspaceCatalogRepository* workspace_catalog_repository = nullptr;
    // EN: The resolved private state directory scopes startup restoration and workspace validation.
    // ZH-CN: 已解析的私有状态目录限定启动恢复范围，并用于验证工作区。
    std::filesystem::path state_root;
};

struct TaskWorkspaceState {
    std::filesystem::path root;
    std::filesystem::path project_directory;
    std::vector<std::filesystem::path> workspace_roots;
    std::vector<WorkspaceTaskCounts> workspace_task_counts;
    std::vector<TaskSummary> tasks;
    TaskOrganization task_organization;
    std::optional<std::size_t> selected_task;
    std::string task_id;
    std::string title;
    std::string status = "pending";
    std::vector<RunEvent> events;
    std::optional<ApprovalRequest> approval;
    std::optional<TaskResult> result;
    WorkspaceNotice notice = WorkspaceNotice::none;
    std::string error;
    // EN: Execution admission belongs to the runtime, independently of the selected conversation.
    // ZH-CN: 执行名额属于运行时，与当前浏览的会话独立。
    bool running = false;
    std::filesystem::path running_root;
    std::string running_task_id;
    std::string running_title;
    std::string running_status;
    bool selected_task_running = false;
    bool loading_tasks = false;
    bool loading_history = false;
    bool showing_archived_tasks = false;
    bool model_execution_available = true;

    [[nodiscard]] const TaskSummary* current_task() const noexcept;
    [[nodiscard]] bool can_resume() const noexcept;
    [[nodiscard]] bool viewing_running_task() const noexcept;
};

enum class WorkspaceChange {
    all,
    tasks,
    selection,
    content,
    event_appended,
    approval,
    run_state,
    error,
};

class TaskWorkspaceController final {
  public:
    using Listener = std::function<void(WorkspaceChange)>;

    TaskWorkspaceController(AgentRuntime& runtime, WorkspaceOptions options);
    ~TaskWorkspaceController();

    TaskWorkspaceController(const TaskWorkspaceController&) = delete;
    TaskWorkspaceController& operator=(const TaskWorkspaceController&) = delete;

    void set_listener(Listener listener);
    [[nodiscard]] const TaskWorkspaceState& state() const noexcept;

    void activate();
    [[nodiscard]] bool set_root(std::filesystem::path root);
    [[nodiscard]] bool remove_root(std::filesystem::path root);
    void set_config(std::filesystem::path config);
    void set_model_execution_available(bool available);
    void set_language(std::string language);
    void refresh();
    void create_task();
    bool select_task(std::size_t index);
    bool select_task(std::string_view task_id);
    bool toggle_task_pin(std::string_view task_id);
    bool archive_task(std::string_view task_id);
    bool unarchive_task(std::string_view task_id);
    bool show_archived_tasks(bool show);
    bool start(std::string question);
    bool resume();
    void resolve_approval(bool approved);
    void cancel();

  private:
    void bind_runtime();
    void notify(WorkspaceChange change) const;
    void clear_content();
    void clear_selection();
    void sync_run_state();
    void show_run();
    void update_run_summary();
    void sync_task_organization();
    void update_current_workspace_task_counts();
    void select_visible_task(std::string_view preferred_task_id = {});
    void accept_tasks(std::vector<TaskSummary> tasks, std::filesystem::path project_directory);
    void accept_history(TaskHistory history);
    void accept_events(std::vector<RunEvent> events);
    void accept_result(TaskResult result);
    void accept_error(RuntimeFailure failure);
    [[nodiscard]] std::optional<std::size_t> find_task(std::string_view id) const;
    [[nodiscard]] bool is_task_visible(const TaskSummary& task) const;
    void switch_to_root(std::filesystem::path root, TaskOrganizer organizer);

    struct SelectionBookmark {
        std::string task_id;
        bool new_task = false;
        bool viewing_run = false;
    };

    AgentRuntime& runtime_;
    WorkspaceOptions options_;
    WorkspaceCatalog workspace_catalog_;
    TaskOrganizer task_organizer_;
    TaskWorkspaceState state_;
    // EN: Retain only the current/latest run; historical conversations remain disk-backed.
    // ZH-CN: 仅保留当前或最近一次运行快照，历史会话仍按需从磁盘读取。
    std::optional<TaskRun> run_;
    std::unordered_map<std::string, SelectionBookmark> selections_;
    bool viewing_run_ = false;
    bool keep_empty_selection_ = false;
    Listener listener_;
};

} // namespace mint::desktop
