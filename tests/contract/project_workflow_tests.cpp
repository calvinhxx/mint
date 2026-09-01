#include "mint/application/project_service.hpp"
#include "mint/domain/task_policy.hpp"
#include "mint/infrastructure/project_store.hpp"
#include "mint/infrastructure/session_store.hpp"
#include "mint/version.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#if defined(_WIN32)
// Windows SDK base types must be declared before aclapi.h.
#include <windows.h>

#include <aclapi.h>
#endif

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
        for (int attempt = 0; attempt < 16; ++attempt) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = std::filesystem::temp_directory_path() /
                    ("mint-v1-3-tests-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directories(path_)) {
                return;
            }
        }
        throw std::runtime_error("could not create temporary test directory");
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

#if defined(_WIN32)

struct LocalMemoryDeleter {
    void operator()(void* value) const noexcept {
        if (value != nullptr) {
            (void)LocalFree(value);
        }
    }
};

using LocalMemory = std::unique_ptr<void, LocalMemoryDeleter>;

std::vector<unsigned char> world_sid() {
    DWORD bytes = SECURITY_MAX_SID_SIZE;
    std::vector<unsigned char> sid(bytes);
    if (CreateWellKnownSid(WinWorldSid, nullptr, sid.data(), &bytes) == 0) {
        throw std::runtime_error("could not create Everyone SID");
    }
    sid.resize(bytes);
    return sid;
}

void grant_legacy_shared_access(const std::filesystem::path& path) {
    PACL existing_acl = nullptr;
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    const auto query = GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                                             DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                             &existing_acl, nullptr, &raw_descriptor);
    LocalMemory descriptor(raw_descriptor);
    if (query != ERROR_SUCCESS || existing_acl == nullptr) {
        throw std::runtime_error("could not read directory ACL");
    }

    auto everyone = world_sid();
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone.data());

    PACL raw_updated_acl = nullptr;
    const auto create = SetEntriesInAclW(1, &entry, existing_acl, &raw_updated_acl);
    LocalMemory updated_acl(raw_updated_acl);
    if (create != ERROR_SUCCESS || raw_updated_acl == nullptr) {
        throw std::runtime_error("could not create legacy directory ACL");
    }
    const auto apply =
        SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, raw_updated_acl, nullptr);
    if (apply != ERROR_SUCCESS) {
        throw std::runtime_error("could not apply legacy directory ACL");
    }
}

bool grants_legacy_shared_access(const std::filesystem::path& path) {
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    const auto query = GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                                             DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl,
                                             nullptr, &raw_descriptor);
    LocalMemory descriptor(raw_descriptor);
    if (query != ERROR_SUCCESS || acl == nullptr) {
        throw std::runtime_error("could not inspect directory ACL");
    }

    ACL_SIZE_INFORMATION size{};
    if (GetAclInformation(acl, &size, sizeof(size), AclSizeInformation) == 0) {
        throw std::runtime_error("could not inspect directory ACEs");
    }
    const auto everyone = world_sid();
    for (DWORD index = 0; index < size.AceCount; ++index) {
        void* raw_ace = nullptr;
        if (GetAce(acl, index, &raw_ace) == 0) {
            throw std::runtime_error("could not read directory ACE");
        }
        const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            continue;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
        if (EqualSid(const_cast<DWORD*>(&ace->SidStart),
                     const_cast<unsigned char*>(everyone.data())) != 0) {
            return true;
        }
    }
    return false;
}

std::vector<std::filesystem::path> managed_state_directories(const mint::ProjectStore& store) {
    std::vector<std::filesystem::path> paths = {store.state_root(), store.state_root() / "projects",
                                                store.project_directory(),
                                                store.project_directory() / "tasks"};
    for (const auto& entry :
         std::filesystem::directory_iterator(store.project_directory() / "tasks")) {
        if (entry.is_directory()) {
            paths.push_back(entry.path());
        }
    }
    return paths;
}

void make_legacy_state_shared(const mint::ProjectStore& store) {
    for (const auto& path : managed_state_directories(store)) {
        grant_legacy_shared_access(path);
    }
}

void expect_migrated_state_private(const mint::ProjectStore& store) {
    for (const auto& path : managed_state_directories(store)) {
        EXPECT_FALSE(grants_legacy_shared_access(path)) << path.generic_string();
    }
}

#endif

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
    MINT_EXPECT(
        cmake_suggestion.policy.at("tool_limits").at("read_file_bytes") == 16 * 1024 &&
            cmake_suggestion.policy.at("tool_limits").at("search_max_files") == 2000 &&
            cmake_suggestion.policy.at("tool_limits").at("command_resources").at("cpu_seconds") ==
                mint::runtime_defaults::managed_command_cpu_seconds &&
            cmake_suggestion.policy.at("tool_limits").at("command_resources").at("max_processes") ==
                mint::runtime_defaults::managed_command_max_processes &&
            cmake_suggestion.policy.at("tool_limits")
                    .at("command_resources")
                    .at("workspace_disk_bytes") ==
                mint::runtime_defaults::managed_command_workspace_disk_bytes,
        "generated policy exposes finite tool and command budgets");

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
    MINT_EXPECT(generic_suggestion.policy.at("max_total_tokens") ==
                    mint::runtime_defaults::managed_max_total_tokens,
                "new managed projects start with a finite cumulative token safety limit");
    MINT_EXPECT(!generic_suggestion.policy.at("tool_limits").contains("command_resources"),
                "read-only projects do not pretend to enforce unused command limits");
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
    const std::string invalid_task_text{"\xC3\x28", 2};
    expect_failure([&] { (void)store.create_task(invalid_task_text); },
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

    const auto failed_task = store.create_task("unsafe workspace mutation");
    mint::SessionStore(failed_task.session)
        .save({{"schema_version", mint::session_schema_version},
               {"workspace_root", store.workspace_root().generic_string()},
               {"status", "failed"},
               {"verification_status", "not_run"},
               {"turns", std::size_t{1}}});
    const auto failed_summary = store.task_summary(failed_task.id);
    MINT_EXPECT(failed_summary.has_value() && failed_summary->status == "failed" &&
                    !failed_summary->completed && !failed_summary->resumable,
                "failed workspace-integrity task is a non-resumable terminal state");
    const auto tasks_after_failure = store.list_tasks();
    const auto listed_failure =
        std::find_if(tasks_after_failure.begin(), tasks_after_failure.end(),
                     [&](const auto& item) { return item.id == failed_task.id; });
    MINT_EXPECT(listed_failure != tasks_after_failure.end() && listed_failure->status == "failed" &&
                    !listed_failure->completed && !listed_failure->resumable,
                "task listing preserves failed without treating the project state as corrupt");

    const auto exhausted_task = store.create_task("exhaust the cumulative token budget");
    mint::SessionStore(exhausted_task.session)
        .save({{"schema_version", mint::session_schema_version},
               {"workspace_root", store.workspace_root().generic_string()},
               {"status", "budget_exhausted"},
               {"verification_status", "not_required"},
               {"turns", std::size_t{2}}});
    const auto exhausted_summary = store.task_summary(exhausted_task.id);
    MINT_EXPECT(exhausted_summary.has_value() && exhausted_summary->status == "budget_exhausted" &&
                    !exhausted_summary->completed && !exhausted_summary->resumable,
                "an exhausted task token budget is a stable non-resumable terminal state");

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

TEST(ProjectWorkflowContractTest, MigratesOwnedV14StateBeforeEveryManagedOperation) {
#if !defined(_WIN32)
    GTEST_SKIP() << "Windows ACL compatibility gate";
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "legacy-workspace";
    const auto state = temporary.path() / "legacy-state";
    std::filesystem::create_directories(workspace / "src");
    write_text(workspace / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\n");

    const auto suggestion = mint::suggest_project_policy(workspace);
    const auto unrecognized_state = temporary.path() / "unrecognized-shared-state";
    std::filesystem::create_directories(unrecognized_state);
    grant_legacy_shared_access(unrecognized_state);
    mint::ProjectStore unrecognized_store(workspace, unrecognized_state);
    EXPECT_ANY_THROW(
        unrecognized_store.initialize(suggestion.project_kind, suggestion.policy, true));
    EXPECT_TRUE(grants_legacy_shared_access(unrecognized_state));

    mint::ProjectStore seed(workspace, state);
    seed.initialize(suggestion.project_kind, suggestion.policy);
    const auto resumable = seed.create_task("resume legacy task");
    mint::SessionStore(resumable.session)
        .save({{"schema_version", mint::session_schema_version},
               {"workspace_root", seed.workspace_root().generic_string()},
               {"status", "timed_out"},
               {"verification_status", "not_run"},
               {"turns", std::size_t{2}}});

    make_legacy_state_shared(seed);
    mint::ProjectStore status_store(workspace, state);
    const auto summaries = status_store.list_tasks();
    ASSERT_EQ(summaries.size(), 1U);
    EXPECT_TRUE(summaries.front().resumable);
    expect_migrated_state_private(status_store);

    make_legacy_state_shared(status_store);
    mint::ProjectStore resume_store(workspace, state);
    const auto latest = resume_store.latest_resumable_task();
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->id, resumable.id);
    expect_migrated_state_private(resume_store);

    make_legacy_state_shared(resume_store);
    mint::ProjectStore run_store(workspace, state);
    EXPECT_NO_THROW((void)run_store.create_task("run after legacy migration"));
    expect_migrated_state_private(run_store);

    make_legacy_state_shared(run_store);
    mint::ProjectStore init_store(workspace, state);
    EXPECT_NO_THROW(init_store.initialize(suggestion.project_kind, suggestion.policy, true));
    expect_migrated_state_private(init_store);
#endif
}

} // namespace

#undef MINT_EXPECT
