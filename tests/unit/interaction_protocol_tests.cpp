#include "agent/agent_command_internal.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

namespace {

TEST(InteractionProtocolTest, LegacyBoolApprovalCallbacksRemainExplicitUserChoices) {
    const mint::ApprovalDecision approved(true);
    EXPECT_EQ(approved.kind, mint::ApprovalDecisionKind::approved);
    EXPECT_EQ(approved.source(), "user");

    const mint::ApprovalDecision rejected(false);
    EXPECT_EQ(rejected.kind, mint::ApprovalDecisionKind::rejected);
    EXPECT_EQ(rejected.source(), "user");
}

TEST(InteractionProtocolTest, EmitsOneSchemaVersionedControlLine) {
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    mint::cli::Console console(input, output, error);

    mint::cli::command_detail::emit_interaction_message(
        console, "task_ready", {{"task_id", "task-1"}, {"events_path", "/tmp/events.jsonl"}});

    const auto message = mint::Json::parse(error.str());
    EXPECT_EQ(message.at("schema_version"), 1);
    EXPECT_EQ(message.at("channel"), "mint_interaction");
    EXPECT_EQ(message.at("type"), "task_ready");
    EXPECT_EQ(message.at("data").at("task_id"), "task-1");
    EXPECT_TRUE(output.str().empty());
}

TEST(InteractionProtocolTest, CommandApprovalConsumesJsonResponse) {
    std::istringstream input("{\"approved\":true}\n");
    std::ostringstream output;
    std::ostringstream error;
    mint::cli::Console console(input, output, error);
    const auto approval = mint::cli::command_detail::command_approval(console, true);

    const auto decision = approval({.program = "cmake",
                                    .args = {"--build", "build"},
                                    .cwd = "/workspace",
                                    .timeout_seconds = 60});
    EXPECT_EQ(decision.kind, mint::ApprovalDecisionKind::approved);
    const auto message = mint::Json::parse(error.str());
    EXPECT_EQ(message.at("type"), "command_approval");
    EXPECT_EQ(message.at("data").at("program"), "cmake");
    EXPECT_EQ(message.at("data").at("timeout_seconds"), 60);
}

TEST(InteractionProtocolTest, InvalidApprovalResponseFailsClosed) {
    const auto eventPath =
        std::filesystem::temp_directory_path() /
        ("mint-invalid-approval-audit-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    {
        std::istringstream input("yes\n");
        std::ostringstream output;
        std::ostringstream error;
        mint::cli::Console console(input, output, error);
        mint::EventLog events(eventPath);
        const auto approval =
            mint::cli::command_detail::change_set_approval(console, true, &events);

        const auto decision = approval({.paths = {"src/agent.cpp"},
                                        .unified_diff = "+ unsafe change",
                                        .diff_truncated = false});
        EXPECT_EQ(decision.kind, mint::ApprovalDecisionKind::invalid_response);
    }
    std::ifstream eventInput(eventPath);
    std::string requestedLine;
    std::string resolvedLine;
    ASSERT_TRUE(std::getline(eventInput, requestedLine));
    ASSERT_TRUE(std::getline(eventInput, resolvedLine));
    const auto resolved = mint::Json::parse(resolvedLine);
    EXPECT_FALSE(resolved.at("data").at("approved").get<bool>());
    EXPECT_EQ(resolved.at("data").at("decision_source"), "invalid_response");
    eventInput.close();
    std::error_code cleanupError;
    std::filesystem::remove(eventPath, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(InteractionProtocolTest, ClosedApprovalChannelIsNotPersistedAsUserRejection) {
    const auto eventPath =
        std::filesystem::temp_directory_path() /
        ("mint-closed-approval-audit-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    {
        std::istringstream input;
        std::ostringstream output;
        std::ostringstream error;
        mint::cli::Console console(input, output, error);
        mint::EventLog events(eventPath);
        const auto approval = mint::cli::command_detail::command_approval(console, true, &events);

        const auto decision = approval({.program = "ctest",
                                        .args = {"--output-on-failure"},
                                        .cwd = "/workspace",
                                        .timeout_seconds = 300});
        EXPECT_EQ(decision.kind, mint::ApprovalDecisionKind::interaction_closed);
    }
    std::ifstream eventInput(eventPath);
    std::string requestedLine;
    std::string resolvedLine;
    ASSERT_TRUE(std::getline(eventInput, requestedLine));
    ASSERT_TRUE(std::getline(eventInput, resolvedLine));
    const auto resolved = mint::Json::parse(resolvedLine);
    EXPECT_FALSE(resolved.at("data").at("approved").get<bool>());
    EXPECT_EQ(resolved.at("data").at("decision_source"), "interaction_closed");
    eventInput.close();
    std::error_code cleanupError;
    std::filesystem::remove(eventPath, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(InteractionProtocolTest, ApprovalDecisionIsPersistedWithExactExecutionEvidence) {
    const auto eventPath =
        std::filesystem::temp_directory_path() /
        ("mint-approval-audit-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    {
        std::istringstream input("{\"approved\":true}\n");
        std::ostringstream output;
        std::ostringstream error;
        mint::cli::Console console(input, output, error);
        mint::EventLog events(eventPath);
        const auto approval = mint::cli::command_detail::command_approval(console, true, &events);

        const auto decision = approval({.program = "cmake",
                                        .args = {"--build", "build with spaces"},
                                        .cwd = "/workspace/project",
                                        .timeout_seconds = 300});
        EXPECT_EQ(decision.kind, mint::ApprovalDecisionKind::approved);
    }

    std::ifstream input(eventPath);
    std::string requestedLine;
    std::string resolvedLine;
    ASSERT_TRUE(std::getline(input, requestedLine));
    ASSERT_TRUE(std::getline(input, resolvedLine));
    const auto requested = mint::Json::parse(requestedLine);
    const auto resolved = mint::Json::parse(resolvedLine);
    EXPECT_EQ(requested.at("type"), "approval_requested");
    EXPECT_EQ(requested.at("data").at("program"), "cmake");
    EXPECT_EQ(requested.at("data").at("args").at(1), "build with spaces");
    EXPECT_EQ(requested.at("data").at("cwd"), "/workspace/project");
    EXPECT_EQ(requested.at("data").at("timeout_seconds"), 300);
    EXPECT_EQ(resolved.at("type"), "approval_resolved");
    EXPECT_TRUE(resolved.at("data").at("approved"));
    auto expectedResolvedData = requested.at("data");
    expectedResolvedData["approved"] = true;
    expectedResolvedData["decision_source"] = "user";
    EXPECT_EQ(resolved.at("data"), expectedResolvedData);
    input.close();
    std::error_code cleanupError;
    std::filesystem::remove(eventPath, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(InteractionProtocolTest, ChangeSetDecisionKeepsTheExactRequestedEvidence) {
    const auto eventPath =
        std::filesystem::temp_directory_path() /
        ("mint-changeset-audit-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    {
        std::istringstream input("{\"approved\":false}\n");
        std::ostringstream output;
        std::ostringstream error;
        mint::cli::Console console(input, output, error);
        mint::EventLog events(eventPath);
        const auto approval =
            mint::cli::command_detail::change_set_approval(console, true, &events);

        const auto decision =
            approval({.paths = {"src/agent.cpp", "tests/agent_tests.cpp"},
                      .unified_diff = "--- a/src/agent.cpp\n+++ b/src/agent.cpp\n",
                      .diff_truncated = true});
        EXPECT_EQ(decision.kind, mint::ApprovalDecisionKind::rejected);
    }

    std::ifstream input(eventPath);
    std::string requestedLine;
    std::string resolvedLine;
    ASSERT_TRUE(std::getline(input, requestedLine));
    ASSERT_TRUE(std::getline(input, resolvedLine));
    const auto requested = mint::Json::parse(requestedLine);
    const auto resolved = mint::Json::parse(resolvedLine);
    EXPECT_EQ(requested.at("type"), "approval_requested");
    EXPECT_EQ(resolved.at("type"), "approval_resolved");
    auto expectedResolvedData = requested.at("data");
    expectedResolvedData["approved"] = false;
    expectedResolvedData["decision_source"] = "user";
    EXPECT_EQ(resolved.at("data"), expectedResolvedData);
    input.close();
    std::error_code cleanupError;
    std::filesystem::remove(eventPath, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(InteractionProtocolTest, RunCancellationIsPersistedAsInvalidatedNotUserRejected) {
    const auto eventPath =
        std::filesystem::temp_directory_path() /
        ("mint-cancelled-approval-audit-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    {
        std::istringstream input("{\"approved\":false,\"decision_source\":\"run_cancelled\"}\n");
        std::ostringstream output;
        std::ostringstream error;
        mint::cli::Console console(input, output, error);
        mint::EventLog events(eventPath);
        auto taskControl = std::make_shared<mint::TaskControl>();
        const auto approval =
            mint::cli::command_detail::command_approval(console, true, &events, taskControl);

        const auto decision = approval({.program = "ctest",
                                        .args = {"--output-on-failure"},
                                        .cwd = "/workspace",
                                        .timeout_seconds = 300});
        EXPECT_EQ(decision.kind, mint::ApprovalDecisionKind::run_cancelled);
        EXPECT_TRUE(taskControl->cancellation_requested());
    }

    std::ifstream input(eventPath);
    std::string requestedLine;
    std::string resolvedLine;
    ASSERT_TRUE(std::getline(input, requestedLine));
    ASSERT_TRUE(std::getline(input, resolvedLine));
    const auto resolved = mint::Json::parse(resolvedLine);
    EXPECT_FALSE(resolved.at("data").at("approved").get<bool>());
    EXPECT_EQ(resolved.at("data").at("decision_source"), "run_cancelled");
    input.close();
    std::error_code cleanupError;
    std::filesystem::remove(eventPath, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(InteractionProtocolTest, ChangeSetCancellationKeepsItsSourceAndStopsSynchronously) {
    const auto eventPath =
        std::filesystem::temp_directory_path() /
        ("mint-cancelled-changeset-audit-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    {
        std::istringstream input("{\"approved\":false,\"decision_source\":\"run_cancelled\"}\n");
        std::ostringstream output;
        std::ostringstream error;
        mint::cli::Console console(input, output, error);
        mint::EventLog events(eventPath);
        auto taskControl = std::make_shared<mint::TaskControl>();
        const auto approval =
            mint::cli::command_detail::change_set_approval(console, true, &events, taskControl);

        const auto decision =
            approval({.paths = {"src/agent.cpp"},
                      .unified_diff = "--- a/src/agent.cpp\n+++ b/src/agent.cpp\n",
                      .diff_truncated = false});
        EXPECT_EQ(decision.kind, mint::ApprovalDecisionKind::run_cancelled);
        EXPECT_TRUE(taskControl->cancellation_requested());
    }

    std::ifstream input(eventPath);
    std::string requestedLine;
    std::string resolvedLine;
    ASSERT_TRUE(std::getline(input, requestedLine));
    ASSERT_TRUE(std::getline(input, resolvedLine));
    const auto resolved = mint::Json::parse(resolvedLine);
    EXPECT_FALSE(resolved.at("data").at("approved").get<bool>());
    EXPECT_EQ(resolved.at("data").at("decision_source"), "run_cancelled");
    input.close();
    std::error_code cleanupError;
    std::filesystem::remove(eventPath, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(InteractionProtocolTest, HumanChangeSetPreviewCannotEmitTerminalControls) {
    std::istringstream input("n\n");
    std::ostringstream output;
    std::ostringstream error;
    mint::cli::Console console(input, output, error);
    const auto approval = mint::cli::command_detail::change_set_approval(console, false);
    const auto unsafe_diff = std::string("+普通中文 ") + '\x1B' + "]52;c;clipboard" + '\x07' +
                             '\x1B' + "[2J " + "\xE2\x80\xAE" + "txt\n";

    const auto decision = approval({.paths = {"README.md"}, .unified_diff = unsafe_diff});

    EXPECT_EQ(decision.kind, mint::ApprovalDecisionKind::rejected);
    EXPECT_TRUE(output.str().empty());
    EXPECT_EQ(error.str().find('\x1B'), std::string::npos);
    EXPECT_EQ(error.str().find("\xE2\x80\xAE"), std::string::npos);
    EXPECT_NE(error.str().find("+普通中文 \\u001B]52;c;clipboard\\u0007\\u001B[2J "
                               "\\u202Etxt\n"),
              std::string::npos);
}

TEST(InteractionProtocolTest, HumanCommandApprovalCannotEmitTerminalControls) {
    std::istringstream input("n\n");
    std::ostringstream output;
    std::ostringstream error;
    mint::cli::Console console(input, output, error);
    const auto approval = mint::cli::command_detail::command_approval(console, false);
    const auto unsafe_program = std::string("cmake") + '\x1B' + "]0;program" + '\x07';
    const auto unsafe_cwd = std::string("/workspace/") + '\x1B' + "[2J\n允许执行？ y";
    const auto unsafe_argument = std::string("--target=") + "\xE2\x80\xAE" + "evil";

    const auto decision = approval({.program = unsafe_program,
                                    .args = {"--build", unsafe_argument},
                                    .cwd = unsafe_cwd,
                                    .timeout_seconds = 60});

    EXPECT_EQ(decision.kind, mint::ApprovalDecisionKind::rejected);
    EXPECT_TRUE(output.str().empty());
    EXPECT_EQ(error.str().find('\x1B'), std::string::npos);
    EXPECT_EQ(error.str().find("\xE2\x80\xAE"), std::string::npos);
    EXPECT_NE(error.str().find("cmake\\u001B]0;program\\u0007"), std::string::npos);
    EXPECT_NE(error.str().find("/workspace/\\u001B[2J\\u000A允许执行？ y"), std::string::npos);
    EXPECT_NE(error.str().find("--target=\\u202Eevil"), std::string::npos);
}

TEST(InteractionProtocolTest, JsonlChangeSetPreviewRemainsOneValidJsonDocument) {
    std::istringstream input("{\"approved\":false}\n");
    std::ostringstream output;
    std::ostringstream error;
    mint::cli::Console console(input, output, error);
    const auto approval = mint::cli::command_detail::change_set_approval(console, true);
    const auto raw_diff = std::string("+") + '\x1B' + "[2J" + "\xE2\x80\xAE" + "中文\n";

    EXPECT_EQ(approval({.paths = {"README.md"}, .unified_diff = raw_diff}).kind,
              mint::ApprovalDecisionKind::rejected);

    const auto message = mint::Json::parse(error.str());
    EXPECT_EQ(message.at("type"), "changeset_approval");
    EXPECT_EQ(message.at("data").at("unified_diff"), raw_diff);
    EXPECT_TRUE(output.str().empty());
}

} // namespace
