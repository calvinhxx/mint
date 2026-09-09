#include "mint/application/agent.hpp"
#include "mint/infrastructure/event_log.hpp"
#include "mint/infrastructure/model_provider_client.hpp"
#include "mint/runtime/task_control.hpp"
#include "mint/tools/tool_registry.hpp"

#include "agent/agent_execution.hpp"
#include "agent/agent_reporting.hpp"
#include "agent_test_models.hpp"
#include "test_executable.hpp"
#include "test_workspace.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using mint::test::model_tool_reply;
using mint::test::read_text;
using mint::test::ScriptedModel;
using mint::test::TemporaryDirectory;
using mint::test::write_text;

class LongCommandModel final : public mint::ModelClient {
  public:
    explicit LongCommandModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        if (calls_++ != 0) {
            throw std::runtime_error("cancelled agent must not call the model again");
        }
        return model_tool_reply("cancel-command", "run_command",
                                {{"program", program_},
                                 {"args", mint::Json::array({"--command-helper", "sleep"})},
                                 {"cwd", "."},
                                 {"timeout_seconds", 5}});
    }

  private:
    std::string program_;
    int calls_ = 0;
};

TEST(AgentLoopTest, CompletesReadOnlyTask) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Loop test\n");

    mint::ToolRegistry tools(workspace);
    ScriptedModel model;
    std::ostringstream log;
    mint::Agent agent(model, tools, log);

    const auto result = agent.run("读取 README 后回答");
    EXPECT_TRUE(result.completed) << ("agent reaches a final answer");
    EXPECT_TRUE(result.answer == "完成") << ("agent returns model final answer");
    EXPECT_TRUE(result.turns == 2) << ("agent performs tool turn then final turn");
    EXPECT_TRUE(result.execution.tool_calls == 1) << ("agent summary counts the read tool call");
    EXPECT_TRUE(result.execution.successful_tool_calls == 1)
        << ("agent summary counts the successful read tool");
    EXPECT_TRUE(result.model.calls == 2 && result.model.attempts == 2 &&
                result.model.usage_reports == 1 && result.model.total_tokens == 110 &&
                result.model.cached_tokens == 80)
        << ("agent aggregates model attempts and token usage across turns");
    EXPECT_TRUE(log.str().find("read_file") != std::string::npos) << ("agent log shows tool call");
    EXPECT_TRUE(log.str().find("缓存 80，命中 80%") != std::string::npos)
        << ("agent log shows observable prompt cache usage");
}

TEST(AgentStateTest, SafeToolResultRetainsBoundedFailureReason) {
    std::string long_error = "文件不存在: ";
    long_error.append(900, 'x');
    const auto summary = mint::agent_detail::safe_tool_result(
        mint::Json{{"ok", false}, {"status", "failed"}, {"error", long_error}}.dump());

    ASSERT_TRUE(summary.at("ok").is_boolean());
    EXPECT_FALSE(summary.at("ok").get<bool>());
    EXPECT_EQ(summary.at("status"), "failed");
    ASSERT_TRUE(summary.at("error").is_string());
    const auto retained = summary.at("error").get<std::string>();
    EXPECT_TRUE(retained.starts_with("文件不存在: "));
    EXPECT_TRUE(retained.ends_with("…[已截断]"));
    EXPECT_LE(retained.size(), 512);
    EXPECT_LT(retained.size(), long_error.size());

    const auto malformed = mint::agent_detail::safe_tool_result("not-json");
    EXPECT_TRUE(malformed.at("parse_error").get<bool>());
    EXPECT_FALSE(malformed.contains("ok"));
}

TEST(AgentStateTest, ApprovalCancellationAndProtocolFailureNeverCountAsUserDenials) {
    const mint::ToolCall command{"approval-result", "run_command", {{"program", "ctest"}}};
    mint::ExecutionSummary summary;
    const mint::Json cancelled{{"ok", true},
                               {"status", "cancelled"},
                               {"cancelled", true},
                               {"approval_decision_source", "run_cancelled"},
                               {"error", "运行已取消，命令未执行"}};
    mint::agent_detail::record_execution(summary, command, cancelled.dump());
    EXPECT_EQ(summary.commands_denied, 0);
    EXPECT_EQ(summary.commands_cancelled, 1);

    const mint::Json unresolved{{"ok", false},
                                {"status", "failed"},
                                {"approval_decision_source", "interaction_closed"},
                                {"error", "审批未完成，命令未执行"}};
    mint::agent_detail::record_execution(summary, command, unresolved.dump());
    EXPECT_EQ(summary.commands_denied, 0);
    EXPECT_EQ(summary.commands_failed, 1);

    const auto persisted = mint::agent_detail::safe_tool_result(unresolved.dump());
    EXPECT_EQ(persisted.at("approval_decision_source"), "interaction_closed");
}

TEST(AgentLoopTest, EmitsEventLogAndMachineResult) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto event_path = temporary.path() / "events.jsonl";
    write_text(workspace / "README.md", "# Event secret body\n");

    mint::ToolRegistry tools(workspace);
    mint::DemoModelClient model;
    mint::EventLog events(event_path);
    std::ostringstream log;
    mint::Agent agent(model, tools, log, {}, mint::AgentServices{.event_sink = &events});
    const auto result = agent.run("读取 README 后回答");
    const auto machine = mint::agent_result_to_json(result);
    EXPECT_TRUE(machine.at("schema_version") == 1 && machine.at("status") == "completed" &&
                machine.at("completed").get<bool>())
        << ("machine result has a versioned completion contract");
    EXPECT_TRUE(machine.at("execution").at("tool_calls") == 3)
        << ("machine result exposes the execution summary");
    EXPECT_TRUE(machine.at("verification_status") == "not_required")
        << ("machine result exposes explicit verification state");

    std::ifstream input(event_path, std::ios::binary);
    std::string raw{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    EXPECT_TRUE(raw.find("Event secret body") == std::string::npos &&
                raw.find("\"query\":\"Agent\"") == std::string::npos)
        << ("JSONL trace omits file contents, search text and raw tool output");
    std::istringstream lines(raw);
    std::string line;
    std::size_t expected_sequence = 1;
    std::vector<std::string> types;
    while (std::getline(lines, line)) {
        const auto event = mint::Json::parse(line);
        EXPECT_TRUE(event.at("schema_version") == 1)
            << ("every JSONL event carries its schema version");
        EXPECT_TRUE(event.at("seq") == expected_sequence++)
            << ("JSONL event sequence is monotonic");
        types.push_back(event.at("type").get<std::string>());
    }
    EXPECT_TRUE(!types.empty() && types.front() == "task_started" &&
                types.back() == "task_finished")
        << ("event trace brackets the complete task lifecycle");
    EXPECT_TRUE(std::find(types.begin(), types.end(), "tool_started") != types.end() &&
                std::find(types.begin(), types.end(), "tool_completed") != types.end())
        << ("event trace records sanitized tool lifecycle events");
}

TEST(AgentLoopTest, CancellationStopsRunningCommand) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = mint_test_executable_path().generic_string();
    auto control = std::make_shared<mint::TaskControl>();

    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.allowed_programs = {program},
                                                       .default_command_timeout_seconds = 5,
                                                       .max_command_timeout_seconds = 5,
                                                       .task_control = control});
    LongCommandModel model(program);
    std::ostringstream log;
    mint::Agent agent(model, tools, log, mint::AgentOptions{.max_turns = 4},
                      mint::AgentServices{.stop_token = control.get()});

    std::thread canceller([control]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        control->request_cancel();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto result = agent.run("运行一个会被取消的命令");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    canceller.join();

    EXPECT_TRUE(!result.completed && result.status == "cancelled")
        << ("agent exposes an explicit cancelled terminal state");
    EXPECT_TRUE(result.stop_reason == "user_cancelled")
        << ("cancelled result records the stop reason");
    EXPECT_TRUE(result.execution.commands_cancelled == 1)
        << ("execution summary counts the cancelled child command");
    EXPECT_TRUE(elapsed < 1500)
        << ("agent cancellation terminates the child process group promptly");
    const auto machine = mint::agent_result_to_json(result);
    EXPECT_TRUE(machine.at("status") == "cancelled" && !machine.at("completed").get<bool>())
        << ("machine result preserves cancellation without pretending completion");
}

} // namespace
