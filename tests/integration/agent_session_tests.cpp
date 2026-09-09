#include "mint/application/agent.hpp"
#include "mint/infrastructure/session_store.hpp"
#include "mint/runtime/task_control.hpp"
#include "mint/tools/tool_registry.hpp"
#include "mint/version.hpp"

#include "agent_test_models.hpp"
#include "test_executable.hpp"
#include "test_workspace.hpp"

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace {

using mint::test::is_verification_ready;
using mint::test::latest_tool_result;
using mint::test::model_tool_reply;
using mint::test::read_text;
using mint::test::ScriptedModel;
using mint::test::TemporaryDirectory;
using mint::test::write_text;

class StoppingSessionPatchModel final : public mint::ModelClient {
  public:
    explicit StoppingSessionPatchModel(std::shared_ptr<mint::TaskControl> control = {})
        : control_(std::move(control)) {}

    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        if (control_ != nullptr) {
            control_->request_cancel();
        }
        return model_tool_reply("session-patch", "apply_patch",
                                {{"path", "README.md"},
                                 {"operation", "replace"},
                                 {"old_text", "# Broken\n"},
                                 {"new_text", "# Fixed\n"}});
    }

  private:
    std::shared_ptr<mint::TaskControl> control_;
};

class ResumeVerificationModel final : public mint::ModelClient {
  public:
    explicit ResumeVerificationModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json& messages, const mint::Json&) override {
        const auto result = latest_tool_result(messages);
        if (calls_++ == 0) {
            EXPECT_TRUE(result.at("ok").get<bool>())
                << ("restored patch succeeds before verification");
            return model_tool_reply("session-verify", "run_command",
                                    {{"program", program_},
                                     {"args", mint::Json::array({"--command-helper", "verify",
                                                                 "README.md", "# Fixed\n"})},
                                     {"cwd", "."},
                                     {"timeout_seconds", 5}});
        }
        EXPECT_TRUE(is_verification_ready(messages.back()))
            << ("resumed passing verification appends a completion prompt");
        EXPECT_TRUE(result.at("exit_code") == 0) << ("resumed verification command passes");
        return {.assistant_message = {{"role", "assistant"}, {"content", "恢复后验证完成"}},
                .text = "恢复后验证完成",
                .tool_calls = {}};
    }

  private:
    std::string program_;
    int calls_ = 0;
};

TEST(SessionTest, CheckpointsAndResumes) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto session_path = temporary.path() / "session.json";
    const auto program = mint_test_executable_path().generic_string();
    write_text(workspace / "README.md", "# Broken\n");
    mint::SessionStore session(session_path);

    auto stop_after_model_reply = std::make_shared<mint::TaskControl>();
    mint::ToolRegistry first_tools(
        workspace, mint::ToolRegistryOptions{.protected_paths = {session_path},
                                             .allow_write = true,
                                             .allowed_programs = {program},
                                             .default_command_timeout_seconds = 5,
                                             .max_command_timeout_seconds = 5,
                                             .task_control = stop_after_model_reply});
    StoppingSessionPatchModel first_model(stop_after_model_reply);
    std::ostringstream first_log;
    mint::Agent first_agent(
        first_model, first_tools, first_log,
        mint::AgentOptions{.max_turns = 1, .require_verification_after_write = true},
        mint::AgentServices{.stop_token = stop_after_model_reply.get(),
                            .session_repository = &session});
    const auto interrupted = first_agent.run("修复 README 并验证");
    EXPECT_TRUE(interrupted.status == "cancelled" && !interrupted.completed)
        << ("task cancellation stops the first invocation explicitly");
    EXPECT_TRUE(read_text(workspace / "README.md") == "# Broken\n")
        << ("pending tool is checkpointed before execution when cancellation arrives");
    const auto checkpoint = session.load();
    EXPECT_TRUE(checkpoint.at("schema_version") == mint::session_schema_version &&
                checkpoint.at("status") == "cancelled" &&
                checkpoint.at("pending_tool_calls").size() == 1 &&
                checkpoint.at("in_flight_tool_call").is_null())
        << ("session stores the pending call at a stable resume point");
    EXPECT_TRUE(checkpoint.at("model").at("response_header_max_request_tokens").is_null() &&
                checkpoint.at("model").at("request_token_estimate_bytes_per_token") == 2)
        << ("session preserves an explicitly unknown provider header budget");

    const auto incomplete_path = temporary.path() / "incomplete-current-session.json";
    mint::SessionStore incomplete_session(incomplete_path);
    auto incomplete_checkpoint = checkpoint;
    incomplete_checkpoint.erase("in_flight_tool_call");
    incomplete_session.save(incomplete_checkpoint);
    bool rejected_incomplete_current = false;
    try {
        mint::ToolRegistry incomplete_tools(
            workspace, mint::ToolRegistryOptions{.protected_paths = {incomplete_path},
                                                 .allow_write = true,
                                                 .allowed_programs = {program},
                                                 .default_command_timeout_seconds = 5,
                                                 .max_command_timeout_seconds = 5});
        StoppingSessionPatchModel unused_model;
        std::ostringstream unused_log;
        mint::Agent incomplete_agent(
            unused_model, incomplete_tools, unused_log,
            mint::AgentOptions{.require_verification_after_write = true, .resume_session = true},
            mint::AgentServices{.session_repository = &incomplete_session});
        (void)incomplete_agent.run("");
    } catch (const std::invalid_argument& error) {
        rejected_incomplete_current =
            std::string(error.what()).find("缺少必需状态") != std::string::npos;
    }
    EXPECT_TRUE(rejected_incomplete_current)
        << ("current schema rejects a checkpoint missing its durable in-flight marker");

    bool rejected_tool_limit_change = false;
    try {
        mint::ToolRegistry mismatched_tools(
            workspace, mint::ToolRegistryOptions{
                           .protected_paths = {session_path},
                           .allow_write = true,
                           .allowed_programs = {program},
                           .default_command_timeout_seconds = 5,
                           .max_command_timeout_seconds = 5,
                           .runtime = mint::ToolRuntimeSettings{.read_file_bytes = 2048}});
        StoppingSessionPatchModel unused_model;
        std::ostringstream unused_log;
        mint::Agent mismatched_agent(
            unused_model, mismatched_tools, unused_log,
            mint::AgentOptions{.require_verification_after_write = true, .resume_session = true},
            mint::AgentServices{.session_repository = &session});
        (void)mismatched_agent.run("");
    } catch (const std::invalid_argument& error) {
        rejected_tool_limit_change =
            std::string(error.what()).find("能力授权") != std::string::npos;
    }
    EXPECT_TRUE(rejected_tool_limit_change)
        << ("resume rejects tool budget changes that could alter pending work");

    const auto inflight_path = temporary.path() / "inflight-session.json";
    mint::SessionStore inflight_session(inflight_path);
    auto inflight_checkpoint = checkpoint;
    inflight_checkpoint["status"] = "running";
    inflight_checkpoint["in_flight_tool_call"] = inflight_checkpoint.at("pending_tool_calls").at(0);
    inflight_session.save(inflight_checkpoint);

    bool blocked_inflight_replay = false;
    try {
        mint::ToolRegistry blocked_tools(
            workspace, mint::ToolRegistryOptions{.protected_paths = {inflight_path},
                                                 .allow_write = true,
                                                 .allowed_programs = {program},
                                                 .default_command_timeout_seconds = 5,
                                                 .max_command_timeout_seconds = 5});
        ResumeVerificationModel unused_model(program);
        std::ostringstream unused_log;
        mint::Agent blocked_agent(unused_model, blocked_tools, unused_log,
                                  mint::AgentOptions{.max_turns = 2,
                                                     .require_verification_after_write = true,
                                                     .resume_session = true},
                                  mint::AgentServices{.session_repository = &inflight_session});
        (void)blocked_agent.run("");
    } catch (const std::runtime_error& error) {
        blocked_inflight_replay =
            std::string(error.what()).find("--retry-inflight") != std::string::npos;
    }
    EXPECT_TRUE(blocked_inflight_replay)
        << ("resume blocks an ambiguous side-effecting in-flight tool by default");

    mint::ToolRegistry retry_tools(workspace,
                                   mint::ToolRegistryOptions{.protected_paths = {inflight_path},
                                                             .allow_write = true,
                                                             .allowed_programs = {program},
                                                             .default_command_timeout_seconds = 5,
                                                             .max_command_timeout_seconds = 5});
    ResumeVerificationModel retry_model(program);
    std::ostringstream retry_log;
    mint::Agent retry_agent(retry_model, retry_tools, retry_log,
                            mint::AgentOptions{.max_turns = 2,
                                               .require_verification_after_write = true,
                                               .resume_session = true,
                                               .retry_in_flight_tool = true},
                            mint::AgentServices{.session_repository = &inflight_session});
    const auto retry_result = retry_agent.run("");
    EXPECT_TRUE(retry_result.completed && read_text(workspace / "README.md") == "# Fixed\n")
        << ("explicit retry authorization replays the in-flight tool and completes verification");
    write_text(workspace / "README.md", "# Broken\n");

    const auto legacy_path = temporary.path() / "legacy-v2-session.json";
    mint::SessionStore legacy_session(legacy_path);
    auto legacy_checkpoint = checkpoint;
    legacy_checkpoint["schema_version"] = 2;
    legacy_checkpoint.erase("in_flight_tool_call");
    legacy_checkpoint.erase("model");
    legacy_checkpoint["capabilities"].erase("command_recipes");
    legacy_checkpoint["capabilities"].erase("policy_fingerprint");
    legacy_checkpoint["capabilities"].erase("approve_each_changeset");
    legacy_checkpoint["capabilities"].erase("tool_limits");
    legacy_checkpoint["execution"].erase("recipe_calls");
    legacy_checkpoint["execution"].erase("verification_commands");
    legacy_checkpoint["execution"].erase("last_command_verification_eligible");
    legacy_checkpoint["change_journal"] = {{"schema_version", 1}, {"entries", mint::Json::array()}};
    legacy_session.save(legacy_checkpoint);
    mint::ToolRegistry legacy_tools(workspace,
                                    mint::ToolRegistryOptions{.protected_paths = {legacy_path},
                                                              .allow_write = true,
                                                              .allowed_programs = {program},
                                                              .default_command_timeout_seconds = 5,
                                                              .max_command_timeout_seconds = 5});
    ResumeVerificationModel legacy_model(program);
    std::ostringstream legacy_log;
    mint::Agent legacy_agent(legacy_model, legacy_tools, legacy_log,
                             mint::AgentOptions{.max_turns = 2,
                                                .require_verification_after_write = true,
                                                .resume_session = true},
                             mint::AgentServices{.session_repository = &legacy_session});
    const auto migrated = legacy_agent.run("");
    EXPECT_TRUE(migrated.completed &&
                legacy_session.load().at("schema_version") == mint::session_schema_version)
        << ("current mint restores a v2 checkpoint and rewrites it with the current schema");
    write_text(workspace / "README.md", "# Broken\n");

    const auto legacy_v4_path = temporary.path() / "legacy-v4-session.json";
    mint::SessionStore legacy_v4_session(legacy_v4_path);
    auto legacy_v4_checkpoint = checkpoint;
    legacy_v4_checkpoint["schema_version"] = 4;
    legacy_v4_checkpoint["change_journal"] = {{"schema_version", 2},
                                              {"entries", mint::Json::array()}};
    legacy_v4_checkpoint["capabilities"]["tool_limits"].erase("workspace_snapshot_entries");
    legacy_v4_checkpoint["capabilities"]["tool_limits"].erase("workspace_snapshot_bytes");
    legacy_v4_checkpoint["capabilities"]["tool_limits"].erase("workspace_snapshot_text_bytes");
    legacy_v4_session.save(legacy_v4_checkpoint);
    mint::ToolRegistry legacy_v4_tools(
        workspace, mint::ToolRegistryOptions{.protected_paths = {legacy_v4_path},
                                             .allow_write = true,
                                             .allowed_programs = {program},
                                             .default_command_timeout_seconds = 5,
                                             .max_command_timeout_seconds = 5});
    ResumeVerificationModel legacy_v4_model(program);
    std::ostringstream legacy_v4_log;
    mint::Agent legacy_v4_agent(legacy_v4_model, legacy_v4_tools, legacy_v4_log,
                                mint::AgentOptions{.max_turns = 2,
                                                   .require_verification_after_write = true,
                                                   .resume_session = true},
                                mint::AgentServices{.session_repository = &legacy_v4_session});
    const auto migrated_v4 = legacy_v4_agent.run("");
    EXPECT_TRUE(migrated_v4.completed &&
                legacy_v4_session.load().at("schema_version") == mint::session_schema_version &&
                legacy_v4_session.load().at("change_journal").at("schema_version") ==
                    mint::workspace_change_schema_version)
        << ("current mint restores an untainted v4 checkpoint and rewrites both safety schemas");
    write_text(workspace / "README.md", "# Broken\n");

    bool rejected_policy_downgrade = false;
    try {
        mint::ToolRegistry mismatched_tools(
            workspace,
            mint::ToolRegistryOptions{
                .protected_paths = {session_path},
                .allow_write = true,
                .allowed_programs = {program},
                .command_approval = [](const mint::CommandApprovalRequest&) { return true; }});
        ScriptedModel unused_model;
        std::ostringstream unused_log;
        mint::Agent mismatched_agent(
            unused_model, mismatched_tools, unused_log,
            mint::AgentOptions{.require_verification_after_write = true, .resume_session = true},
            mint::AgentServices{.session_repository = &session});
        (void)mismatched_agent.run("");
    } catch (const std::invalid_argument& error) {
        rejected_policy_downgrade = std::string(error.what()).find("能力授权") != std::string::npos;
    }
    EXPECT_TRUE(rejected_policy_downgrade)
        << ("resume rejects a different per-command approval policy");

    mint::ToolRegistry resumed_tools(workspace,
                                     mint::ToolRegistryOptions{.protected_paths = {session_path},
                                                               .allow_write = true,
                                                               .allowed_programs = {program},
                                                               .default_command_timeout_seconds = 5,
                                                               .max_command_timeout_seconds = 5});
    ResumeVerificationModel resumed_model(program);
    std::ostringstream resumed_log;
    mint::Agent resumed_agent(resumed_model, resumed_tools, resumed_log,
                              mint::AgentOptions{.max_turns = 2,
                                                 .require_verification_after_write = true,
                                                 .resume_session = true},
                              mint::AgentServices{.session_repository = &session});
    const auto resumed = resumed_agent.run("");
    EXPECT_TRUE(resumed.completed && resumed.status == "completed")
        << ("resumed session executes the pending patch and reaches completion");
    EXPECT_TRUE(resumed.answer == "恢复后验证完成" && resumed.turns == 3)
        << ("resume preserves prior turns and continues the original conversation");
    EXPECT_TRUE(resumed.execution.file_changes == 1 && resumed.execution.commands_passed == 1)
        << ("resume preserves and extends the execution summary");
    EXPECT_TRUE(resumed.verification_status == "passed")
        << ("resumed task still satisfies the verification gate");
    EXPECT_TRUE(read_text(workspace / "README.md") == "# Fixed\n")
        << ("restored pending patch changes the workspace exactly once");
    EXPECT_TRUE(resumed.changes.unified_diff.find("-# Broken\n+# Fixed\n") != std::string::npos)
        << ("restored change journal preserves the original-to-final diff");
    EXPECT_TRUE(session.load().at("status") == "completed")
        << ("final stable checkpoint records terminal completion");
}

} // namespace
