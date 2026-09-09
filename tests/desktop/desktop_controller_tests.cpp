#include "application/task_workspace_controller.hpp"

#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <utility>

namespace {

class FakeRuntime final : public mint::desktop::AgentRuntime {
  public:
    void set_callbacks(mint::desktop::AgentRuntimeCallbacks value) override {
        callbacks = std::move(value);
    }

    void refresh_tasks(const std::filesystem::path& value, const std::string&) override {
        refreshed_root = value;
        ++refresh_count;
    }

    void load_task(const std::string& value) override {
        loaded_task = value;
        ++load_count;
    }

    void start_task(mint::desktop::RunRequest value) override {
        run_request = std::move(value);
        active = true;
    }

    void resume_task(mint::desktop::ResumeRequest value) override {
        resume_request = std::move(value);
        active = true;
    }

    [[nodiscard]] bool resolve_approval(bool value) override {
        approval = value;
        if (!approval_accepted && callbacks.error) {
            callbacks.error({.operation = mint::desktop::RuntimeOperation::approval,
                             .detail = "Approval channel failed"});
        }
        return approval_accepted;
    }
    void cancel() override {
        cancelled = true;
    }
    [[nodiscard]] bool running() const noexcept override {
        return active;
    }

    mint::desktop::AgentRuntimeCallbacks callbacks;
    std::filesystem::path refreshed_root;
    std::string loaded_task;
    int load_count = 0;
    int refresh_count = 0;
    mint::desktop::RunRequest run_request;
    mint::desktop::ResumeRequest resume_request;
    std::optional<bool> approval;
    bool approval_accepted = true;
    bool active = false;
    bool cancelled = false;
};

class MemoryTaskOrganizationRepository final : public mint::desktop::TaskOrganizationRepository {
  public:
    [[nodiscard]] std::optional<mint::desktop::TaskOrganization>
    load(std::string_view project_scope) const override {
        if (!load_succeeds) {
            return std::nullopt;
        }
        const auto found = values.find(std::string(project_scope));
        if (found == values.end()) {
            return mint::desktop::TaskOrganization{};
        }
        return found->second;
    }

    bool save(std::string_view project_scope,
              const mint::desktop::TaskOrganization& organization) override {
        last_saved_scope = std::string(project_scope);
        values[std::string(project_scope)] = organization;
        return true;
    }

    std::unordered_map<std::string, mint::desktop::TaskOrganization> values;
    std::string last_saved_scope;
    bool load_succeeds = true;
};

class MemoryWorkspaceCatalogRepository final : public mint::desktop::WorkspaceCatalogRepository {
  public:
    [[nodiscard]] std::optional<mint::desktop::WorkspaceCatalogState> load() const override {
        return stored;
    }

    bool save(const mint::desktop::WorkspaceCatalogState& catalog) override {
        if (!save_succeeds) {
            return false;
        }
        stored = catalog;
        return true;
    }

    std::optional<mint::desktop::WorkspaceCatalogState> stored =
        mint::desktop::WorkspaceCatalogState{};
    bool save_succeeds = true;
};

TEST(DesktopController, RestoresTheLastSelectedWorkspaceWithinItsOwnStateDirectory) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto base = std::filesystem::path(directory.path().toStdString());
    const auto alpha = mint::desktop::normalize_workspace_root(base / "alpha");
    const auto beta = mint::desktop::normalize_workspace_root(base / "beta");
    std::filesystem::create_directories(alpha);
    std::filesystem::create_directories(beta);
    MemoryWorkspaceCatalogRepository repository;
    repository.stored->roots = {alpha, beta};
    repository.stored->last_active_root = alpha;
    {
        FakeRuntime runtime;
        mint::desktop::TaskWorkspaceController controller(
            runtime, {.workspace_catalog_repository = &repository, .state_root = base / "state"});
        EXPECT_EQ(controller.state().root, alpha);
        controller.activate();
        EXPECT_EQ(runtime.refreshed_root, alpha);
        ASSERT_TRUE(controller.set_root(beta));
        ASSERT_TRUE(controller.set_root(alpha));
        EXPECT_EQ(repository.stored->last_active_root, alpha);
        EXPECT_EQ(repository.stored->roots, (std::vector<std::filesystem::path>{alpha, beta}));
    }
    FakeRuntime reopened_runtime;
    mint::desktop::TaskWorkspaceController reopened(
        reopened_runtime,
        {.workspace_catalog_repository = &repository, .state_root = base / "state"});
    EXPECT_EQ(reopened.state().root, alpha);

    FakeRuntime isolated_runtime;
    MemoryWorkspaceCatalogRepository other_repository;
    mint::desktop::TaskWorkspaceController isolated(
        isolated_runtime,
        {.workspace_catalog_repository = &other_repository, .state_root = base / "other-state"});
    EXPECT_TRUE(isolated.state().root.empty());
    EXPECT_TRUE(isolated.state().workspace_roots.empty());
}

TEST(DesktopController, LegacyStartupSkipsRelativeInvalidAndOverlappingWorkspaces) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto base = mint::desktop::normalize_workspace_root(
        std::filesystem::path(directory.path().toStdString()));
    const auto alpha = base / "alpha";
    const auto beta = base / "beta";
    const auto state_root = base / "state";
    std::filesystem::create_directories(alpha);
    std::filesystem::create_directories(beta);
    std::filesystem::create_directories(state_root / "nested");
    std::ofstream(base / "file.txt") << "not a directory";
    FakeRuntime runtime;
    MemoryWorkspaceCatalogRepository repository;
    repository.stored->roots = {alpha,
                                beta,
                                base / "missing",
                                base / "file.txt",
                                state_root,
                                state_root / "nested",
                                base,
                                base.root_path(),
                                "."};
    repository.stored->task_counts = {{.root = alpha, .unarchived = 2},
                                      {.root = base.root_path(), .unarchived = 1}};
    repository.stored->last_active_root = state_root;
    mint::desktop::TaskWorkspaceController controller(
        runtime, {.workspace_catalog_repository = &repository, .state_root = state_root});

    EXPECT_EQ(controller.state().root, beta);
    EXPECT_EQ(controller.state().workspace_roots,
              (std::vector<std::filesystem::path>{alpha, beta}));
    ASSERT_EQ(repository.stored->task_counts.size(), 1U);
    EXPECT_EQ(repository.stored->task_counts.front().root, alpha);
    EXPECT_EQ(repository.stored->last_active_root, beta);
}

TEST(DesktopController, ExplicitWorkspaceWinsAndAnInvalidChoiceNeverFallsBackToAnotherProject) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto base = mint::desktop::normalize_workspace_root(
        std::filesystem::path(directory.path().toStdString()));
    const auto alpha = base / "alpha";
    const auto beta = base / "beta";
    std::filesystem::create_directories(alpha);
    std::filesystem::create_directories(beta);
    MemoryWorkspaceCatalogRepository repository;
    repository.stored->roots = {alpha, beta};
    repository.stored->last_active_root = beta;
    {
        FakeRuntime runtime;
        mint::desktop::TaskWorkspaceController controller(
            runtime, {.root = alpha,
                      .workspace_catalog_repository = &repository,
                      .state_root = base / "state"});
        EXPECT_EQ(controller.state().root, alpha);
        EXPECT_EQ(repository.stored->last_active_root, alpha);
    }
    for (const auto& invalid : {base.root_path(), base / "missing", base / "state", base}) {
        FakeRuntime runtime;
        mint::desktop::TaskWorkspaceController controller(
            runtime, {.root = invalid,
                      .workspace_catalog_repository = &repository,
                      .state_root = base / "state"});
        EXPECT_TRUE(controller.state().root.empty()) << invalid;
        EXPECT_EQ(controller.state().notice, mint::desktop::WorkspaceNotice::invalid_workspace);
        controller.activate();
        EXPECT_FALSE(controller.start("Do not run in another project"));
        EXPECT_EQ(runtime.refresh_count, 0);
        EXPECT_FALSE(runtime.active);
        EXPECT_EQ(repository.stored->last_active_root, alpha);
    }
}

TEST(DesktopController, AnEmptyStartupDoesNotRegisterTheWorkingDirectoryOrStartTheCli) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto base = mint::desktop::normalize_workspace_root(
        std::filesystem::path(directory.path().toStdString()));
    const auto project = base / "project";
    std::filesystem::create_directories(project);
    FakeRuntime runtime;
    MemoryWorkspaceCatalogRepository repository;
    mint::desktop::TaskWorkspaceController controller(
        runtime, {.workspace_catalog_repository = &repository, .state_root = base / "state"});

    controller.activate();
    controller.refresh();
    controller.create_task();
    EXPECT_TRUE(controller.state().root.empty());
    EXPECT_TRUE(controller.state().workspace_roots.empty());
    EXPECT_TRUE(repository.stored->roots.empty());
    EXPECT_TRUE(repository.stored->last_active_root.empty());
    EXPECT_EQ(runtime.refresh_count, 0);
    EXPECT_FALSE(controller.state().loading_tasks);
    EXPECT_FALSE(controller.start("Need a workspace"));
    EXPECT_FALSE(controller.resume());
    EXPECT_FALSE(runtime.active);

    EXPECT_FALSE(controller.set_root(base.root_path()));
    EXPECT_TRUE(controller.state().workspace_roots.empty());
    ASSERT_TRUE(controller.set_root(project));
    EXPECT_EQ(controller.state().notice, mint::desktop::WorkspaceNotice::none);
    EXPECT_EQ(repository.stored->last_active_root, project);
    EXPECT_EQ(runtime.refreshed_root, project);
    ASSERT_TRUE(controller.start("Inspect this project"));
    EXPECT_EQ(runtime.run_request.root, project);
}

TEST(DesktopController, StartupRejectsSymlinkAliasesOfTheSettingsDirectory) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto base = mint::desktop::normalize_workspace_root(
        std::filesystem::path(directory.path().toStdString()));
    const auto state_root = base / "state";
    const auto project = base / "project";
    const auto alias = base / "state-alias";
    std::filesystem::create_directories(state_root);
    std::filesystem::create_directories(project);
    std::error_code error;
    std::filesystem::create_directory_symlink(state_root, alias, error);
    if (error) {
        GTEST_SKIP() << "Directory symlinks are unavailable: " << error.message();
    }
    mint::desktop::WorkspaceCatalogState stored{.roots = {project, alias},
                                                .last_active_root = alias};
    EXPECT_EQ(mint::desktop::select_startup_workspace({}, stored, state_root), project);
    EXPECT_TRUE(mint::desktop::select_startup_workspace(alias, stored, state_root).empty());
}

TEST(DesktopController, PersistsMultipleWorkspacesAndSwitchesTheActiveProject) {
    FakeRuntime runtime;
    MemoryWorkspaceCatalogRepository repository;
    mint::desktop::TaskWorkspaceController controller(
        runtime,
        {.root = "/projects/alpha", .language = "en", .workspace_catalog_repository = &repository});

    ASSERT_EQ(controller.state().workspace_roots.size(), 1U);
    EXPECT_EQ(controller.state().workspace_roots.front(), std::filesystem::path("/projects/alpha"));
    runtime.callbacks.tasks_loaded({{.id = "task-1", .question = "Inspect", .status = "completed"}},
                                   "/state");
    const auto alpha_counts =
        std::find_if(controller.state().workspace_task_counts.begin(),
                     controller.state().workspace_task_counts.end(), [](const auto& counts) {
                         return counts.root == std::filesystem::path("/projects/alpha");
                     });
    ASSERT_NE(alpha_counts, controller.state().workspace_task_counts.end());
    EXPECT_EQ(alpha_counts->unarchived, 1U);

    EXPECT_TRUE(controller.set_root("/projects/beta"));

    EXPECT_EQ(controller.state().root, std::filesystem::path("/projects/beta"));
    ASSERT_EQ(controller.state().workspace_roots.size(), 2U);
    EXPECT_EQ(controller.state().workspace_roots.front(), std::filesystem::path("/projects/alpha"));
    EXPECT_EQ(controller.state().workspace_roots.back(), std::filesystem::path("/projects/beta"));
    ASSERT_TRUE(repository.stored.has_value());
    EXPECT_EQ(repository.stored->roots, controller.state().workspace_roots);
    EXPECT_EQ(repository.stored->last_active_root, std::filesystem::path("/projects/beta"));
    EXPECT_EQ(runtime.refreshed_root, std::filesystem::path("/projects/beta"));
    const auto retained_alpha_counts =
        std::find_if(controller.state().workspace_task_counts.begin(),
                     controller.state().workspace_task_counts.end(), [](const auto& counts) {
                         return counts.root == std::filesystem::path("/projects/alpha");
                     });
    const auto beta_counts =
        std::find_if(controller.state().workspace_task_counts.begin(),
                     controller.state().workspace_task_counts.end(), [](const auto& counts) {
                         return counts.root == std::filesystem::path("/projects/beta");
                     });
    ASSERT_NE(retained_alpha_counts, controller.state().workspace_task_counts.end());
    EXPECT_EQ(retained_alpha_counts->unarchived, 1U);
    EXPECT_EQ(beta_counts, controller.state().workspace_task_counts.end());

    runtime.callbacks.tasks_loaded({}, "/state-beta");
    const auto known_empty_beta =
        std::find_if(controller.state().workspace_task_counts.begin(),
                     controller.state().workspace_task_counts.end(), [](const auto& counts) {
                         return counts.root == std::filesystem::path("/projects/beta");
                     });
    ASSERT_NE(known_empty_beta, controller.state().workspace_task_counts.end());
    EXPECT_EQ(known_empty_beta->unarchived, 0U);
    ASSERT_TRUE(repository.stored.has_value());
    EXPECT_EQ(repository.stored->task_counts, controller.state().workspace_task_counts);
}

TEST(DesktopController, ModelSelectionChangesOnlyTheNextRunSnapshot) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(
        runtime, {.root = "/workspace", .config = "/old-override.json", .language = "en"});
    ASSERT_TRUE(controller.start("First run"));
    EXPECT_EQ(runtime.run_request.config, "/old-override.json");
    controller.set_config({});
    EXPECT_EQ(runtime.run_request.config, "/old-override.json");
    EXPECT_TRUE(controller.state().running);
    runtime.active = false;
    runtime.callbacks.run_finished({.status = "completed", .answer = "Done"});
    controller.create_task();
    ASSERT_TRUE(controller.start("Next run"));
    EXPECT_TRUE(runtime.run_request.config.empty());
}

TEST(DesktopController, ModelSettingsGateLeavesDemoAndActiveRunControlsAvailable) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime, {.root = "/workspace"});
    ASSERT_TRUE(controller.start("Active model task"));
    controller.set_model_execution_available(false);
    EXPECT_TRUE(controller.state().running);
    controller.create_task();
    EXPECT_TRUE(controller.state().running);
    EXPECT_TRUE(controller.set_root("/another-workspace"));
    EXPECT_FALSE(controller.state().model_execution_available);
    controller.cancel();
    EXPECT_TRUE(runtime.cancelled);

    FakeRuntime demo_runtime;
    mint::desktop::TaskWorkspaceController demo(demo_runtime, {.root = "/workspace", .demo = true});
    demo.set_model_execution_available(false);
    EXPECT_TRUE(demo.state().model_execution_available);
    EXPECT_TRUE(demo.start("Offline demo remains available"));
    EXPECT_TRUE(demo_runtime.run_request.demo);
}

TEST(DesktopController, KeepsRunningWhileBrowsingAnotherWorkspaceAndRestoresTheRun) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(
        runtime, {.root = "/projects/alpha", .language = "en"});
    controller.activate();
    ASSERT_TRUE(controller.start("Inspect the project"));
    runtime.callbacks.task_ready("active-task");
    runtime.callbacks.event_received(
        {.sequence = 1, .kind = mint::desktop::RunEventKind::tool_started});

    ASSERT_TRUE(controller.set_root("/projects/beta"));
    EXPECT_EQ(controller.state().root, "/projects/beta");
    EXPECT_EQ(controller.state().running_root, "/projects/alpha");
    EXPECT_EQ(controller.state().running_task_id, "active-task");
    EXPECT_EQ(controller.state().running_title, "Inspect the project");
    EXPECT_TRUE(controller.state().running);
    EXPECT_FALSE(controller.state().viewing_running_task());
    EXPECT_FALSE(runtime.cancelled);
    EXPECT_FALSE(controller.start("Another task"));
    runtime.callbacks.tasks_loaded(
        {{.id = "history", .question = "Earlier", .status = "completed"}}, "/state/beta");
    runtime.callbacks.task_loaded(
        "history",
        {.result = mint::desktop::TaskResult{.status = "completed", .answer = "Earlier result"}});
    runtime.callbacks.event_received(
        {.sequence = 2, .kind = mint::desktop::RunEventKind::tool_completed});
    runtime.callbacks.approval_requested({.program = "build"});
    EXPECT_EQ(controller.state().task_id, "history");
    EXPECT_TRUE(controller.state().events.empty());
    EXPECT_EQ(controller.state().result->answer, "Earlier result");
    EXPECT_FALSE(controller.state().approval.has_value());
    EXPECT_EQ(controller.state().running_status, "waiting");
    controller.resolve_approval(true);
    EXPECT_FALSE(runtime.approval.has_value());

    ASSERT_TRUE(controller.set_root("/projects/alpha"));
    EXPECT_TRUE(controller.state().viewing_running_task());
    EXPECT_EQ(controller.state().task_id, "active-task");
    EXPECT_EQ(controller.state().events.size(), 2U);
    ASSERT_TRUE(controller.state().approval.has_value());
    EXPECT_EQ(controller.state().status, "waiting");
    controller.resolve_approval(true);
    ASSERT_TRUE(runtime.approval.has_value());
    EXPECT_TRUE(*runtime.approval);
    controller.cancel();
    EXPECT_TRUE(runtime.cancelled);
}

TEST(DesktopController, BackgroundCompletionDoesNotReplaceAnotherConversationOrItsHistory) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime, {.root = "/workspace"});
    runtime.callbacks.tasks_loaded(
        {{.id = "history", .question = "Earlier", .status = "completed"}}, "/state");
    ASSERT_TRUE(controller.start("Current run"));
    runtime.callbacks.task_ready("active-task");
    ASSERT_TRUE(controller.select_task("history"));
    runtime.callbacks.task_loaded(
        "history",
        {.result = mint::desktop::TaskResult{.status = "completed", .answer = "Earlier result"}});
    runtime.callbacks.event_received(
        {.sequence = 4, .kind = mint::desktop::RunEventKind::tool_completed});
    runtime.callbacks.run_finished({.status = "max_turns", .answer = "Paused for another turn"});

    EXPECT_FALSE(controller.state().running);
    EXPECT_EQ(controller.state().task_id, "history");
    EXPECT_EQ(controller.state().result->answer, "Earlier result");
    EXPECT_TRUE(controller.state().events.empty());
    EXPECT_TRUE(controller.state().running_task_id.empty());
    ASSERT_TRUE(controller.select_task("active-task"));
    EXPECT_EQ(controller.state().status, "max_turns");
    EXPECT_EQ(controller.state().events.size(), 1U);
    EXPECT_EQ(controller.state().result->answer, "Paused for another turn");
    EXPECT_TRUE(controller.state().can_resume());
}

TEST(DesktopController, BackgroundCompletionDoesNotStealAWorkspaceOrAnEmptyDraft) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime, {.root = "/projects/alpha"});
    ASSERT_TRUE(controller.start("Current run"));
    runtime.callbacks.task_ready("active-task");
    ASSERT_TRUE(controller.set_root("/projects/beta"));
    runtime.callbacks.tasks_loaded(
        {{.id = "history", .question = "Earlier", .status = "completed"}}, "/state/beta");
    controller.create_task();
    runtime.callbacks.run_finished({.status = "completed", .answer = "New result"});

    EXPECT_EQ(controller.state().root, "/projects/beta");
    EXPECT_TRUE(controller.state().task_id.empty());
    EXPECT_TRUE(controller.state().events.empty());
    EXPECT_FALSE(controller.state().result.has_value());
    controller.refresh();
    runtime.callbacks.tasks_loaded(
        {{.id = "history", .question = "Earlier", .status = "completed"}}, "/state/beta");
    EXPECT_TRUE(controller.state().task_id.empty());
    ASSERT_TRUE(controller.set_root("/projects/alpha"));
    EXPECT_EQ(controller.state().task_id, "active-task");
    ASSERT_TRUE(controller.state().result.has_value());
    EXPECT_EQ(controller.state().result->answer, "New result");
}

TEST(DesktopController, RunReadyCannotClaimANewDraftBeforeTheCliAssignsItsId) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime, {.root = "/workspace"});
    ASSERT_TRUE(controller.start("Current run"));
    EXPECT_TRUE(controller.state().viewing_running_task());
    controller.create_task();
    EXPECT_FALSE(controller.state().viewing_running_task());
    runtime.callbacks.task_ready("active-task");
    runtime.callbacks.event_received(
        {.sequence = 1, .kind = mint::desktop::RunEventKind::tool_started});
    EXPECT_TRUE(controller.state().task_id.empty());
    EXPECT_TRUE(controller.state().title.empty());
    EXPECT_TRUE(controller.state().events.empty());
    EXPECT_EQ(controller.state().running_task_id, "active-task");
    ASSERT_TRUE(controller.select_task("active-task"));
    EXPECT_TRUE(controller.state().viewing_running_task());
    EXPECT_EQ(controller.state().events.size(), 1U);
}

TEST(DesktopController, BackgroundFailuresAndHistoryFailuresKeepTheirOwners) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime, {.root = "/workspace"});
    runtime.callbacks.tasks_loaded(
        {{.id = "history", .question = "Earlier", .status = "completed"}}, "/state");
    ASSERT_TRUE(controller.start("Current run"));
    runtime.callbacks.task_ready("active-task");
    ASSERT_TRUE(controller.select_task("history"));
    runtime.callbacks.error({.operation = mint::desktop::RuntimeOperation::task_history,
                             .detail = "History unavailable"});
    EXPECT_EQ(controller.state().error, "History unavailable");
    EXPECT_TRUE(controller.state().running);
    EXPECT_FALSE(controller.state().loading_history);
    runtime.callbacks.error(
        {.operation = mint::desktop::RuntimeOperation::task_run, .detail = "Run failed"});
    EXPECT_EQ(controller.state().task_id, "history");
    EXPECT_EQ(controller.state().error, "History unavailable");
    EXPECT_FALSE(controller.state().running);
    ASSERT_TRUE(controller.select_task("active-task"));
    EXPECT_EQ(controller.state().error, "Run failed");
    EXPECT_EQ(controller.state().status, "failed");
}

TEST(DesktopController, PublishesBackgroundApprovalWithoutReplacingTheSelection) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime, {.root = "/workspace"});
    runtime.callbacks.tasks_loaded(
        {{.id = "history", .question = "Earlier", .status = "completed"}}, "/state");
    ASSERT_TRUE(controller.start("Current run"));
    runtime.callbacks.task_ready("active-task");
    ASSERT_TRUE(controller.select_task("history"));
    std::size_t notifications = 0;
    controller.set_listener([&](mint::desktop::WorkspaceChange change) {
        notifications += change == mint::desktop::WorkspaceChange::run_state ? 1 : 0;
    });
    runtime.callbacks.approval_requested({.program = "build"});
    EXPECT_EQ(notifications, 1U);
    EXPECT_EQ(controller.state().task_id, "history");
    EXPECT_FALSE(controller.state().approval.has_value());
    EXPECT_EQ(controller.state().running_status, "waiting");
    const auto running_task =
        std::find_if(controller.state().tasks.begin(), controller.state().tasks.end(),
                     [](const auto& task) { return task.id == "active-task"; });
    ASSERT_NE(running_task, controller.state().tasks.end());
    EXPECT_EQ(running_task->status, "waiting");
    ASSERT_TRUE(controller.select_task("active-task"));
    ASSERT_TRUE(controller.state().approval.has_value());
    controller.resolve_approval(true);
    ASSERT_TRUE(runtime.approval.has_value());
    EXPECT_EQ(controller.state().running_status, "running");
}

TEST(DesktopController, ResumeIgnoresReplayedEventsAndKeepsNewEventsInOrder) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime, {.root = "/workspace"});
    runtime.callbacks.tasks_loaded(
        {{.id = "task", .question = "Earlier", .status = "max_turns", .resumable = true}},
        "/state");
    runtime.callbacks.task_loaded(
        "task", {.events = {{.sequence = 5, .kind = mint::desktop::RunEventKind::tool_completed},
                            {.sequence = 6,
                             .kind = mint::desktop::RunEventKind::task_finished,
                             .status = "max_turns"}}});
    ASSERT_TRUE(controller.resume());
    runtime.callbacks.task_ready("task");
    runtime.callbacks.events_received(
        {{.sequence = 1, .kind = mint::desktop::RunEventKind::task_started},
         {.sequence = 5, .kind = mint::desktop::RunEventKind::tool_completed},
         {.sequence = 6, .kind = mint::desktop::RunEventKind::task_finished, .status = "max_turns"},
         {.sequence = 7, .kind = mint::desktop::RunEventKind::model_requested}});
    ASSERT_EQ(controller.state().events.size(), 3U);
    EXPECT_EQ(controller.state().events.front().sequence, 5U);
    EXPECT_EQ(controller.state().events.back().sequence, 7U);
    EXPECT_EQ(controller.state().status, "running");
    runtime.callbacks.events_received(
        {{.sequence = 7, .kind = mint::desktop::RunEventKind::model_requested}});
    EXPECT_EQ(controller.state().events.size(), 3U);
}

TEST(DesktopController, KeepsOrganizationAvailableAndProtectsOnlyTheActiveRun) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime, {.root = "/projects/alpha"});
    runtime.callbacks.tasks_loaded(
        {{.id = "history", .question = "Earlier", .status = "completed"}}, "/state");
    ASSERT_TRUE(controller.start("Current run"));
    runtime.callbacks.task_ready("active-task");
    EXPECT_TRUE(controller.toggle_task_pin("history"));
    EXPECT_TRUE(controller.archive_task("history"));
    EXPECT_FALSE(controller.archive_task("active-task"));
    EXPECT_TRUE(controller.show_archived_tasks(true));
    EXPECT_EQ(controller.state().task_id, "history");
    EXPECT_TRUE(controller.unarchive_task("history"));
    EXPECT_TRUE(controller.state().task_id.empty());
    ASSERT_TRUE(controller.set_root("/projects/beta"));
    EXPECT_FALSE(controller.remove_root("/projects/alpha"));
    EXPECT_TRUE(controller.remove_root("/projects/beta"));
    EXPECT_EQ(controller.state().root, "/projects/alpha");
    EXPECT_TRUE(controller.state().running);
}

TEST(DesktopController, AppendsBoundedEventBatchesWithoutNotifyingForBackgroundEvents) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime, {.root = "/workspace"});
    ASSERT_TRUE(controller.start("Current run"));
    runtime.callbacks.task_ready("active-task");
    std::size_t event_notifications = 0;
    controller.set_listener([&](mint::desktop::WorkspaceChange change) {
        event_notifications += change == mint::desktop::WorkspaceChange::event_appended ? 1 : 0;
    });
    std::vector<mint::desktop::RunEvent> events;
    for (std::size_t index = 0; index < mint::desktop::maximum_retained_run_events + 12; ++index) {
        events.push_back({.sequence = index, .kind = mint::desktop::RunEventKind::tool_completed});
    }
    runtime.callbacks.events_received(std::move(events));
    EXPECT_EQ(event_notifications, 1U);
    ASSERT_EQ(controller.state().events.size(), mint::desktop::maximum_retained_run_events);
    EXPECT_EQ(controller.state().events.front().sequence, 12U);
    controller.create_task();
    runtime.callbacks.events_received(
        {{.sequence = mint::desktop::maximum_retained_run_events + 12}});
    EXPECT_EQ(event_notifications, 1U);
    ASSERT_TRUE(controller.select_task("active-task"));
    ASSERT_EQ(controller.state().events.size(), mint::desktop::maximum_retained_run_events);
    EXPECT_EQ(controller.state().events.front().sequence, 13U);
    EXPECT_EQ(controller.state().events.back().sequence,
              mint::desktop::maximum_retained_run_events + 12);
}

TEST(DesktopController, NormalizesWorkspaceAliasesAndRetainsASingleCatalogEntry) {
    FakeRuntime runtime;
    MemoryWorkspaceCatalogRepository repository;
    const auto root = std::filesystem::current_path();
    repository.stored->roots = {".", root / ".", root};
    mint::desktop::TaskWorkspaceController controller(
        runtime, {.root = ".", .workspace_catalog_repository = &repository});
    EXPECT_TRUE(controller.state().root.is_absolute());
    EXPECT_EQ(controller.state().root, std::filesystem::weakly_canonical(root));
    ASSERT_EQ(controller.state().workspace_roots.size(), 1U);
    EXPECT_TRUE(controller.set_root(root / "."));
    EXPECT_EQ(controller.state().workspace_roots.size(), 1U);
}

TEST(DesktopController, KeepsPinnedAndArchivedTasksWhenCanonicalizingAnExistingWorkspace) {
    FakeRuntime runtime;
    MemoryWorkspaceCatalogRepository catalog_repository;
    MemoryTaskOrganizationRepository organization_repository;
    catalog_repository.stored->roots = {".", std::filesystem::current_path()};
    organization_repository.values["."].pinned_task_ids.insert("pinned");
    organization_repository.values["."].archived_task_ids.insert("archived");
    mint::desktop::TaskWorkspaceController controller(
        runtime, {.root = ".",
                  .task_organization_repository = &organization_repository,
                  .workspace_catalog_repository = &catalog_repository});
    EXPECT_TRUE(controller.state().task_organization.pinned_task_ids.contains("pinned"));
    EXPECT_TRUE(controller.state().task_organization.archived_task_ids.contains("archived"));
    runtime.callbacks.tasks_loaded({{.id = "pinned"}, {.id = "archived"}}, "/state");
    ASSERT_TRUE(controller.unarchive_task("archived"));
    mint::desktop::TaskWorkspaceController reopened(
        runtime, {.root = ".",
                  .task_organization_repository = &organization_repository,
                  .workspace_catalog_repository = &catalog_repository});
    EXPECT_FALSE(reopened.state().task_organization.archived_task_ids.contains("archived"));
}

TEST(DesktopController, FailedOrganizationLoadKeepsTheCurrentWorkspaceAndScope) {
    FakeRuntime runtime;
    MemoryTaskOrganizationRepository organization_repository;
    MemoryWorkspaceCatalogRepository catalog_repository;
    mint::desktop::TaskWorkspaceController controller(
        runtime, {.root = "/projects/alpha",
                  .language = "en",
                  .task_organization_repository = &organization_repository,
                  .workspace_catalog_repository = &catalog_repository});
    runtime.callbacks.tasks_loaded({{.id = "task-1", .question = "Inspect", .status = "completed"}},
                                   "/state");
    organization_repository.load_succeeds = false;

    EXPECT_FALSE(controller.set_root("/projects/beta"));
    EXPECT_EQ(controller.state().root, std::filesystem::path("/projects/alpha"));
    EXPECT_EQ(controller.state().workspace_roots,
              std::vector<std::filesystem::path>{std::filesystem::path("/projects/alpha")});

    ASSERT_TRUE(controller.toggle_task_pin("task-1"));
    EXPECT_EQ(organization_repository.last_saved_scope, "/projects/alpha");
}

TEST(DesktopController, RemovingAWorkspaceKeepsOneSafeFallback) {
    FakeRuntime runtime;
    MemoryWorkspaceCatalogRepository repository;
    mint::desktop::TaskWorkspaceController controller(
        runtime,
        {.root = "/projects/alpha", .language = "en", .workspace_catalog_repository = &repository});
    ASSERT_TRUE(controller.set_root("/projects/beta"));

    ASSERT_TRUE(controller.remove_root("/projects/beta"));
    EXPECT_EQ(controller.state().root, std::filesystem::path("/projects/alpha"));
    EXPECT_EQ(repository.stored->last_active_root, std::filesystem::path("/projects/alpha"));
    EXPECT_EQ(controller.state().workspace_roots,
              std::vector<std::filesystem::path>{std::filesystem::path("/projects/alpha")});
    EXPECT_EQ(runtime.refreshed_root, std::filesystem::path("/projects/alpha"));
    EXPECT_FALSE(controller.remove_root("/projects/alpha"));
}

TEST(DesktopController, OwnsTaskSelectionWithoutExposingTheCliProtocol) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});

    controller.activate();
    EXPECT_EQ(runtime.refreshed_root, std::filesystem::path("/workspace"));

    runtime.callbacks.tasks_loaded({{.id = "task-1",
                                     .question = "Inspect the project",
                                     .status = "running",
                                     .resumable = true}},
                                   "/state/project");

    EXPECT_EQ(controller.state().task_id, "task-1");
    EXPECT_EQ(controller.state().status, "resumable");
    EXPECT_TRUE(controller.state().can_resume());
    EXPECT_EQ(runtime.loaded_task, "task-1");
    EXPECT_EQ(runtime.load_count, 1);

    EXPECT_TRUE(controller.select_task("task-1"));
    EXPECT_EQ(runtime.load_count, 1);
}

TEST(DesktopController, ExposesBalancedWorkspaceAndHistoryLoadingStates) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});

    controller.activate();
    EXPECT_TRUE(controller.state().loading_tasks);
    EXPECT_FALSE(controller.state().loading_history);

    runtime.callbacks.tasks_loaded({{.id = "task-1", .question = "Inspect", .status = "completed"}},
                                   "/state");
    EXPECT_FALSE(controller.state().loading_tasks);
    EXPECT_TRUE(controller.state().loading_history);

    runtime.callbacks.task_loaded("task-1", {});
    EXPECT_FALSE(controller.state().loading_tasks);
    EXPECT_FALSE(controller.state().loading_history);
}

TEST(DesktopController, IgnoresHistoryAfterCreatingANewTask) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    runtime.callbacks.tasks_loaded({{.id = "task-1", .question = "Inspect", .status = "completed"}},
                                   "/state");
    controller.create_task();

    runtime.callbacks.task_loaded(
        "task-1",
        {.events = {{.sequence = 1, .kind = mint::desktop::RunEventKind::model_completed}},
         .result = mint::desktop::TaskResult{.status = "completed", .answer = "Old"}});

    EXPECT_TRUE(controller.state().task_id.empty());
    EXPECT_TRUE(controller.state().events.empty());
    EXPECT_FALSE(controller.state().result.has_value());
}

TEST(DesktopController, IgnoresHistoryForATaskThatIsNoLongerSelected) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    runtime.callbacks.tasks_loaded({{.id = "task-1", .question = "First", .status = "completed"},
                                    {.id = "task-2", .question = "Second", .status = "completed"}},
                                   "/state");
    ASSERT_TRUE(controller.select_task("task-2"));

    runtime.callbacks.task_loaded(
        "task-1",
        {.events = {{.sequence = 1, .kind = mint::desktop::RunEventKind::model_completed}},
         .result = mint::desktop::TaskResult{.status = "completed", .answer = "Old"}});

    EXPECT_EQ(controller.state().task_id, "task-2");
    EXPECT_TRUE(controller.state().events.empty());
    EXPECT_FALSE(controller.state().result.has_value());
}

TEST(DesktopController, IgnoresHistoryAfterSwitchingWorkspaces) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(
        runtime, {.root = "/projects/alpha", .language = "en"});
    runtime.callbacks.tasks_loaded({{.id = "task-1", .question = "Inspect", .status = "completed"}},
                                   "/state");
    ASSERT_TRUE(controller.set_root("/projects/beta"));

    runtime.callbacks.task_loaded(
        "task-1",
        {.events = {{.sequence = 1, .kind = mint::desktop::RunEventKind::model_completed}},
         .result = mint::desktop::TaskResult{.status = "completed", .answer = "Old"}});

    EXPECT_EQ(controller.state().root, std::filesystem::path("/projects/beta"));
    EXPECT_TRUE(controller.state().task_id.empty());
    EXPECT_TRUE(controller.state().events.empty());
    EXPECT_FALSE(controller.state().result.has_value());
}

TEST(DesktopController, CoordinatesRunApprovalAndResultAsTypedState) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(
        runtime, {.root = "/workspace", .config = "/config.json", .language = "zh-CN"});

    ASSERT_TRUE(controller.start("Fix the tests"));
    EXPECT_TRUE(controller.state().running);
    EXPECT_EQ(runtime.run_request.question, "Fix the tests");

    runtime.callbacks.event_received({.sequence = 1,
                                      .kind = mint::desktop::RunEventKind::tool_started,
                                      .raw_type = "tool_started",
                                      .name = "read_file"});
    ASSERT_EQ(controller.state().events.size(), 1U);

    runtime.callbacks.approval_requested(
        {.kind = mint::desktop::ApprovalKind::changeset, .paths = {"src/main.cpp"}});
    EXPECT_EQ(controller.state().status, "waiting");
    controller.resolve_approval(true);
    ASSERT_TRUE(runtime.approval.has_value());
    EXPECT_TRUE(*runtime.approval);

    runtime.callbacks.run_finished(
        {.status = "completed", .answer = "Done", .usage = {.calls = 2, .total_tokens = 40}});
    EXPECT_FALSE(controller.state().running);
    ASSERT_TRUE(controller.state().result.has_value());
    EXPECT_EQ(controller.state().result->answer, "Done");
}

TEST(DesktopController, KeepsApprovalWhenRuntimeCannotAcceptTheDecision) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});

    ASSERT_TRUE(controller.start("Fix the tests"));
    runtime.callbacks.approval_requested(
        {.kind = mint::desktop::ApprovalKind::changeset, .paths = {"src/main.cpp"}});
    runtime.approval_accepted = false;

    controller.resolve_approval(true);

    EXPECT_TRUE(controller.state().approval.has_value());
    EXPECT_EQ(controller.state().status, "waiting");
    EXPECT_TRUE(controller.state().running);
    EXPECT_EQ(controller.state().error, "Approval channel failed");

    runtime.approval_accepted = true;
    controller.resolve_approval(true);

    EXPECT_FALSE(controller.state().approval.has_value());
    EXPECT_EQ(controller.state().status, "running");
    EXPECT_TRUE(controller.state().error.empty());
}

TEST(DesktopController, HistoryFailureDoesNotMasqueradeAsRunFailure) {
    FakeRuntime runtime;
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});

    runtime.callbacks.error({.operation = mint::desktop::RuntimeOperation::task_history,
                             .detail = "Project is not initialized"});
    EXPECT_EQ(controller.state().status, "pending");
    EXPECT_EQ(controller.state().error, "Project is not initialized");

    ASSERT_TRUE(controller.start("Inspect the project"));
    runtime.callbacks.error(
        {.operation = mint::desktop::RuntimeOperation::task_run, .detail = "Process failed"});
    EXPECT_EQ(controller.state().status, "failed");
    EXPECT_FALSE(controller.state().running);
}

TEST(DesktopController, KeepsTaskOrganizationSeparateFromExecutionState) {
    FakeRuntime runtime;
    MemoryTaskOrganizationRepository repository;
    mint::desktop::TaskWorkspaceController controller(
        runtime,
        {.root = "/workspace", .language = "en", .task_organization_repository = &repository});
    runtime.callbacks.tasks_loaded({{.id = "task-1", .question = "First", .status = "completed"},
                                    {.id = "task-2", .question = "Second", .status = "completed"}},
                                   "/state/project");
    ASSERT_EQ(controller.state().task_id, "task-1");

    ASSERT_TRUE(controller.toggle_task_pin("task-2"));
    EXPECT_TRUE(controller.state().task_organization.pinned_task_ids.contains("task-2"));

    ASSERT_TRUE(controller.archive_task("task-1"));
    EXPECT_TRUE(controller.state().task_organization.archived_task_ids.contains("task-1"));
    EXPECT_EQ(controller.state().task_id, "task-2");

    ASSERT_TRUE(controller.show_archived_tasks(true));
    EXPECT_TRUE(controller.state().showing_archived_tasks);
    EXPECT_EQ(controller.state().task_id, "task-1");

    ASSERT_TRUE(controller.unarchive_task("task-1"));
    EXPECT_TRUE(controller.state().task_id.empty());
    EXPECT_FALSE(controller.state().task_organization.archived_task_ids.contains("task-1"));
    EXPECT_FALSE(controller.state().running);
}

} // namespace
