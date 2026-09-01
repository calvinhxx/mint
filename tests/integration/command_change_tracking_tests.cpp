#include "mint/application/agent.hpp"
#include "mint/infrastructure/session_store.hpp"
#include "mint/tools/tool_registry.hpp"
#include "mint/version.hpp"

#include "agent/agent_execution.hpp"
#include "changes/workspace_change_tracker.hpp"
#include "test_executable.hpp"
#include "test_workspace.hpp"
#include "workspace/path_identity.hpp"
#include "workspace/workspace_support.hpp"

#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using mint::test::read_text;
using mint::test::TemporaryDirectory;
using mint::test::write_text;

mint::ToolRegistryOptions command_options(const std::string& program, bool allow_write = false,
                                          std::vector<std::filesystem::path> write_paths = {}) {
    return {.allow_write = allow_write,
            .allowed_write_paths = std::move(write_paths),
            .allowed_programs = {program},
            .default_command_timeout_seconds = 5,
            .max_command_timeout_seconds = 5};
}

mint::Json helper_arguments(const std::string& program,
                            std::initializer_list<std::string> arguments) {
    auto command_arguments = mint::Json::array({"--command-helper"});
    for (const auto& argument : arguments) {
        command_arguments.push_back(argument);
    }
    return {{"program", program}, {"args", std::move(command_arguments)}, {"timeout_seconds", 5}};
}

mint::ToolCall helper_call(std::string id, const std::string& program,
                           std::initializer_list<std::string> arguments) {
    return {std::move(id), "run_command", helper_arguments(program, arguments)};
}

mint::Json run_helper(mint::ToolRegistry& tools, std::string id, const std::string& program,
                      std::initializer_list<std::string> arguments) {
    return mint::Json::parse(tools.execute(helper_call(std::move(id), program, arguments)));
}

#if defined(__APPLE__)
mint::Json run_recipe(mint::ToolRegistry& tools, std::string id, std::string recipe) {
    return mint::Json::parse(
        tools.execute({std::move(id), "run_recipe", {{"recipe", std::move(recipe)}}}));
}
#endif

bool is_verification_ready(const mint::Json& message) {
    return message.value("role", "") == "user" && message.contains("content") &&
           message.at("content").is_string() &&
           message.at("content").get<std::string>().find("[Harness status]") != std::string::npos;
}

mint::Json latest_tool_result(const mint::Json& messages) {
    const auto offset = is_verification_ready(messages.back()) ? std::size_t{1} : std::size_t{0};
    const auto& message = messages.at(messages.size() - 1 - offset);
    EXPECT_EQ(message.value("role", ""), "tool");
    return mint::Json::parse(message.at("content").get<std::string>());
}

mint::ModelReply model_tool_reply(std::string id, std::string name, mint::Json arguments) {
    mint::Json raw_call = {{"id", id},
                           {"type", "function"},
                           {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
    mint::ModelReply reply;
    reply.assistant_message = {
        {"role", "assistant"}, {"content", nullptr}, {"tool_calls", mint::Json::array({raw_call})}};
    reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
    return reply;
}

class CommandWriteThenVerifyModel final : public mint::ModelClient {
  public:
    explicit CommandWriteThenVerifyModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json& messages, const mint::Json&) override {
        switch (calls_++) {
        case 0:
            return model_tool_reply("command-write", "run_command",
                                    helper_arguments(program_, {"write", "src/main.cpp"}));
        case 1: {
            const auto result = latest_tool_result(messages);
            EXPECT_EQ(result.at("exit_code"), 0);
            EXPECT_TRUE(result.at("workspace_changed").get<bool>());
            EXPECT_FALSE(result.at("verification_eligible").get<bool>());
            EXPECT_EQ(result.at("changed_files").size(), 1);
            return {.assistant_message = {{"role", "assistant"}, {"content", "过早结束"}},
                    .text = "过早结束"};
        }
        case 2:
            EXPECT_EQ(messages.back().value("role", ""), "user");
            EXPECT_NE(messages.back().at("content").get<std::string>().find("not_run"),
                      std::string::npos);
            gate_seen_ = true;
            return model_tool_reply(
                "command-verify", "run_command",
                helper_arguments(program_, {"verify", "src/main.cpp", "sandbox write probe\n"}));
        default:
            EXPECT_TRUE(is_verification_ready(messages.back()));
            return {.assistant_message = {{"role", "assistant"}, {"content", "命令修改已验证"}},
                    .text = "命令修改已验证"};
        }
    }

    [[nodiscard]] bool gate_seen() const noexcept {
        return gate_seen_;
    }

  private:
    std::string program_;
    int calls_ = 0;
    bool gate_seen_ = false;
};

class PolicyViolationModel final : public mint::ModelClient {
  public:
    explicit PolicyViolationModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        EXPECT_EQ(calls_++, 0);
        return model_tool_reply("outside-write", "run_command",
                                helper_arguments(program_, {"write", "README.md"}));
    }

  private:
    std::string program_;
    int calls_ = 0;
};

class InvalidFilenameModel final : public mint::ModelClient {
  public:
    explicit InvalidFilenameModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        EXPECT_EQ(calls_++, 0);
        return model_tool_reply("invalid-filename", "run_command",
                                helper_arguments(program_, {"write-invalid-name"}));
    }

  private:
    std::string program_;
    int calls_ = 0;
};

TEST(WorkspaceChangeTrackerTest, RejectsUnboundedSnapshots) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "src" / "one.cpp", "one\n");
    write_text(workspace / "src" / "two.cpp", "two\n");
    const auto include_all = [](const std::filesystem::path&) { return true; };

    mint::tools::detail::WorkspaceSnapshotLimits entry_limit;
    entry_limit.max_entries = 2;
    EXPECT_THROW(
        (void)mint::tools::detail::capture_workspace_snapshot(workspace, include_all, entry_limit),
        std::runtime_error);

    mint::tools::detail::WorkspaceSnapshotLimits byte_limit;
    byte_limit.max_bytes = 1;
    byte_limit.max_text_bytes = 1;
    EXPECT_THROW(
        (void)mint::tools::detail::capture_workspace_snapshot(workspace, include_all, byte_limit),
        std::runtime_error);
}

TEST(PathIdentityTest, ExistingCaseAliasesCannotBypassProtectedOrIgnoredPaths) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto protected_config = workspace / "config.json";
    const auto config_alias = workspace / "CONFIG.JSON";
    const auto metadata = workspace / ".git";
    const auto metadata_alias = workspace / ".GIT";
    write_text(protected_config, "secret\n");
    write_text(metadata / "config", "token\n");

    std::error_code error;
    const bool config_alias_exists =
        std::filesystem::equivalent(protected_config, config_alias, error) && !error;
    error.clear();
    const bool metadata_alias_exists =
        std::filesystem::equivalent(metadata, metadata_alias, error) && !error;
    if (!config_alias_exists || !metadata_alias_exists) {
        GTEST_SKIP() << "The test directory is on a case-sensitive filesystem";
    }

    mint::ToolRegistry tools(
        workspace,
        mint::ToolRegistryOptions{.protected_paths = {protected_config}, .allow_write = true});
    const auto read_alias = mint::Json::parse(
        tools.execute({"read-protected-alias", "read_file", {{"path", "CONFIG.JSON"}}}));
    EXPECT_FALSE(read_alias.at("ok").get<bool>());

    const auto patch_alias = mint::Json::parse(tools.execute({"patch-ignored-alias",
                                                              "apply_patch",
                                                              {{"path", ".GIT/config"},
                                                               {"operation", "replace"},
                                                               {"old_text", "token\n"},
                                                               {"new_text", "changed\n"}}}));
    EXPECT_FALSE(patch_alias.at("ok").get<bool>());
    EXPECT_EQ(read_text(metadata / "config"), "token\n");

    const auto changeset_alias = mint::Json::parse(
        tools.execute({"changeset-ignored-alias",
                       "apply_changeset",
                       {{"changes", mint::Json::array({{{"operation", "create"},
                                                        {"path", ".GIT/new.txt"},
                                                        {"new_text", "should not exist\n"}}})}}}));
    EXPECT_FALSE(changeset_alias.at("ok").get<bool>());
    EXPECT_FALSE(std::filesystem::exists(metadata / "new.txt"));

    write_text(workspace / "src" / "existing.txt", "before\n");
    mint::ToolRegistry scoped_tools(
        workspace, mint::ToolRegistryOptions{.allow_write = true,
                                             .allowed_write_paths = {"src/existing.txt"}});
    const auto scoped_alias = mint::Json::parse(scoped_tools.execute({"patch-write-scope-alias",
                                                                      "apply_patch",
                                                                      {{"path", "SRC/existing.txt"},
                                                                       {"operation", "replace"},
                                                                       {"old_text", "before\n"},
                                                                       {"new_text", "after\n"}}}));
    EXPECT_TRUE(scoped_alias.at("ok").get<bool>());
    EXPECT_EQ(read_text(workspace / "src" / "existing.txt"), "after\n");

    EXPECT_TRUE(
        mint::tools::detail::contains_ignored_component(workspace, workspace / ".GIT" / "config"));
    const auto include_visible = [&](const std::filesystem::path& path) {
        return !mint::tools::detail::contains_ignored_component(workspace, path);
    };
    const auto snapshot =
        mint::tools::detail::capture_workspace_snapshot(workspace, include_visible);
    EXPECT_FALSE(snapshot.contains(".git"));
    EXPECT_FALSE(snapshot.contains(".git/config"));
    EXPECT_FALSE(snapshot.contains(".GIT"));
    EXPECT_FALSE(snapshot.contains(".GIT/config"));
}

TEST(PathIdentityTest, DistinctCaseRemainsVisibleOnCaseSensitiveFilesystems) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto metadata = workspace / ".git";
    const auto distinct = workspace / ".GIT";
    write_text(metadata / "config", "ignored\n");
    write_text(distinct / "visible.txt", "visible\n");

    std::error_code error;
    if (std::filesystem::equivalent(metadata, distinct, error) && !error) {
        GTEST_SKIP() << "The test directory is on a case-insensitive filesystem";
    }

    EXPECT_FALSE(
        mint::tools::detail::contains_ignored_component(workspace, distinct / "visible.txt"));
    const auto include_visible = [&](const std::filesystem::path& path) {
        return !mint::tools::detail::contains_ignored_component(workspace, path);
    };
    const auto snapshot =
        mint::tools::detail::capture_workspace_snapshot(workspace, include_visible);
    EXPECT_TRUE(snapshot.contains(".GIT"));
    EXPECT_TRUE(snapshot.contains(".GIT/visible.txt"));
    EXPECT_FALSE(snapshot.contains(".git"));
}

TEST(CommandRunnerTest, TracksChangesWithoutExposingWriteTools) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = mint_test_executable_path().generic_string();
    write_text(workspace / "src" / "main.cpp", "before\n");

    mint::ToolRegistry tools(workspace, command_options(program));
    EXPECT_FALSE(tools.can_write());
    EXPECT_EQ(tools.definitions().size(), 4);
    const auto hidden_journal = mint::Json::parse(
        tools.execute({"hidden-workspace-changes", "workspace_changes", mint::Json::object()}));
    EXPECT_FALSE(hidden_journal.at("ok").get<bool>());

    const auto result =
        run_helper(tools, "command-only-source-write", program, {"write", "src/main.cpp"});
    ASSERT_EQ(result.at("exit_code"), 0) << result.dump(2);
    EXPECT_TRUE(result.at("workspace_changed").get<bool>());
    EXPECT_FALSE(result.at("verification_eligible").get<bool>());
    EXPECT_TRUE(tools.has_workspace_changes());
    EXPECT_EQ(tools.workspace_change_snapshot().at("changed_files").at(0).at("path"),
              "src/main.cpp");

    mint::ToolRegistry restored(workspace, command_options(program));
    EXPECT_NO_THROW(restored.restore_workspace_change_state(tools.workspace_change_state()));
    EXPECT_TRUE(restored.has_workspace_changes());
    EXPECT_EQ(restored.workspace_change_snapshot().at("changed_files").at(0).at("path"),
              "src/main.cpp");
}

TEST(CommandRunnerTest, FailsClosedForUnauditableWorkspaceChanges) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = mint_test_executable_path().generic_string();
    mint::ToolRegistry tools(workspace, command_options(program));
    const auto call =
        helper_call("command-binary-write", program, {"write-binary", "src/data.bin"});

    const auto raw_result = tools.execute(call);
    const auto result = mint::Json::parse(raw_result);
    EXPECT_FALSE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("status"), "unauditable_workspace_change");
    EXPECT_TRUE(result.at("workspace_changed").get<bool>());
    EXPECT_FALSE(result.at("verification_eligible").get<bool>());
    ASSERT_EQ(result.at("changed_files").size(), 1);
    EXPECT_EQ(result.at("changed_files").at(0).at("path"), "src/data.bin");
    EXPECT_EQ(result.at("changed_files").at(0).at("status"), "unauditable");
    EXPECT_TRUE(tools.has_workspace_changes());
    EXPECT_TRUE(tools.workspace_integrity_failed());
    EXPECT_EQ(tools.workspace_change_state().at("schema_version"),
              mint::workspace_change_schema_version);

    mint::ExecutionSummary execution;
    mint::agent_detail::record_execution(execution, call, raw_result);
    EXPECT_EQ(execution.file_changes, 1);
    EXPECT_EQ(execution.last_file_change_call, 1);
    EXPECT_EQ(execution.last_command_call, 1);
    EXPECT_FALSE(execution.last_command_verification_eligible);
    EXPECT_EQ(mint::agent_detail::verification_status(execution, tools.has_workspace_changes()),
              "not_run");

    const auto blocked =
        run_helper(tools, "command-after-binary-write", program, {"echo", "must-not-run"});
    EXPECT_EQ(blocked.at("status"), "workspace_tracking_failed");
    EXPECT_FALSE(blocked.at("verification_eligible").get<bool>());

    mint::ToolRegistry restored(workspace, command_options(program));
    EXPECT_NO_THROW(restored.restore_workspace_change_state(tools.workspace_change_state()));
    EXPECT_TRUE(restored.has_workspace_changes());
    EXPECT_EQ(restored.workspace_change_snapshot().at("changed_files").at(0).at("path"),
              "src/data.bin");

    auto downgraded = tools.workspace_change_state();
    downgraded["schema_version"] = 2;
    mint::ToolRegistry downgrade_target(workspace, command_options(program));
    EXPECT_THROW(downgrade_target.restore_workspace_change_state(downgraded),
                 std::invalid_argument);

    mint::ToolRegistry legacy_target(workspace, command_options(program));
    EXPECT_NO_THROW(legacy_target.restore_workspace_change_state(
        {{"schema_version", 2}, {"entries", mint::Json::array()}}));
}

TEST(CommandRunnerTest, DetectsSameSizeBinaryReplacementWithoutHashing) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = mint_test_executable_path().generic_string();
    write_text(workspace / "src" / "data.bin", std::string("\x00\x01\x02\x03", 4));
    mint::ToolRegistry tools(workspace, command_options(program, true, {"src"}));

    const auto result =
        run_helper(tools, "command-binary-replace", program, {"write-binary", "src/data.bin"});

    ASSERT_EQ(result.at("exit_code"), 0) << result.dump(2);
    EXPECT_FALSE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("status"), "unauditable_workspace_change");
    EXPECT_TRUE(result.at("workspace_changed").get<bool>());
    ASSERT_EQ(result.at("changed_files").size(), 1);
    EXPECT_EQ(result.at("changed_files").at(0).at("path"), "src/data.bin");
    EXPECT_EQ(result.at("changed_files").at(0).at("status"), "unauditable");
}

TEST(CommandRunnerTest, FailsClosedForSymlinkPointingAtProtectedFile) {
#if defined(_WIN32)
    GTEST_SKIP() << "Symbolic-link creation requires additional Windows privileges";
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto protected_session = workspace / "session.json";
    const auto program = mint_test_executable_path().generic_string();
    write_text(protected_session, "{}\n");
    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.protected_paths = {protected_session},
                                                       .allow_write = true,
                                                       .allowed_write_paths = {"src"},
                                                       .allowed_programs = {program},
                                                       .default_command_timeout_seconds = 5,
                                                       .max_command_timeout_seconds = 5});

    const auto result = run_helper(tools, "command-protected-symlink", program,
                                   {"create-symlink", "../session.json", "src/session-link"});

    ASSERT_EQ(result.at("exit_code"), 0) << result.dump(2);
    EXPECT_FALSE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("status"), "unauditable_workspace_change");
    EXPECT_TRUE(result.at("workspace_changed").get<bool>());
    EXPECT_FALSE(result.at("verification_eligible").get<bool>());
    ASSERT_EQ(result.at("changed_files").size(), 1);
    EXPECT_EQ(result.at("changed_files").at(0).at("path"), "src/session-link");
    EXPECT_EQ(result.at("changed_files").at(0).at("status"), "unauditable");
    EXPECT_TRUE(tools.workspace_integrity_failed());
#endif
}

TEST(CommandRunnerTest, RefusesCommandWhenProtectedFileHasAdditionalHardLink) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto protected_config = workspace / "config.json";
    const auto alias = workspace / "build" / "config-alias.json";
    const auto program = mint_test_executable_path().generic_string();
    write_text(protected_config, "secret\n");
    std::filesystem::create_directories(alias.parent_path());
    std::error_code link_error;
    std::filesystem::create_hard_link(protected_config, alias, link_error);
    if (link_error) {
        GTEST_SKIP() << "The test filesystem does not support hard links";
    }
    ASSERT_TRUE(mint::tools::detail::same_path_identity(protected_config, alias));
    ASSERT_FALSE(mint::tools::detail::same_path_entry_identity(protected_config, alias));

    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.protected_paths = {protected_config},
                                                       .allowed_programs = {program},
                                                       .default_command_timeout_seconds = 5,
                                                       .max_command_timeout_seconds = 5});
    const auto result = run_helper(tools, "command-protected-hardlink", program,
                                   {"write", "build/config-alias.json"});

    EXPECT_FALSE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("status"), "workspace_tracking_failed");
    EXPECT_TRUE(result.at("workspace_changed").get<bool>());
    EXPECT_FALSE(result.at("verification_eligible").get<bool>());
    EXPECT_FALSE(result.contains("exit_code"));
    EXPECT_EQ(read_text(protected_config), "secret\n");
    EXPECT_TRUE(tools.workspace_integrity_failed());
    EXPECT_EQ(tools.workspace_change_snapshot().at("changed_files").at(0).at("path"),
              "<workspace>");
}

TEST(CommandRunnerTest, AllowsCommandsWhenOptionalProtectedFilesDoNotExist) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = mint_test_executable_path().generic_string();
    auto options = command_options(program);
    options.protected_paths = {temporary.path() / "runtime" / "pending-transaction.json"};
    mint::ToolRegistry tools(workspace, std::move(options));

    const auto result =
        run_helper(tools, "command-missing-protected-file", program, {"echo", "unchanged"});

    EXPECT_TRUE(result.at("ok").get<bool>()) << result.dump(2);
    EXPECT_EQ(result.at("exit_code"), 0) << result.dump(2);
    EXPECT_FALSE(result.value("workspace_changed", false)) << result.dump(2);
    EXPECT_FALSE(tools.workspace_integrity_failed());
}

TEST(CommandRunnerTest, HardLinkAliasDoesNotInheritExactFileWriteScope) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto readme = workspace / "README.md";
    const auto allowed = workspace / "src" / "allowed.txt";
    const auto program = mint_test_executable_path().generic_string();
    write_text(readme, "before\n");
    std::error_code link_error;
    std::filesystem::create_hard_link(readme, allowed, link_error);
    if (link_error) {
        GTEST_SKIP() << "The test filesystem does not support hard links";
    }

    mint::ToolRegistry patch_tools(
        workspace,
        mint::ToolRegistryOptions{.allow_write = true, .allowed_write_paths = {"src/allowed.txt"}});
    const auto patch = mint::Json::parse(patch_tools.execute({"patch-hardlink-outside-scope",
                                                              "apply_patch",
                                                              {{"path", "README.md"},
                                                               {"operation", "replace"},
                                                               {"old_text", "before\n"},
                                                               {"new_text", "after\n"}}}));
    EXPECT_FALSE(patch.at("ok").get<bool>());
    EXPECT_EQ(read_text(readme), "before\n");
    EXPECT_EQ(read_text(allowed), "before\n");

    mint::ToolRegistry two_explicit_scopes(
        workspace,
        mint::ToolRegistryOptions{.allow_write = true,
                                  .allowed_write_paths = {"README.md", "src/allowed.txt"}});
    EXPECT_EQ(two_explicit_scopes.allowed_write_paths().size(), 2);

    mint::ToolRegistry command_tools(workspace,
                                     command_options(program, true, {"src/allowed.txt"}));
    const auto result = run_helper(command_tools, "command-hardlink-outside-scope", program,
                                   {"write", "README.md"});
    ASSERT_EQ(result.at("exit_code"), 0) << result.dump(2);
    EXPECT_FALSE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("status"), "policy_violation");
    EXPECT_TRUE(result.at("workspace_changed").get<bool>());
    EXPECT_TRUE(command_tools.workspace_integrity_failed());
    const auto violation = std::find_if(
        result.at("changed_files").begin(), result.at("changed_files").end(),
        [](const mint::Json& entry) {
            return entry.at("path") == "README.md" && entry.at("status") == "policy_violation";
        });
    EXPECT_NE(violation, result.at("changed_files").end());
}

TEST(CommandRunnerTest, FailsClosedForPermissionOnlyChanges) {
#if defined(_WIN32)
    GTEST_SKIP() << "POSIX mode bits are not available on Windows";
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = mint_test_executable_path().generic_string();
    const auto script = workspace / "src" / "script.sh";
    write_text(script, "#!/bin/sh\n");
    std::filesystem::permissions(
        script,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read | std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace);
    mint::ToolRegistry tools(workspace, command_options(program, true, {"src"}));
    const auto patched =
        mint::Json::parse(tools.execute({"patch-before-chmod",
                                         "apply_patch",
                                         {{"path", "src/script.sh"},
                                          {"operation", "replace"},
                                          {"old_text", "#!/bin/sh\n"},
                                          {"new_text", "#!/bin/sh\necho ready\n"}}}));
    ASSERT_TRUE(patched.at("ok").get<bool>()) << patched.dump(2);

    const auto result =
        run_helper(tools, "command-chmod", program, {"chmod-executable", "src/script.sh"});

    ASSERT_EQ(result.at("exit_code"), 0) << result.dump(2);
    EXPECT_FALSE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("status"), "unauditable_workspace_change");
    EXPECT_TRUE(result.at("workspace_changed").get<bool>());
    EXPECT_FALSE(result.at("verification_eligible").get<bool>());
    ASSERT_EQ(result.at("changed_files").size(), 1);
    EXPECT_EQ(result.at("changed_files").at(0).at("path"), "src/script.sh");
    EXPECT_EQ(result.at("changed_files").at(0).at("status"), "unauditable");
    EXPECT_TRUE(tools.workspace_integrity_failed());
    const auto changes = tools.workspace_change_snapshot();
    ASSERT_EQ(changes.at("changed_files").size(), 1);
    EXPECT_EQ(changes.at("changed_files").at(0).at("path"), "src/script.sh");
    EXPECT_EQ(changes.at("changed_files").at(0).at("status"), "unauditable");
#endif
}

TEST(CommandRunnerTest, AllowsManagedDirectoryCreationWithTextFile) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = mint_test_executable_path().generic_string();
    mint::ToolRegistry tools(workspace, command_options(program, true, {"src"}));

    const auto result = run_helper(tools, "command-create-managed-directory", program,
                                   {"write-with-parent", "src/generated/main.cpp"});

    ASSERT_EQ(result.at("exit_code"), 0) << result.dump(2);
    EXPECT_TRUE(result.at("ok").get<bool>()) << result.dump(2);
    EXPECT_TRUE(result.at("workspace_changed").get<bool>());
    EXPECT_FALSE(result.at("verification_eligible").get<bool>());
    ASSERT_EQ(result.at("changed_files").size(), 1);
    EXPECT_EQ(result.at("changed_files").at(0).at("path"), "src/generated/main.cpp");
    EXPECT_EQ(result.at("changed_files").at(0).at("status"), "created");
    EXPECT_FALSE(tools.workspace_integrity_failed());
}

TEST(CommandRunnerTest, FailsClosedForDirectoryStructureChangesOutsidePolicy) {
    const auto program = mint_test_executable_path().generic_string();
    {
        TemporaryDirectory temporary;
        const auto workspace = temporary.path() / "workspace";
        mint::ToolRegistry tools(workspace, command_options(program, true, {"src"}));
        const auto created = run_helper(tools, "command-create-directory", program,
                                        {"create-directory", "outside-empty"});
        ASSERT_EQ(created.at("exit_code"), 0) << created.dump(2);
        EXPECT_EQ(created.at("status"), "policy_violation");
        EXPECT_EQ(created.at("changed_files").at(0).at("path"), "outside-empty");
    }
    {
        TemporaryDirectory temporary;
        const auto workspace = temporary.path() / "workspace";
        std::filesystem::create_directory(workspace / "outside-empty");
        mint::ToolRegistry tools(workspace, command_options(program, true, {"src"}));
        const auto deleted = run_helper(tools, "command-delete-directory", program,
                                        {"delete-directory", "outside-empty"});
        ASSERT_EQ(deleted.at("exit_code"), 0) << deleted.dump(2);
        EXPECT_EQ(deleted.at("status"), "policy_violation");
        EXPECT_EQ(deleted.at("changed_files").at(0).at("path"), "outside-empty");
    }
}

TEST(CommandRunnerTest, JournalsManagedSourceChangesAndIgnoresBuildOutputs) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = mint_test_executable_path().generic_string();
    write_text(workspace / "src" / "main.cpp", "original\n");
    std::filesystem::create_directories(workspace / "build");
    mint::ToolRegistry tools(workspace, command_options(program, true));

    const auto patched = mint::Json::parse(tools.execute({"command-journal-patch",
                                                          "apply_patch",
                                                          {{"path", "src/main.cpp"},
                                                           {"operation", "replace"},
                                                           {"old_text", "original\n"},
                                                           {"new_text", "patched\n"}}}));
    ASSERT_TRUE(patched.at("ok").get<bool>()) << patched.dump(2);

    const auto build =
        run_helper(tools, "command-build-output", program, {"write", "build/artifact.txt"});
    EXPECT_EQ(build.at("exit_code"), 0) << build.dump(2);
    EXPECT_FALSE(build.value("workspace_changed", false));
    EXPECT_TRUE(build.at("verification_eligible").get<bool>());

    const auto source =
        run_helper(tools, "command-source-write", program, {"write", "src/main.cpp"});
    ASSERT_EQ(source.at("exit_code"), 0) << source.dump(2);
    ASSERT_TRUE(source.at("workspace_changed").get<bool>());
    EXPECT_FALSE(source.at("verification_eligible").get<bool>());
    ASSERT_EQ(source.at("changed_files").size(), 1);
    EXPECT_EQ(source.at("changed_files").at(0).at("path"), "src/main.cpp");
    EXPECT_EQ(source.at("changed_files").at(0).at("status"), "modified");

    const auto changes = tools.workspace_change_snapshot();
    ASSERT_EQ(changes.at("changed_files").size(), 1);
    EXPECT_EQ(changes.at("changed_files").at(0).at("path"), "src/main.cpp");
    EXPECT_NE(changes.at("diff").get<std::string>().find("-original\n+sandbox write probe\n"),
              std::string::npos);
    EXPECT_EQ(changes.at("diff").get<std::string>().find("patched"), std::string::npos);
    EXPECT_EQ(changes.at("diff").get<std::string>().find("build/artifact.txt"), std::string::npos);
}

TEST(CommandRunnerTest, RunsRepairFixtureRecipesAfterManagedEdits) {
#if !defined(__APPLE__)
    GTEST_SKIP() << "The live regression being guarded uses macOS Seatbelt";
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto fixture =
        std::filesystem::path{MINT_TEST_SOURCE_DIR} / "tests/fixtures/broken_cpp_project";
    std::filesystem::copy(fixture, workspace,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);

    const auto policy = mint::load_task_policy(workspace / "policy.json");
    const auto make_tools = [&](bool allow_write) {
        return mint::ToolRegistry(
            workspace,
            mint::ToolRegistryOptions{.protected_paths = {policy.source_path},
                                      .allow_write = allow_write,
                                      .allowed_write_paths =
                                          allow_write ? policy.write_paths
                                                      : std::vector<std::filesystem::path>{},
                                      .command_recipes = policy.recipes,
                                      .policy_fingerprint = policy.fingerprint,
                                      .require_command_sandbox = true,
                                      .runtime = policy.tool_limits});
    };

    {
        auto baseline = make_tools(false);
        const auto configure = run_recipe(baseline, "baseline-configure", "configure");
        ASSERT_EQ(configure.at("exit_code"), 0) << configure.dump(2);
        const auto build = run_recipe(baseline, "baseline-build", "build");
        ASSERT_EQ(build.at("exit_code"), 0) << build.dump(2);
        const auto test = run_recipe(baseline, "baseline-test", "test");
        EXPECT_NE(test.at("exit_code"), 0) << test.dump(2);
    }

    auto tools = make_tools(true);
    const auto source_patch =
        mint::Json::parse(tools.execute({"repair-source",
                                         "apply_patch",
                                         {{"path", "src/calculator.cpp"},
                                          {"operation", "replace"},
                                          {"old_text", "return left - right;"},
                                          {"new_text", "return left + right;"}}}));
    ASSERT_TRUE(source_patch.at("ok").get<bool>()) << source_patch.dump(2);
    const auto report_patch = mint::Json::parse(tools.execute(
        {"repair-report",
         "apply_patch",
         {{"path", "FIX_REPORT.md"},
          {"operation", "create"},
          {"new_text", "The implementation subtracted instead of adding. All tests pass.\n"}}}));
    ASSERT_TRUE(report_patch.at("ok").get<bool>()) << report_patch.dump(2);

    const auto configure = run_recipe(tools, "repair-configure", "configure");
    ASSERT_EQ(configure.at("exit_code"), 0) << configure.dump(2);
    EXPECT_FALSE(configure.value("workspace_changed", false)) << configure.dump(2);
    EXPECT_FALSE(tools.workspace_integrity_failed());

    const auto build = run_recipe(tools, "repair-build", "build");
    ASSERT_EQ(build.at("exit_code"), 0) << build.dump(2);
    EXPECT_FALSE(build.value("workspace_changed", false)) << build.dump(2);
    EXPECT_FALSE(tools.workspace_integrity_failed());

    const auto test = run_recipe(tools, "repair-test", "test");
    ASSERT_EQ(test.at("exit_code"), 0) << test.dump(2);
    EXPECT_TRUE(test.at("verification_eligible").get<bool>());
    EXPECT_FALSE(test.value("workspace_changed", false)) << test.dump(2);
    EXPECT_FALSE(tools.workspace_integrity_failed());
#endif
}

TEST(CommandRunnerTest, FailsClosedForWritesOutsidePolicyScope) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = mint_test_executable_path().generic_string();
    write_text(workspace / "README.md", "before\n");
    write_text(workspace / "src" / "main.cpp", "managed\n");
    mint::ToolRegistry tools(workspace, command_options(program, true, {"src"}));

    const auto unchanged =
        run_helper(tools, "command-policy-baseline", program, {"echo", "baseline"});
    EXPECT_EQ(unchanged.at("exit_code"), 0) << unchanged.dump(2);
    EXPECT_FALSE(unchanged.value("workspace_changed", false));
    EXPECT_FALSE(tools.has_workspace_changes());

    const auto violation =
        run_helper(tools, "command-policy-violation", program, {"write", "README.md"});
    EXPECT_EQ(violation.at("exit_code"), 0) << violation.dump(2);
    EXPECT_FALSE(violation.at("ok").get<bool>());
    EXPECT_EQ(violation.at("status"), "policy_violation");
    EXPECT_TRUE(violation.at("workspace_changed").get<bool>());
    EXPECT_FALSE(violation.at("verification_eligible").get<bool>());
    ASSERT_EQ(violation.at("changed_files").size(), 1);
    EXPECT_EQ(violation.at("changed_files").at(0).at("path"), "README.md");
    EXPECT_EQ(violation.at("changed_files").at(0).at("status"), "policy_violation");

    const auto changes = tools.workspace_change_snapshot();
    ASSERT_EQ(changes.at("changed_files").size(), 1);
    EXPECT_EQ(changes.at("changed_files").at(0).at("path"), "README.md");
    EXPECT_EQ(changes.at("changed_files").at(0).at("status"), "policy_violation");

    mint::ToolRegistry restored(workspace, command_options(program, true, {"src"}));
    EXPECT_NO_THROW(restored.restore_workspace_change_state(tools.workspace_change_state()));
    EXPECT_TRUE(restored.has_workspace_changes());
    EXPECT_EQ(restored.workspace_change_snapshot().at("changed_files").at(0).at("path"),
              "README.md");
}

TEST(AgentLoopTest, CommandCannotVerifyItsOwnWorkspaceChanges) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = mint_test_executable_path().generic_string();
    write_text(workspace / "src" / "main.cpp", "before\n");
    mint::ToolRegistry tools(workspace, command_options(program, true, {"src"}));
    CommandWriteThenVerifyModel model(program);
    std::ostringstream log;
    mint::Agent agent(model, tools, log,
                      mint::AgentOptions{.require_verification_after_write = true});

    const auto result = agent.run("通过命令修改源码并完成验证");

    EXPECT_TRUE(result.completed);
    EXPECT_EQ(result.answer, "命令修改已验证");
    EXPECT_TRUE(model.gate_seen());
    EXPECT_EQ(result.turns, 4);
    EXPECT_EQ(result.execution.file_changes, 1);
    EXPECT_EQ(result.execution.command_calls, 2);
    EXPECT_EQ(result.execution.verification_commands, 1);
    EXPECT_EQ(result.execution.commands_passed, 2);
    EXPECT_EQ(result.verification_status, "passed");
    EXPECT_EQ(result.changes.files, std::vector<std::string>{"src/main.cpp"});
    EXPECT_EQ(read_text(workspace / "src" / "main.cpp"), "sandbox write probe\n");
}

TEST(AgentLoopTest, WorkspacePolicyViolationIsAnImmediateTerminalFailure) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto session_path = temporary.path() / "session.json";
    const auto program = mint_test_executable_path().generic_string();
    write_text(workspace / "README.md", "before\n");
    mint::SessionStore session(session_path);
    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.protected_paths = {session_path},
                                                       .allow_write = true,
                                                       .allowed_write_paths = {"src"},
                                                       .allowed_programs = {program},
                                                       .default_command_timeout_seconds = 5,
                                                       .max_command_timeout_seconds = 5});
    PolicyViolationModel model(program);
    std::ostringstream log;
    mint::Agent agent(model, tools, log, {}, mint::AgentServices{.session_repository = &session});

    const auto result = agent.run("尝试修改策略范围外文件");

    EXPECT_FALSE(result.completed);
    EXPECT_EQ(result.status, "failed");
    EXPECT_EQ(result.stop_reason, "workspace_integrity_failed");
    EXPECT_EQ(result.turns, 1);
    ASSERT_EQ(result.changes.details.size(), 1);
    EXPECT_EQ(result.changes.details.at(0).path, "README.md");
    EXPECT_EQ(result.changes.details.at(0).status, "policy_violation");
    const auto machine = mint::agent_result_to_json(result);
    EXPECT_EQ(machine.at("changes").at("details").at(0).at("status"), "policy_violation");
    EXPECT_NE(log.str().find("[工作区风险] README.md：超出写入策略"), std::string::npos);
    EXPECT_EQ(session.load().at("status"), "failed");
    EXPECT_EQ(session.load().at("schema_version"), mint::session_schema_version);
    EXPECT_EQ(session.load().at("change_journal").at("schema_version"),
              mint::workspace_change_schema_version);

    const auto downgraded_path = temporary.path() / "downgraded-v4-session.json";
    mint::SessionStore downgraded_session(downgraded_path);
    auto downgraded_checkpoint = session.load();
    downgraded_checkpoint["schema_version"] = 4;
    downgraded_checkpoint["status"] = "running";
    downgraded_session.save(downgraded_checkpoint);
    mint::ToolRegistry downgraded_tools(
        workspace, mint::ToolRegistryOptions{.protected_paths = {downgraded_path},
                                             .allow_write = true,
                                             .allowed_write_paths = {"src"},
                                             .allowed_programs = {program},
                                             .default_command_timeout_seconds = 5,
                                             .max_command_timeout_seconds = 5});
    PolicyViolationModel downgraded_model(program);
    std::ostringstream downgraded_log;
    mint::Agent downgraded_agent(downgraded_model, downgraded_tools, downgraded_log,
                                 mint::AgentOptions{.resume_session = true},
                                 mint::AgentServices{.session_repository = &downgraded_session});
    EXPECT_THROW((void)downgraded_agent.run(""), std::invalid_argument);

    mint::ToolRegistry resume_tools(workspace,
                                    mint::ToolRegistryOptions{.protected_paths = {session_path},
                                                              .allow_write = true,
                                                              .allowed_write_paths = {"src"},
                                                              .allowed_programs = {program},
                                                              .default_command_timeout_seconds = 5,
                                                              .max_command_timeout_seconds = 5});
    PolicyViolationModel unused_model(program);
    std::ostringstream resume_log;
    mint::Agent resume_agent(unused_model, resume_tools, resume_log,
                             mint::AgentOptions{.resume_session = true},
                             mint::AgentServices{.session_repository = &session});
    EXPECT_THROW((void)resume_agent.run(""), std::invalid_argument);
}

TEST(AgentLoopTest, InvalidUtf8FilenameFailsClosedAndPersistsSafeCheckpoint) {
#if defined(_WIN32)
    GTEST_SKIP() << "Windows filenames are Unicode rather than arbitrary byte sequences";
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto session_path = temporary.path() / "session.json";
    const auto program = mint_test_executable_path().generic_string();
    mint::ToolRuntimeSettings runtime;
    runtime.workspace_snapshot_bytes = 1024;
    runtime.workspace_snapshot_text_bytes = 1024;
    mint::SessionStore session(session_path);
    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.protected_paths = {session_path},
                                                       .allow_write = true,
                                                       .allowed_write_paths = {"src"},
                                                       .allowed_programs = {program},
                                                       .default_command_timeout_seconds = 5,
                                                       .max_command_timeout_seconds = 5,
                                                       .runtime = runtime});
    InvalidFilenameModel model(program);
    std::ostringstream log;
    mint::Agent agent(model, tools, log, {}, mint::AgentServices{.session_repository = &session});

    const auto result = agent.run("创建无法安全表示的文件名");

    EXPECT_FALSE(result.completed);
    EXPECT_EQ(result.status, "failed");
    ASSERT_EQ(result.changes.details.size(), 1);
    EXPECT_EQ(result.changes.details.at(0).path, "<workspace>");
    EXPECT_EQ(result.changes.details.at(0).status, "unauditable");
    const auto checkpoint = session.load();
    EXPECT_EQ(checkpoint.at("status"), "failed");
    EXPECT_EQ(checkpoint.at("change_journal").at("schema_version"),
              mint::workspace_change_schema_version);
    EXPECT_TRUE(checkpoint.at("change_journal").contains("workspace_tracking_error"));
    EXPECT_EQ(checkpoint.at("change_journal").at("workspace_tracking_error"),
              "workspace_reconciliation_failed");
    EXPECT_NO_THROW((void)checkpoint.dump());

    mint::ToolRegistry restored(workspace,
                                mint::ToolRegistryOptions{.protected_paths = {session_path},
                                                          .allow_write = true,
                                                          .allowed_write_paths = {"src"},
                                                          .allowed_programs = {program},
                                                          .default_command_timeout_seconds = 5,
                                                          .max_command_timeout_seconds = 5,
                                                          .runtime = runtime});
    EXPECT_NO_THROW(restored.restore_workspace_change_state(checkpoint.at("change_journal")));
    EXPECT_TRUE(restored.workspace_integrity_failed());
    EXPECT_EQ(restored.workspace_change_state(), checkpoint.at("change_journal"));
#endif
}

} // namespace
