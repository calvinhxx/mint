#include "mint/application/agent.hpp"
#include "mint/infrastructure/event_log.hpp"
#include "mint/tools/tool_registry.hpp"

#include "agent_test_models.hpp"
#include "test_executable.hpp"
#include "test_workspace.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using mint::test::is_verification_ready;
using mint::test::latest_tool_result;
using mint::test::model_tool_reply;
using mint::test::read_text;
using mint::test::TemporaryDirectory;
using mint::test::write_text;

class WritingModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        EXPECT_TRUE(tools.size() == 6) << ("write-enabled agent exposes six tools");
        EXPECT_TRUE(messages.at(0).at("content").get<std::string>().find("apply_patch") !=
                    std::string::npos)
            << ("write-enabled system prompt explains apply_patch");

        if (calls_++ == 0) {
            const mint::Json arguments = {{"path", "README.md"},
                                          {"operation", "replace"},
                                          {"old_text", "# Before\n"},
                                          {"new_text", "# After\n"}};
            const mint::Json raw_call = {
                {"id", "test-patch"},
                {"type", "function"},
                {"function", {{"name", "apply_patch"}, {"arguments", arguments.dump()}}}};
            return {.assistant_message = {{"role", "assistant"},
                                          {"content", nullptr},
                                          {"tool_calls", mint::Json::array({raw_call})}},
                    .text = {},
                    .tool_calls = {{"test-patch", "apply_patch", arguments}}};
        }

        EXPECT_TRUE(messages.back().at("role") == "tool")
            << ("patch result is appended to context");
        const auto result = mint::Json::parse(messages.back().at("content").get<std::string>());
        EXPECT_TRUE(result.at("ok").get<bool>())
            << ("patch result returned to model is successful");
        return {.assistant_message = {{"role", "assistant"}, {"content", "修改完成"}},
                .text = "修改完成",
                .tool_calls = {}};
    }

  private:
    int calls_ = 0;
};

std::size_t verification_ready_count(const mint::Json& messages) {
    return static_cast<std::size_t>(
        std::count_if(messages.begin(), messages.end(),
                      [](const mint::Json& message) { return is_verification_ready(message); }));
}

class PatchThenVerifyModel final : public mint::ModelClient {
  public:
    explicit PatchThenVerifyModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        EXPECT_TRUE(tools.size() == 7) << ("write-and-command agent exposes seven tools");
        const auto system_prompt = messages.at(0).at("content").get<std::string>();
        EXPECT_TRUE(system_prompt.find("apply_patch") != std::string::npos)
            << ("validation system prompt explains apply_patch");
        EXPECT_TRUE(system_prompt.find("run_command") != std::string::npos)
            << ("validation system prompt explains run_command");

        if (calls_ == 0) {
            ++calls_;
            return tool_reply("e2e-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Broken\n"},
                               {"new_text", "# Fixed\n"}});
        }

        if (calls_ == 2) {
            EXPECT_TRUE(is_verification_ready(messages.back()))
                << ("passing verification appends a completion prompt");
        }
        const auto previous = latest_tool_result(messages);
        EXPECT_TRUE(previous.at("ok").get<bool>()) << ("previous e2e tool result succeeded");

        if (calls_ == 1) {
            ++calls_;
            return tool_reply("e2e-verify", "run_command",
                              {{"program", program_},
                               {"args", mint::Json::array({"--command-helper", "verify",
                                                           "README.md", "# Fixed\n"})},
                               {"cwd", "."},
                               {"timeout_seconds", 5}});
        }

        EXPECT_TRUE(previous.at("status") == "exited") << ("verification command exited normally");
        EXPECT_TRUE(previous.at("exit_code") == 0) << ("verification command passed");
        EXPECT_TRUE(previous.at("output").get<std::string>().find("verification passed") !=
                    std::string::npos)
            << ("verification evidence is returned to the model");
        ++calls_;
        return {.assistant_message = {{"role", "assistant"}, {"content", "修改并验证完成"}},
                .text = "修改并验证完成",
                .tool_calls = {}};
    }

  private:
    static mint::ModelReply tool_reply(std::string id, std::string name, mint::Json arguments) {
        mint::Json raw_call = {{"id", id},
                               {"type", "function"},
                               {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
        mint::ModelReply reply;
        reply.assistant_message = {{"role", "assistant"},
                                   {"content", nullptr},
                                   {"tool_calls", mint::Json::array({raw_call})}};
        reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
        return reply;
    }

    std::string program_;
    int calls_ = 0;
};

class VerifyAtTurnLimitModel final : public mint::ModelClient {
  public:
    explicit VerifyAtTurnLimitModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        switch (calls_++) {
        case 0:
            EXPECT_TRUE(!tools.empty()) << ("the repair turn keeps workspace tools available");
            return tool_reply("limit-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Broken\n"},
                               {"new_text", "# Fixed\n"}});
        case 1:
            EXPECT_TRUE(!tools.empty()) << ("the verification turn keeps command tools available");
            return tool_reply("limit-verify", "run_command",
                              {{"program", program_},
                               {"args", mint::Json::array({"--command-helper", "verify",
                                                           "README.md", "# Fixed\n"})},
                               {"cwd", "."},
                               {"timeout_seconds", 5}});
        default:
            EXPECT_TRUE(tools.empty())
                << ("the reserved final-answer turn cannot request more tools");
            EXPECT_TRUE(is_verification_ready(messages.back()))
                << ("the final-answer turn receives verified completion context");
            final_answer_turn_seen_ = true;
            return {.assistant_message = {{"role", "assistant"}, {"content", "轮数边界内验证完成"}},
                    .text = "轮数边界内验证完成"};
        }
    }

    [[nodiscard]] bool final_answer_turn_seen() const noexcept {
        return final_answer_turn_seen_;
    }

  private:
    static mint::ModelReply tool_reply(std::string id, std::string name, mint::Json arguments) {
        const mint::Json raw_call = {
            {"id", id},
            {"type", "function"},
            {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
        return {.assistant_message = {{"role", "assistant"},
                                      {"content", nullptr},
                                      {"tool_calls", mint::Json::array({raw_call})}},
                .tool_calls = {{std::move(id), std::move(name), std::move(arguments)}}};
    }

    std::string program_;
    int calls_ = 0;
    bool final_answer_turn_seen_ = false;
};

class FailureThenRepairModel final : public mint::ModelClient {
  public:
    explicit FailureThenRepairModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        EXPECT_TRUE(tools.size() == 7) << ("verification-gated agent exposes seven tools");
        const auto system_prompt = messages.at(0).at("content").get<std::string>();
        EXPECT_TRUE(system_prompt.find("Harness policy requires verification") != std::string::npos)
            << ("system prompt explains the required verification gate");

        switch (calls_++) {
        case 0:
            return tool_reply("retry-first-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Broken\n"},
                               {"new_text", "# Almost\n"}});
        case 1:
            EXPECT_TRUE(latest_tool_result(messages).at("ok").get<bool>())
                << ("first patch succeeded before failed verification");
            return verification_call("retry-first-verify");
        case 2: {
            const auto failed = latest_tool_result(messages);
            EXPECT_TRUE(failed.at("exit_code") == 9)
                << ("first verification exposes the expected failure");
            return final_reply("错误地提前结束");
        }
        case 3:
            EXPECT_TRUE(messages.back().at("role") == "user")
                << ("harness gate appends a continuation requirement");
            EXPECT_TRUE(messages.back().at("content").get<std::string>().find(
                            "unverified changes") != std::string::npos)
                << ("continuation requirement explains unverified changes");
            gate_seen_ = true;
            return tool_reply("retry-second-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Almost\n"},
                               {"new_text", "# Fixed\n"}});
        case 4:
            EXPECT_TRUE(latest_tool_result(messages).at("ok").get<bool>())
                << ("second patch succeeded after the gate");
            return verification_call("retry-second-verify");
        default: {
            EXPECT_TRUE(is_verification_ready(messages.back()))
                << ("repaired task receives the verified completion prompt");
            const auto passed = latest_tool_result(messages);
            EXPECT_TRUE(passed.at("exit_code") == 0)
                << ("second verification passes after the repair");
            return final_reply("失败后继续修复并验证完成");
        }
        }
    }

    [[nodiscard]] bool gate_seen() const noexcept {
        return gate_seen_;
    }

  private:
    static mint::ModelReply tool_reply(std::string id, std::string name, mint::Json arguments) {
        mint::Json raw_call = {{"id", id},
                               {"type", "function"},
                               {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
        mint::ModelReply reply;
        reply.assistant_message = {{"role", "assistant"},
                                   {"content", nullptr},
                                   {"tool_calls", mint::Json::array({raw_call})}};
        reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
        return reply;
    }

    mint::ModelReply verification_call(std::string id) const {
        return tool_reply(
            std::move(id), "run_command",
            {{"program", program_},
             {"args", mint::Json::array({"--command-helper", "verify", "README.md", "# Fixed\n"})},
             {"cwd", "."},
             {"timeout_seconds", 5}});
    }

    static mint::ModelReply final_reply(std::string text) {
        return {.assistant_message = {{"role", "assistant"}, {"content", text}},
                .text = std::move(text),
                .tool_calls = {}};
    }

    std::string program_;
    int calls_ = 0;
    bool gate_seen_ = false;
};

class PassThenFailModel final : public mint::ModelClient {
  public:
    explicit PassThenFailModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json& messages, const mint::Json&) override {
        switch (calls_++) {
        case 0:
            return tool_reply("regression-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Broken\n"},
                               {"new_text", "# Fixed\n"}});
        case 1:
            return tool_reply("regression-pass", "run_command",
                              {{"program", program_},
                               {"args", mint::Json::array({"--command-helper", "verify",
                                                           "README.md", "# Fixed\n"})},
                               {"cwd", "."},
                               {"timeout_seconds", 5}});
        case 2:
            EXPECT_TRUE(is_verification_ready(messages.back()) &&
                        verification_ready_count(messages) == 1)
                << ("passing verification emits one completion prompt");
            EXPECT_TRUE(latest_tool_result(messages).at("exit_code") == 0)
                << ("initial verification passes before the later failure");
            return tool_reply("regression-fail", "run_command",
                              {{"program", program_},
                               {"args", mint::Json::array({"--command-helper", "fail"})},
                               {"cwd", "."},
                               {"timeout_seconds", 5}});
        default:
            EXPECT_TRUE(verification_ready_count(messages) == 1)
                << ("later failed command does not duplicate the completion prompt");
            EXPECT_TRUE(latest_tool_result(messages).at("exit_code") == 7)
                << ("later command exposes the regression failure");
            return {.assistant_message = {{"role", "assistant"}, {"content", "错误地忽略后续失败"}},
                    .text = "错误地忽略后续失败",
                    .tool_calls = {}};
        }
    }

  private:
    static mint::ModelReply tool_reply(std::string id, std::string name, mint::Json arguments) {
        mint::Json raw_call = {{"id", id},
                               {"type", "function"},
                               {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
        mint::ModelReply reply;
        reply.assistant_message = {{"role", "assistant"},
                                   {"content", nullptr},
                                   {"tool_calls", mint::Json::array({raw_call})}};
        reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
        return reply;
    }

    std::string program_;
    int calls_ = 0;
};

class DeniedVerificationModel final : public mint::ModelClient {
  public:
    explicit DeniedVerificationModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        switch (calls_++) {
        case 0:
            return model_tool_reply("denied-patch", "apply_patch",
                                    {{"path", "README.md"},
                                     {"operation", "replace"},
                                     {"old_text", "# Broken\n"},
                                     {"new_text", "# Fixed\n"}});
        case 1:
            return model_tool_reply("denied-verify", "run_command",
                                    {{"program", program_},
                                     {"args", mint::Json::array({"--command-helper", "verify",
                                                                 "README.md", "# Fixed\n"})},
                                     {"cwd", "."},
                                     {"timeout_seconds", 5}});
        default:
            return {
                .assistant_message = {{"role", "assistant"}, {"content", "错误地把拒绝当成通过"}},
                .text = "错误地把拒绝当成通过",
                .tool_calls = {}};
        }
    }

  private:
    std::string program_;
    int calls_ = 0;
};

TEST(AgentLoopTest, CompletesWriteTask) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Before\n");

    mint::ToolRegistry tools(workspace, mint::ToolRegistryOptions{.allow_write = true});
    WritingModel model;
    std::ostringstream log;
    mint::Agent agent(model, tools, log);

    const auto result = agent.run("修改 README 标题");
    EXPECT_TRUE(result.completed) << ("write-enabled agent reaches a final answer");
    EXPECT_TRUE(result.answer == "修改完成") << ("write-enabled agent returns final answer");
    EXPECT_TRUE(read_text(workspace / "README.md") == "# After\n")
        << ("agent loop executes apply_patch");
    EXPECT_TRUE(result.execution.file_changes == 1)
        << ("write agent summary counts the file modification");
    EXPECT_TRUE(result.changes.files == std::vector<std::string>{"README.md"})
        << ("write agent result exposes the changed file list");
    EXPECT_TRUE(result.changes.unified_diff.find("-# Before\n+# After\n") != std::string::npos)
        << ("write agent result exposes the unified diff");
    EXPECT_TRUE(result.verification_status == "not_run")
        << ("unverified write is marked not_run when the gate is disabled");
    const auto write_log = log.str();
    EXPECT_TRUE(write_log.find("README.md") != std::string::npos)
        << ("patch log includes the target path");
    const auto before_final = write_log.substr(0, write_log.find("[最终回答]"));
    EXPECT_TRUE(before_final.find("# Before") == std::string::npos)
        << ("patch log does not dump old file contents");
    EXPECT_TRUE(before_final.find("# After") == std::string::npos)
        << ("patch log does not dump new file contents");
}

TEST(AgentLoopTest, PatchesThenVerifies) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto event_path = temporary.path() / "events.jsonl";
    write_text(workspace / "README.md", "# Broken\n");
    const auto program = mint_test_executable_path().generic_string();

    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.allow_write = true,
                                                       .allowed_programs = {program},
                                                       .default_command_timeout_seconds = 5,
                                                       .max_command_timeout_seconds = 5});
    PatchThenVerifyModel model(program);
    mint::EventLog events(event_path);
    std::ostringstream log;
    mint::Agent agent(model, tools, log,
                      mint::AgentOptions{.require_verification_after_write = true},
                      mint::AgentServices{.event_sink = &events});

    const auto result = agent.run("修复 README 并执行验证");
    EXPECT_TRUE(result.completed) << ("patch-then-verify agent reaches a final answer");
    EXPECT_TRUE(result.answer == "修改并验证完成")
        << ("patch-then-verify agent returns the verified answer");
    EXPECT_TRUE(result.turns == 3)
        << ("agent performs patch, verification command, then final answer");
    EXPECT_TRUE(result.execution.tool_calls == 2)
        << ("end-to-end summary counts patch and command tools");
    EXPECT_TRUE(result.execution.file_changes == 1) << ("end-to-end summary counts the patch");
    EXPECT_TRUE(result.execution.command_calls == 1)
        << ("end-to-end summary counts the verification command");
    EXPECT_TRUE(result.execution.commands_passed == 1)
        << ("end-to-end summary records the passing verification");
    EXPECT_TRUE(result.execution.commands_failed == 0)
        << ("end-to-end summary has no failed commands");
    EXPECT_TRUE(result.verification_status == "passed")
        << ("end-to-end result records passed verification");
    EXPECT_TRUE(result.changes.files == std::vector<std::string>{"README.md"})
        << ("end-to-end result lists the modified file");
    EXPECT_TRUE(read_text(workspace / "README.md") == "# Fixed\n")
        << ("end-to-end loop leaves the requested file change");
    EXPECT_TRUE(log.str().find("apply_patch") != std::string::npos)
        << ("end-to-end log records the patch tool");
    EXPECT_TRUE(log.str().find("run_command") != std::string::npos)
        << ("end-to-end log records the verification command");
    const auto e2e_log = log.str();
    const auto e2e_before_final = e2e_log.substr(0, e2e_log.find("[最终回答]"));
    EXPECT_TRUE(e2e_before_final.find("# Fixed") == std::string::npos)
        << ("end-to-end tool-call logs do not dump patch or command arguments");
    EXPECT_TRUE(e2e_log.find("+# Fixed") != std::string::npos)
        << ("end-to-end final state prints the audited diff");
    std::ifstream event_input(event_path, std::ios::binary);
    const std::string event_log{std::istreambuf_iterator<char>(event_input),
                                std::istreambuf_iterator<char>()};
    EXPECT_TRUE(event_log.find("\"type\":\"verification_ready\"") != std::string::npos)
        << ("event trace records that verification is ready for completion");
    EXPECT_TRUE(event_log.find("Harness status") == std::string::npos)
        << ("event trace does not copy the model-facing completion prompt");
}

TEST(AgentLoopTest, ReservesFinalAnswerAfterVerificationAtTurnLimit) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Broken\n");
    const auto program = mint_test_executable_path().generic_string();

    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.allow_write = true,
                                                       .allowed_programs = {program},
                                                       .default_command_timeout_seconds = 5,
                                                       .max_command_timeout_seconds = 5});
    VerifyAtTurnLimitModel model(program);
    std::ostringstream log;
    mint::Agent agent(model, tools, log,
                      mint::AgentOptions{.max_turns = 2, .require_verification_after_write = true});

    const auto result = agent.run("修复 README 并执行验证");

    EXPECT_TRUE(result.completed && result.status == "completed")
        << ("verified work receives a reserved final-answer turn");
    EXPECT_TRUE(result.answer == "轮数边界内验证完成")
        << ("the reserved turn preserves the model's final answer");
    EXPECT_TRUE(result.turns == 3 && model.final_answer_turn_seen())
        << ("the action budget excludes one tool-free final-answer turn");
    EXPECT_TRUE(result.verification_status == "passed")
        << ("the reserved turn cannot weaken the verification gate");
    EXPECT_TRUE(read_text(workspace / "README.md") == "# Fixed\n")
        << ("the verified workspace change remains intact");
}

TEST(AgentLoopTest, FailedVerificationRequiresRepair) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Broken\n");
    const auto program = mint_test_executable_path().generic_string();

    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.allow_write = true,
                                                       .allowed_programs = {program},
                                                       .default_command_timeout_seconds = 5,
                                                       .max_command_timeout_seconds = 5});
    FailureThenRepairModel model(program);
    std::ostringstream log;
    mint::Agent agent(model, tools, log,
                      mint::AgentOptions{.max_turns = 8, .require_verification_after_write = true});

    const auto result = agent.run("修复 README；验证失败时继续修复");
    EXPECT_TRUE(result.completed) << ("verification-gated agent eventually completes");
    EXPECT_TRUE(result.answer == "失败后继续修复并验证完成")
        << ("premature final answer is not accepted");
    EXPECT_TRUE(result.turns == 6)
        << ("agent uses patch, failed verify, rejected final, patch, pass, final");
    EXPECT_TRUE(model.gate_seen())
        << ("scripted model receives the harness continuation requirement");
    EXPECT_TRUE(result.execution.file_changes == 2)
        << ("execution summary counts both repair attempts");
    EXPECT_TRUE(result.execution.command_calls == 2)
        << ("execution summary counts both verification attempts");
    EXPECT_TRUE(result.execution.commands_failed == 1)
        << ("execution summary records the first failed verification");
    EXPECT_TRUE(result.execution.commands_passed == 1)
        << ("execution summary records the final passing verification");
    EXPECT_TRUE(result.verification_status == "passed") << ("final verification status is passed");
    EXPECT_TRUE(read_text(workspace / "README.md") == "# Fixed\n")
        << ("second repair leaves the verified contents");
    EXPECT_TRUE(result.changes.files == std::vector<std::string>{"README.md"})
        << ("change journal collapses repeated edits into one file");
    EXPECT_TRUE(result.changes.unified_diff.find("-# Broken\n+# Fixed\n") != std::string::npos)
        << ("final diff compares the original baseline with verified contents");
    EXPECT_TRUE(result.changes.unified_diff.find("Almost") == std::string::npos)
        << ("intermediate failed contents do not pollute the final diff");
    EXPECT_TRUE(log.str().find("[继续]") != std::string::npos)
        << ("console trace records the rejected premature completion");
}

TEST(AgentLoopTest, LatestCommandControlsVerificationStatus) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Broken\n");
    const auto program = mint_test_executable_path().generic_string();

    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.allow_write = true,
                                                       .allowed_programs = {program},
                                                       .default_command_timeout_seconds = 5,
                                                       .max_command_timeout_seconds = 5});
    PassThenFailModel model(program);
    std::ostringstream log;
    mint::Agent agent(model, tools, log,
                      mint::AgentOptions{.max_turns = 4, .require_verification_after_write = true});

    const auto result = agent.run("修改后先验证成功，再模拟一次回归失败");
    EXPECT_TRUE(!result.completed)
        << ("a later failed command prevents completion despite an earlier pass");
    EXPECT_TRUE(result.verification_status == "failed")
        << ("the latest post-write command controls verification status");
    EXPECT_TRUE(result.execution.commands_passed == 1)
        << ("regression scenario records the initial pass");
    EXPECT_TRUE(result.execution.commands_failed == 1)
        << ("regression scenario records the later failure");
    EXPECT_TRUE(log.str().find("[继续]") != std::string::npos)
        << ("verification gate rejects completion after the later failure");
}

TEST(AgentLoopTest, DeniedCommandCannotSatisfyVerification) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = mint_test_executable_path().generic_string();
    write_text(workspace / "README.md", "# Broken\n");

    mint::ToolRegistry tools(
        workspace,
        mint::ToolRegistryOptions{
            .allow_write = true,
            .allowed_programs = {program},
            .default_command_timeout_seconds = 5,
            .max_command_timeout_seconds = 5,
            .command_approval = [](const mint::CommandApprovalRequest&) { return false; }});
    DeniedVerificationModel model(program);
    std::ostringstream log;
    mint::Agent agent(model, tools, log,
                      mint::AgentOptions{.max_turns = 3, .require_verification_after_write = true});
    const auto result = agent.run("修改并申请运行验证");
    EXPECT_TRUE(!result.completed && result.status == "max_turns")
        << ("approval denial prevents the model from ending the task");
    EXPECT_TRUE(result.verification_status == "denied")
        << ("verification state distinguishes approval denial from test failure");
    EXPECT_TRUE(result.execution.commands_denied == 1)
        << ("execution summary counts the denied command separately");
    EXPECT_TRUE(log.str().find("[继续]") != std::string::npos)
        << ("verification gate requests continuation after denial");
}

} // namespace
