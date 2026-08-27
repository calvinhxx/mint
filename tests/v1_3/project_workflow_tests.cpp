#include "mint/application/project_service.hpp"
#include "mint/domain/task_policy.hpp"
#include "mint/infrastructure/project_store.hpp"
#include "mint/infrastructure/session_store.hpp"
#include "mint/version.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

#define MINT_EXPECT(condition, message) EXPECT_TRUE(condition) << (message)

template <typename Callable> void expect_failure(Callable&& callable, const std::string& message) {
    EXPECT_ANY_THROW(callable()) << message;
}

void write_text(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not write " + path.generic_string());
    }
    output << content;
}

bool contains_string(const mint::Json& array, const std::string& expected) {
    if (!array.is_array()) {
        return false;
    }
    for (const auto& value : array) {
        if (value.is_string() && value.get<std::string>() == expected) {
            return true;
        }
    }
    return false;
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ =
            std::filesystem::temp_directory_path() / ("mint-v1-3-tests-" + std::to_string(stamp));
        if (!std::filesystem::create_directories(path_)) {
            throw std::runtime_error("could not create temporary test directory");
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

void expect_private_permissions(const std::filesystem::path& path, const std::string& message) {
#if !defined(_WIN32)
    std::error_code error;
    const auto permissions = std::filesystem::status(path, error).permissions();
    const auto public_permissions =
        std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    MINT_EXPECT(!error && (permissions & public_permissions) == std::filesystem::perms::none,
                message);
#else
    (void)path;
    (void)message;
#endif
}

void test_project_detection(const std::filesystem::path& root) {
    const auto cmake = root / "cmake-project";
    std::filesystem::create_directories(cmake / "src");
    std::filesystem::create_directories(cmake / "tests");
    write_text(cmake / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\n");
    write_text(cmake / "README.md", "# Fixture\n");

#if !defined(_WIN32)
    std::filesystem::create_directories(root / "external-docs");
    std::error_code symlink_error;
    std::filesystem::create_directory_symlink(root / "external-docs", cmake / "docs",
                                              symlink_error);
    MINT_EXPECT(!symlink_error, "test setup creates a directory symlink");
#endif

    const auto cmake_suggestion = mint::suggest_project_policy(cmake);
    MINT_EXPECT(cmake_suggestion.project_kind == "cmake", "CMake project is detected");
    MINT_EXPECT(cmake_suggestion.evidence == std::vector<std::string>{"CMakeLists.txt"},
                "CMake detection records evidence");
    MINT_EXPECT(contains_string(cmake_suggestion.policy.at("write_paths"), "src") &&
                    contains_string(cmake_suggestion.policy.at("write_paths"), "tests") &&
                    contains_string(cmake_suggestion.policy.at("write_paths"), "CMakeLists.txt"),
                "CMake policy suggests existing source and manifest paths");
#if !defined(_WIN32)
    MINT_EXPECT(!contains_string(cmake_suggestion.policy.at("write_paths"), "docs"),
                "policy suggestions do not grant a symlinked write path");
#endif
    MINT_EXPECT(cmake_suggestion.policy.at("recipes").size() == 3 &&
                    cmake_suggestion.policy.at("recipes").at(2).at("name") == "test" &&
                    cmake_suggestion.policy.at("recipes").at(2).at("verification") == true,
                "CMake policy has configure/build/test recipes and a verification gate");
    MINT_EXPECT(cmake_suggestion.policy.at("tool_limits").at("read_file_bytes") == 16 * 1024 &&
                    cmake_suggestion.policy.at("tool_limits").at("search_max_files") == 2000,
                "generated policy exposes tunable tool budgets instead of hiding them in code");

    const auto cargo = root / "cargo-project";
    std::filesystem::create_directories(cargo / "src");
    write_text(cargo / "Cargo.toml", "[package]\nname = \"fixture\"\nversion = \"0.1.0\"\n");
    const auto cargo_suggestion = mint::suggest_project_policy(cargo);
    MINT_EXPECT(cargo_suggestion.project_kind == "cargo" &&
                    cargo_suggestion.policy.at("recipes").size() == 2,
                "Cargo project receives build and test recipes");

    const auto npm = root / "npm-project";
    std::filesystem::create_directories(npm / "src");
    write_text(npm / "package.json", R"({"scripts":{"build":"vite build","test":"node test.js"}})");
    const auto npm_suggestion = mint::suggest_project_policy(npm);
    MINT_EXPECT(npm_suggestion.project_kind == "npm" &&
                    npm_suggestion.policy.at("recipes").size() == 2 &&
                    npm_suggestion.policy.at("require_verification") == true,
                "npm project only exposes declared build and test scripts");

    const auto npm_read_only = root / "npm-read-only";
    std::filesystem::create_directories(npm_read_only);
    write_text(npm_read_only / "package.json", R"({"name":"fixture"})");
    const auto npm_read_only_suggestion = mint::suggest_project_policy(npm_read_only);
    MINT_EXPECT(npm_read_only_suggestion.project_kind == "npm-read-only" &&
                    npm_read_only_suggestion.policy.at("write_paths").empty() &&
                    npm_read_only_suggestion.policy.at("recipes").empty(),
                "npm manifest without supported scripts remains read-only");

    const auto npm_blank_scripts = root / "npm-blank-scripts";
    std::filesystem::create_directories(npm_blank_scripts);
    write_text(npm_blank_scripts / "package.json", R"({"scripts":{"build":"  \t","test":""}})");
    const auto npm_blank_suggestion = mint::suggest_project_policy(npm_blank_scripts);
    MINT_EXPECT(npm_blank_suggestion.project_kind == "npm-read-only" &&
                    npm_blank_suggestion.policy.at("recipes").empty(),
                "blank npm scripts cannot become verification recipes");

#if !defined(_WIN32)
    const auto linked_manifest = root / "linked-manifest-project";
    const auto external_manifest = root / "external-package.json";
    std::filesystem::create_directories(linked_manifest);
    write_text(external_manifest, R"({"scripts":{"test":"node test.js"}})");
    std::error_code manifest_symlink_error;
    std::filesystem::create_symlink(external_manifest, linked_manifest / "package.json",
                                    manifest_symlink_error);
    MINT_EXPECT(!manifest_symlink_error, "test setup creates a manifest symlink");
    const auto linked_manifest_suggestion = mint::suggest_project_policy(linked_manifest);
    MINT_EXPECT(linked_manifest_suggestion.project_kind == "generic-read-only" &&
                    linked_manifest_suggestion.policy.at("recipes").empty(),
                "project detection does not follow a symlinked build manifest");
#endif

    const auto generic = root / "generic-project";
    std::filesystem::create_directories(generic);
    const auto generic_suggestion = mint::suggest_project_policy(generic);
    MINT_EXPECT(generic_suggestion.project_kind == "generic-read-only" &&
                    generic_suggestion.policy.at("write_paths").empty() &&
                    generic_suggestion.policy.at("recipes").empty(),
                "unknown project starts read-only");
}

void test_project_and_task_store(const std::filesystem::path& root) {
    const auto workspace = root / "workspace";
    const auto state = root / "state";
    std::filesystem::create_directories(workspace / "src");
    write_text(workspace / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\n");

    expect_failure([&] { (void)mint::ProjectStore(workspace, workspace / ".state"); },
                   "managed state is rejected inside the workspace");
    expect_failure([&] { (void)mint::ProjectStore(workspace, root); },
                   "managed state root cannot be an ancestor of the workspace");

    const auto suggestion = mint::suggest_project_policy(workspace);
    mint::ProjectStore store(workspace, state);
    MINT_EXPECT(!store.initialized(), "new project store starts uninitialized");
    expect_failure([&] { store.initialize("invalid kind", suggestion.policy); },
                   "project kind has a bounded identifier contract");
    auto invalid_policy = suggestion.policy;
    invalid_policy["schema_version"] = 999;
    expect_failure([&] { store.initialize(suggestion.project_kind, invalid_policy); },
                   "project store validates a policy before persisting it");

#if !defined(_WIN32)
    const auto public_state = root / "public-state";
    std::filesystem::create_directories(public_state);
    std::filesystem::permissions(
        public_state, std::filesystem::perms::owner_all | std::filesystem::perms::group_read,
        std::filesystem::perm_options::replace);
    mint::ProjectStore public_store(workspace, public_state);
    expect_failure([&] { public_store.initialize(suggestion.project_kind, suggestion.policy); },
                   "existing public directory is rejected instead of silently chmodded");
    MINT_EXPECT((std::filesystem::status(public_state).permissions() &
                 std::filesystem::perms::group_read) != std::filesystem::perms::none,
                "rejected public state directory keeps its original permissions");

    const auto linked_state = root / "linked-state";
    const auto linked_projects_target = root / "linked-projects-target";
    std::filesystem::create_directories(linked_state);
    std::filesystem::permissions(linked_state, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    std::filesystem::create_directories(linked_projects_target);
    std::error_code projects_symlink_error;
    std::filesystem::create_directory_symlink(linked_projects_target, linked_state / "projects",
                                              projects_symlink_error);
    MINT_EXPECT(!projects_symlink_error, "test setup creates a projects symlink");
    mint::ProjectStore linked_store(workspace, linked_state);
    expect_failure([&] { linked_store.initialize(suggestion.project_kind, suggestion.policy); },
                   "state hierarchy rejects a substituted projects symlink");
#endif

    store.initialize(suggestion.project_kind, suggestion.policy);
    MINT_EXPECT(store.initialized(), "init persists a project profile and policy");
    MINT_EXPECT(store.load_profile().at("project_kind") == "cmake",
                "project profile is bound to the detected project kind");
    MINT_EXPECT(store.project_directory().parent_path().parent_path() ==
                    std::filesystem::weakly_canonical(state),
                "project state is kept under the explicit external state root");
    expect_private_permissions(store.project_directory(), "project state directory is private");
    expect_private_permissions(store.profile_path(), "project profile is private");
    expect_private_permissions(store.project_policy_path(), "project policy is private");
    expect_failure([&] { store.initialize(suggestion.project_kind, suggestion.policy); },
                   "reinitialization requires explicit force");
    expect_failure(
        [&] {
            (void)store.create_task(std::string{"\xC3\x28", 2});
        },
        "managed task text must be valid UTF-8 before JSON persistence");

    const auto original_policy = mint::load_task_policy(store.project_policy_path());
    const auto task = store.create_task("repair the fixture");
    MINT_EXPECT(task.directory.parent_path() == store.project_directory() / "tasks",
                "task state lives under the external project state directory");
    expect_private_permissions(task.directory, "task directory is private");
    expect_private_permissions(task.metadata, "task metadata is private");
    expect_private_permissions(task.policy, "task policy snapshot is private");
    MINT_EXPECT(mint::load_task_policy(task.policy).fingerprint == original_policy.fingerprint,
                "new task snapshots the current project policy");
    const auto original_metadata = mint::SessionStore(task.metadata).load();
    auto mismatched_metadata = original_metadata;
    mismatched_metadata["workspace_root"] = (root / "other-workspace").generic_string();
    mint::SessionStore(task.metadata).save(mismatched_metadata);
    expect_failure([&] { (void)store.task_summary(task.id); },
                   "task metadata remains bound to its initialized workspace");
    mint::SessionStore(task.metadata).save(original_metadata);
    MINT_EXPECT(store.list_tasks().size() == 1 && store.list_tasks().front().status == "created",
                "task is visible before its first agent checkpoint");
    MINT_EXPECT(!store.latest_resumable_task().has_value(),
                "task without a checkpoint is not treated as resumable");

    mint::SessionStore(task.session)
        .save({{"schema_version", mint::session_schema_version},
               {"workspace_root", store.workspace_root().generic_string()},
               {"status", "timed_out"},
               {"verification_status", "not_run"},
               {"turns", std::size_t{3}}});
    const auto summary = store.task_summary(task.id);
    MINT_EXPECT(summary.has_value() && summary->status == "timed_out" && summary->resumable &&
                    summary->turns == 3,
                "valid interrupted checkpoint becomes resumable");
    const auto resumable = store.latest_resumable_task();
    MINT_EXPECT(resumable.has_value() && resumable->id == task.id,
                "latest resumable task resolves to its isolated state files");

    auto changed_policy = suggestion.policy;
    changed_policy["max_turns"] = 9;
    store.initialize(suggestion.project_kind, changed_policy, true);
    MINT_EXPECT(mint::load_task_policy(store.project_policy_path()).fingerprint !=
                    original_policy.fingerprint,
                "forced init replaces the project policy for future tasks");
    MINT_EXPECT(mint::load_task_policy(task.policy).fingerprint == original_policy.fingerprint,
                "forced init does not mutate an existing task policy snapshot");
    const auto future_task = store.create_task("inspect the updated project policy");
    MINT_EXPECT(mint::load_task_policy(future_task.policy).fingerprint !=
                    original_policy.fingerprint,
                "tasks created after forced init receive the updated policy snapshot");
    MINT_EXPECT(store.list_tasks().front().id == future_task.id,
                "task IDs preserve creation order for latest-task discovery");
    const auto still_resumable = store.latest_resumable_task();
    MINT_EXPECT(still_resumable.has_value() && still_resumable->id == task.id,
                "latest resumable discovery skips a newer task without a checkpoint");
    const auto demo_task = store.create_task("offline inspection", mint::ManagedTaskMode::demo);
    mint::SessionStore(demo_task.session)
        .save({{"schema_version", mint::session_schema_version},
               {"workspace_root", store.workspace_root().generic_string()},
               {"status", "timed_out"},
               {"verification_status", "not_required"},
               {"turns", std::size_t{1}}});
    const auto demo_summary = store.task_summary(demo_task.id);
    MINT_EXPECT(demo_summary.has_value() && demo_summary->mode == mint::ManagedTaskMode::demo &&
                    !demo_summary->resumable,
                "interrupted offline demo cannot be resumed as a real model task");

    MINT_EXPECT(!store.task_summary("missing-task").has_value(), "missing task query is explicit");
    expect_failure([&] { (void)store.task_paths("../escape"); },
                   "task identifiers cannot traverse directories");

#if !defined(_WIN32)
    const auto replacement = root / "replacement-policy.json";
    mint::SessionStore(replacement).save(changed_policy);
    std::filesystem::remove(task.policy);
    std::error_code symlink_error;
    std::filesystem::create_symlink(replacement, task.policy, symlink_error);
    MINT_EXPECT(!symlink_error, "test setup replaces task policy with a symlink");
    expect_failure([&] { (void)store.task_paths(task.id); },
                   "task policy cannot be replaced by a symlink");
#endif
}

TEST(ProjectWorkflowContractTest, DetectsProjectAndSuggestsPolicy) {
    TemporaryDirectory temporary;
    test_project_detection(temporary.path());
}

TEST(ProjectWorkflowContractTest, PersistsProjectsTasksAndRecoveryState) {
    TemporaryDirectory temporary;
    test_project_and_task_store(temporary.path());
}

} // namespace

#undef MINT_EXPECT
