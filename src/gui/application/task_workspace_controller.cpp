#include "task_workspace_controller.hpp"

#include <algorithm>
#include <utility>

namespace mint::desktop {
namespace {

void preserve_organization_aliases(const WorkspaceOptions& options) {
    if (options.task_organization_repository == nullptr ||
        options.workspace_catalog_repository == nullptr) {
        return;
    }
    const auto catalog = options.workspace_catalog_repository->load();
    if (!catalog.has_value()) {
        return;
    }
    for (const auto& root : catalog->roots) {
        if (!options.state_root.empty() &&
            (!root.is_absolute() || !is_usable_workspace_root(root, options.state_root))) {
            continue;
        }
        const auto previous_scope = root.lexically_normal().generic_string();
        const auto canonical_scope = normalize_workspace_root(root).generic_string();
        if (previous_scope.empty() || previous_scope == canonical_scope) {
            continue;
        }
        const auto previous = options.task_organization_repository->load(previous_scope);
        auto canonical = options.task_organization_repository->load(canonical_scope);
        if (!previous.has_value() || !canonical.has_value()) {
            continue;
        }
        canonical->pinned_task_ids.insert(previous->pinned_task_ids.begin(),
                                          previous->pinned_task_ids.end());
        canonical->archived_task_ids.insert(previous->archived_task_ids.begin(),
                                            previous->archived_task_ids.end());
        for (const auto& id : canonical->archived_task_ids) {
            canonical->pinned_task_ids.erase(id);
        }
        (void)options.task_organization_repository->save(canonical_scope, *canonical);
    }
}

} // namespace

const TaskSummary* TaskWorkspaceState::current_task() const noexcept {
    if (!selected_task.has_value() || *selected_task >= tasks.size()) {
        return nullptr;
    }
    return &tasks[*selected_task];
}

bool TaskWorkspaceState::can_resume() const noexcept {
    const auto* task = current_task();
    return model_execution_available && !running && task != nullptr && task->resumable;
}

bool TaskWorkspaceState::viewing_running_task() const noexcept {
    return running && selected_task_running;
}

TaskWorkspaceController::TaskWorkspaceController(AgentRuntime& runtime, WorkspaceOptions options)
    : runtime_(runtime), options_(std::move(options)),
      workspace_catalog_(options_.workspace_catalog_repository),
      task_organizer_(options_.task_organization_repository) {
    preserve_organization_aliases(options_);
    state_.root = normalize_workspace_root(options_.root);
    if (!options_.state_root.empty()) {
        const auto stored = options_.workspace_catalog_repository != nullptr
                                ? options_.workspace_catalog_repository->load()
                                : std::optional<WorkspaceCatalogState>{};
        state_.root = select_startup_workspace(
            options_.root, stored.value_or(WorkspaceCatalogState{}), options_.state_root);
        if (!options_.root.empty() && state_.root.empty()) {
            state_.notice = WorkspaceNotice::invalid_workspace;
        }
    }
    options_.root = state_.root;
    (void)workspace_catalog_.initialize(state_.root, options_.state_root);
    state_.workspace_roots = workspace_catalog_.state().roots;
    state_.workspace_task_counts = workspace_catalog_.state().task_counts;
    if (!state_.root.empty()) {
        (void)task_organizer_.open_project(state_.root);
    }
    sync_task_organization();
    bind_runtime();
}

TaskWorkspaceController::~TaskWorkspaceController() {
    runtime_.set_callbacks({});
}

void TaskWorkspaceController::set_listener(Listener listener) {
    listener_ = std::move(listener);
}

const TaskWorkspaceState& TaskWorkspaceController::state() const noexcept {
    return state_;
}

void TaskWorkspaceController::activate() {
    refresh();
}

bool TaskWorkspaceController::set_root(std::filesystem::path root) {
    root = normalize_workspace_root(root);
    if (root.empty()) {
        return false;
    }
    if (!options_.state_root.empty() && !is_usable_workspace_root(root, options_.state_root)) {
        state_.notice = WorkspaceNotice::invalid_workspace;
        notify(WorkspaceChange::error);
        return false;
    }
    if (root == state_.root) {
        if (state_.notice == WorkspaceNotice::invalid_workspace) {
            state_.notice = WorkspaceNotice::none;
            notify(WorkspaceChange::error);
        }
        return true;
    }
    auto next_organizer = task_organizer_;
    if (!next_organizer.open_project(root)) {
        return false;
    }
    if (!workspace_catalog_.add(root)) {
        return false;
    }
    switch_to_root(std::move(root), std::move(next_organizer));
    return true;
}

bool TaskWorkspaceController::remove_root(std::filesystem::path root) {
    root = normalize_workspace_root(root);
    const auto& roots = workspace_catalog_.state().roots;
    const auto found = std::find(roots.begin(), roots.end(), root);
    if ((state_.running && root == state_.running_root) || root.empty() || found == roots.end() ||
        roots.size() <= 1) {
        return false;
    }

    if (root != state_.root) {
        if (!workspace_catalog_.remove(root)) {
            return false;
        }
        state_.workspace_roots = workspace_catalog_.state().roots;
        state_.workspace_task_counts = workspace_catalog_.state().task_counts;
        selections_.erase(root.generic_string());
        notify(WorkspaceChange::tasks);
        return true;
    }

    const auto position = static_cast<std::size_t>(std::distance(roots.begin(), found));
    const auto fallback_position = position + 1 < roots.size() ? position + 1 : position - 1;
    auto fallback = roots[fallback_position];
    auto next_organizer = task_organizer_;
    if (!next_organizer.open_project(fallback) || !workspace_catalog_.remove(root)) {
        return false;
    }
    switch_to_root(std::move(fallback), std::move(next_organizer));
    selections_.erase(root.generic_string());
    return true;
}

void TaskWorkspaceController::set_config(std::filesystem::path config) {
    // EN: Running requests own their startup snapshot; this selects only the next run.
    // ZH-CN: 运行中的请求保留启动时快照，此处仅影响下一次运行。
    options_.config = std::move(config);
}

void TaskWorkspaceController::set_model_execution_available(bool available) {
    const auto allowed = options_.demo || available;
    if (state_.model_execution_available == allowed) {
        return;
    }
    // EN: Model selection gates only new execution; active requests keep their startup snapshot.
    // ZH-CN: 模型选择仅限制新执行，现有请求保留启动快照。
    state_.model_execution_available = allowed;
    notify(WorkspaceChange::run_state);
}

void TaskWorkspaceController::set_language(std::string language) {
    options_.language = std::move(language);
}

void TaskWorkspaceController::refresh() {
    if (state_.root.empty()) {
        return;
    }
    state_.loading_tasks = true;
    state_.loading_history = false;
    notify(WorkspaceChange::tasks);
    runtime_.refresh_tasks(state_.root, options_.language);
}

void TaskWorkspaceController::create_task() {
    if (state_.root.empty()) {
        return;
    }
    if (state_.showing_archived_tasks) {
        state_.showing_archived_tasks = false;
        notify(WorkspaceChange::tasks);
    }
    state_.loading_history = false;
    keep_empty_selection_ = true;
    clear_selection();
}

bool TaskWorkspaceController::select_task(std::size_t index) {
    if (index >= state_.tasks.size() || !is_task_visible(state_.tasks[index])) {
        return false;
    }
    const auto& task = state_.tasks[index];
    keep_empty_selection_ = false;
    if (run_.has_value() && run_->root == state_.root && run_->task_id == task.id) {
        const auto was_selected = viewing_run_ && state_.task_id == task.id;
        show_run();
        if (!was_selected) {
            notify(WorkspaceChange::selection);
        }
        return true;
    }
    const auto next_status = task.resumable && task.status == "running" ? "resumable" : task.status;
    if (state_.selected_task == index && state_.task_id == task.id) {
        const auto presentation_changed =
            state_.title != task.question || state_.status != next_status;
        state_.title = task.question;
        state_.status = next_status;
        if (presentation_changed) {
            notify(WorkspaceChange::selection);
        }
        return true;
    }
    state_.selected_task = index;
    viewing_run_ = false;
    sync_run_state();
    state_.task_id = task.id;
    state_.title = task.question;
    state_.status = next_status;
    state_.loading_history = true;
    clear_content();
    notify(WorkspaceChange::selection);
    runtime_.load_task(task.id);
    return true;
}

bool TaskWorkspaceController::select_task(std::string_view task_id) {
    const auto index = find_task(task_id);
    return index.has_value() && select_task(*index);
}

bool TaskWorkspaceController::toggle_task_pin(std::string_view task_id) {
    if (!find_task(task_id).has_value() || !task_organizer_.toggle_pin(task_id)) {
        return false;
    }
    sync_task_organization();
    notify(WorkspaceChange::tasks);
    return true;
}

bool TaskWorkspaceController::archive_task(std::string_view task_id) {
    if ((state_.running && state_.root == state_.running_root &&
         task_id == state_.running_task_id) ||
        !find_task(task_id).has_value() || !task_organizer_.archive(task_id)) {
        return false;
    }
    const auto archived_current_task = std::string_view(state_.task_id) == task_id;
    sync_task_organization();
    update_current_workspace_task_counts();
    notify(WorkspaceChange::tasks);
    if (archived_current_task && !state_.showing_archived_tasks) {
        select_visible_task();
    }
    return true;
}

bool TaskWorkspaceController::unarchive_task(std::string_view task_id) {
    if (!find_task(task_id).has_value() || !task_organizer_.unarchive(task_id)) {
        return false;
    }
    const auto unarchived_current_task = std::string_view(state_.task_id) == task_id;
    sync_task_organization();
    update_current_workspace_task_counts();
    notify(WorkspaceChange::tasks);
    if (unarchived_current_task && state_.showing_archived_tasks) {
        select_visible_task();
    }
    return true;
}

bool TaskWorkspaceController::show_archived_tasks(bool show) {
    if (state_.showing_archived_tasks == show) {
        return true;
    }
    state_.showing_archived_tasks = show;
    notify(WorkspaceChange::tasks);
    select_visible_task(state_.task_id);
    return true;
}

bool TaskWorkspaceController::start(std::string question) {
    if (state_.root.empty() || state_.running || !state_.model_execution_available ||
        question.empty()) {
        return false;
    }
    RunRequest request{.root = state_.root,
                       .config = options_.config,
                       .question = std::move(question),
                       .language = options_.language,
                       .demo = options_.demo};
    const auto was_showing_archived_tasks = std::exchange(state_.showing_archived_tasks, false);
    state_.loading_tasks = false;
    run_.emplace(request.root, std::string{}, request.question);
    show_run();
    if (was_showing_archived_tasks) {
        notify(WorkspaceChange::tasks);
    }
    notify(WorkspaceChange::selection);
    runtime_.start_task(std::move(request));
    return true;
}

bool TaskWorkspaceController::resume() {
    if (!state_.can_resume()) {
        return false;
    }
    ResumeRequest request{.root = state_.root,
                          .config = options_.config,
                          .task_id = state_.task_id,
                          .language = options_.language};
    run_.emplace(request.root, request.task_id, state_.title, std::move(state_.events));
    show_run();
    update_run_summary();
    notify(WorkspaceChange::run_state);
    runtime_.resume_task(std::move(request));
    return true;
}

void TaskWorkspaceController::resolve_approval(bool approved) {
    if (!state_.viewing_running_task() || !state_.approval.has_value()) {
        return;
    }
    if (!runtime_.resolve_approval(approved)) {
        return;
    }
    run_->resolve_approval();
    state_.approval = run_->approval;
    state_.status = run_->status;
    state_.error = run_->error;
    sync_run_state();
    update_run_summary();
    notify(WorkspaceChange::approval);
}

void TaskWorkspaceController::cancel() {
    if (state_.running) {
        runtime_.cancel();
    }
}

void TaskWorkspaceController::bind_runtime() {
    runtime_.set_callbacks(
        {.tasks_loaded =
             [this](std::vector<TaskSummary> tasks, std::filesystem::path directory) {
                 accept_tasks(std::move(tasks), std::move(directory));
             },
         .task_loaded =
             [this](std::string task_id, TaskHistory history) {
                 if (!viewing_run_ && task_id == state_.task_id) {
                     accept_history(std::move(history));
                 }
             },
         .task_ready =
             [this](std::string id) {
                 if (!state_.running || !run_.has_value()) {
                     return;
                 }
                 run_->ready(std::move(id));
                 update_run_summary();
                 if (viewing_run_) {
                     state_.task_id = run_->task_id;
                     state_.selected_task = find_task(state_.task_id);
                     state_.status = run_->status;
                 }
                 sync_run_state();
                 notify(WorkspaceChange::run_state);
                 if (run_->root == state_.root && state_.project_directory.empty()) {
                     refresh();
                 }
             },
         .event_received =
             [this](RunEvent event) {
                 std::vector<RunEvent> events;
                 events.push_back(std::move(event));
                 accept_events(std::move(events));
             },
         .events_received =
             [this](std::vector<RunEvent> events) { accept_events(std::move(events)); },
         .approval_requested =
             [this](ApprovalRequest request) {
                 if (!state_.running || !run_.has_value()) {
                     return;
                 }
                 run_->request_approval(std::move(request));
                 sync_run_state();
                 update_run_summary();
                 if (viewing_run_) {
                     state_.approval = run_->approval;
                     state_.status = run_->status;
                     notify(WorkspaceChange::approval);
                 } else {
                     notify(WorkspaceChange::run_state);
                 }
             },
         .run_finished = [this](TaskResult result) { accept_result(std::move(result)); },
         .error = [this](RuntimeFailure failure) { accept_error(std::move(failure)); }});
}

void TaskWorkspaceController::notify(WorkspaceChange change) const {
    if (listener_) {
        listener_(change);
    }
}

void TaskWorkspaceController::clear_content() {
    state_.events.clear();
    state_.approval.reset();
    state_.result.reset();
    state_.notice = WorkspaceNotice::none;
    state_.error.clear();
}

void TaskWorkspaceController::clear_selection() {
    viewing_run_ = false;
    sync_run_state();
    state_.selected_task.reset();
    state_.task_id.clear();
    state_.title.clear();
    state_.status = "pending";
    state_.loading_history = false;
    clear_content();
    notify(WorkspaceChange::selection);
}

void TaskWorkspaceController::sync_run_state() {
    state_.running = run_.has_value() && run_->running();
    state_.running_root = state_.running ? run_->root : std::filesystem::path{};
    state_.running_task_id = state_.running ? run_->task_id : std::string{};
    state_.running_title = state_.running ? run_->title : std::string{};
    state_.running_status = state_.running ? run_->status : std::string{};
    state_.selected_task_running = state_.running && viewing_run_;
}

void TaskWorkspaceController::show_run() {
    if (!run_.has_value() || run_->root != state_.root) {
        return;
    }
    viewing_run_ = true;
    keep_empty_selection_ = false;
    state_.task_id = run_->task_id;
    state_.selected_task = find_task(state_.task_id);
    state_.title = run_->title;
    state_.status = run_->status;
    state_.events = run_->events;
    state_.approval = run_->approval;
    state_.result = run_->result;
    state_.error = run_->error;
    state_.notice = WorkspaceNotice::none;
    state_.loading_history = false;
    sync_run_state();
}

void TaskWorkspaceController::update_run_summary() {
    if (!run_.has_value() || run_->task_id.empty() || run_->root != state_.root) {
        return;
    }
    auto index = find_task(run_->task_id);
    if (!index.has_value()) {
        state_.tasks.insert(state_.tasks.begin(), TaskSummary{.id = run_->task_id});
        index = 0;
        state_.selected_task = find_task(state_.task_id);
    }
    auto& task = state_.tasks[*index];
    task.question = run_->title;
    task.status = run_->status;
    task.completed = run_->status == "completed";
    task.resumable = run_->resumable();
}

void TaskWorkspaceController::sync_task_organization() {
    state_.task_organization = task_organizer_.state();
}

void TaskWorkspaceController::update_current_workspace_task_counts() {
    const auto normalized_active = state_.root.lexically_normal();
    std::size_t unarchived = 0;
    std::size_t archived = 0;
    for (const auto& task : state_.tasks) {
        if (state_.task_organization.archived_task_ids.contains(task.id)) {
            ++archived;
        } else {
            ++unarchived;
        }
    }
    if (workspace_catalog_.set_task_counts(normalized_active, unarchived, archived)) {
        state_.workspace_task_counts = workspace_catalog_.state().task_counts;
        return;
    }

    const auto active =
        std::find_if(state_.workspace_task_counts.begin(), state_.workspace_task_counts.end(),
                     [&](const auto& counts) { return counts.root == normalized_active; });
    if (active == state_.workspace_task_counts.end()) {
        state_.workspace_task_counts.push_back(
            {.root = normalized_active, .unarchived = unarchived, .archived = archived});
    } else {
        active->unarchived = unarchived;
        active->archived = archived;
    }
}

void TaskWorkspaceController::select_visible_task(std::string_view preferred_task_id) {
    if (!preferred_task_id.empty()) {
        const auto preferred = find_task(preferred_task_id);
        if (preferred.has_value() && is_task_visible(state_.tasks[*preferred])) {
            (void)select_task(*preferred);
            return;
        }
    }
    const auto visible = std::find_if(state_.tasks.begin(), state_.tasks.end(),
                                      [this](const auto& task) { return is_task_visible(task); });
    if (visible == state_.tasks.end()) {
        clear_selection();
        return;
    }
    (void)select_task(static_cast<std::size_t>(std::distance(state_.tasks.begin(), visible)));
}

void TaskWorkspaceController::accept_tasks(std::vector<TaskSummary> tasks,
                                           std::filesystem::path project_directory) {
    const auto selected_id = state_.task_id;
    const auto had_selection = state_.selected_task.has_value() || viewing_run_;
    state_.loading_tasks = false;
    state_.tasks = std::move(tasks);
    update_run_summary();
    state_.selected_task = had_selection ? find_task(selected_id) : std::nullopt;
    state_.project_directory = std::move(project_directory);
    (void)task_organizer_.reconcile(state_.tasks);
    sync_task_organization();
    update_current_workspace_task_counts();
    notify(WorkspaceChange::tasks);
    if (viewing_run_) {
        return;
    }
    if (keep_empty_selection_) {
        return;
    }
    select_visible_task(selected_id);
}

void TaskWorkspaceController::accept_history(TaskHistory history) {
    state_.loading_history = false;
    state_.events = std::move(history.events);
    state_.result = std::move(history.result);
    if (state_.result.has_value() && !state_.result->status.empty()) {
        state_.status = state_.result->status;
    }
    if (history.ignored_event_records > 0 || history.invalid_snapshot) {
        state_.notice = WorkspaceNotice::incomplete_history;
    }
    notify(WorkspaceChange::content);
}

void TaskWorkspaceController::accept_events(std::vector<RunEvent> events) {
    if (events.empty() || !state_.running || !run_.has_value()) {
        return;
    }
    const auto appended = run_->append_events(std::move(events));
    if (appended.empty()) {
        return;
    }
    if (state_.running_status != run_->status) {
        sync_run_state();
    }
    if (!viewing_run_) {
        return;
    }
    state_.status = run_->status;
    append_retained_events(state_.events, appended);
    notify(WorkspaceChange::event_appended);
}

void TaskWorkspaceController::accept_result(TaskResult result) {
    if (!state_.running || !run_.has_value()) {
        return;
    }
    run_->finish(std::move(result));
    sync_run_state();
    update_run_summary();
    if (viewing_run_) {
        show_run();
        notify(WorkspaceChange::content);
    } else {
        // EN: A background completion changes run controls, never the reader's selection/content.
        // ZH-CN: 后台完成只更新运行控件，不改变用户正在浏览的会话和内容。
        notify(WorkspaceChange::run_state);
    }
    if (run_->root == state_.root) {
        update_current_workspace_task_counts();
        notify(WorkspaceChange::tasks);
        if (!state_.loading_tasks && !state_.loading_history) {
            refresh();
        }
    }
}

void TaskWorkspaceController::accept_error(RuntimeFailure failure) {
    if (failure.operation == RuntimeOperation::task_history) {
        state_.loading_tasks = false;
        state_.loading_history = false;
        if (!viewing_run_) {
            state_.error = std::move(failure.detail);
            notify(WorkspaceChange::error);
        }
        return;
    }
    if (!state_.running || !run_.has_value()) {
        return;
    }
    const auto operation = failure.operation;
    run_->fail(std::move(failure));
    if (operation == RuntimeOperation::task_run) {
        sync_run_state();
        update_run_summary();
    }
    if (viewing_run_) {
        show_run();
        notify(WorkspaceChange::error);
    } else if (operation == RuntimeOperation::task_run) {
        notify(WorkspaceChange::run_state);
    }
}

std::optional<std::size_t> TaskWorkspaceController::find_task(std::string_view id) const {
    if (id.empty()) {
        return std::nullopt;
    }
    const auto found = std::find_if(state_.tasks.begin(), state_.tasks.end(),
                                    [&id](const TaskSummary& task) { return task.id == id; });
    if (found == state_.tasks.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(state_.tasks.begin(), found));
}

bool TaskWorkspaceController::is_task_visible(const TaskSummary& task) const {
    const auto archived = task_organizer_.is_archived(task.id);
    return state_.showing_archived_tasks ? archived : !archived;
}

void TaskWorkspaceController::switch_to_root(std::filesystem::path root, TaskOrganizer organizer) {
    selections_[state_.root.generic_string()] = {
        .task_id = state_.task_id, .new_task = keep_empty_selection_, .viewing_run = viewing_run_};
    const auto model_execution_available = state_.model_execution_available;
    task_organizer_ = std::move(organizer);
    options_.root = std::move(root);
    state_ = TaskWorkspaceState{.root = options_.root,
                                .workspace_roots = workspace_catalog_.state().roots,
                                .workspace_task_counts = workspace_catalog_.state().task_counts,
                                .model_execution_available = model_execution_available};
    viewing_run_ = false;
    keep_empty_selection_ = false;
    const auto bookmark = selections_.find(state_.root.generic_string());
    if (bookmark != selections_.end()) {
        state_.task_id = bookmark->second.task_id;
        keep_empty_selection_ = bookmark->second.new_task;
        if (bookmark->second.viewing_run) {
            show_run();
        }
    }
    sync_run_state();
    sync_task_organization();
    notify(WorkspaceChange::all);
    refresh();
}

} // namespace mint::desktop
