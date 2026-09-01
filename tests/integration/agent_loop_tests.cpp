#include "mint/application/agent.hpp"
#include "mint/infrastructure/event_log.hpp"
#include "mint/infrastructure/model_provider_client.hpp"
#include "mint/infrastructure/session_store.hpp"
#include "mint/ports/model_client.hpp"
#include "mint/runtime/task_control.hpp"
#include "mint/tools/tool_registry.hpp"
#include "mint/version.hpp"

#include "agent/agent_context.hpp"
#include "agent/agent_execution.hpp"
#include "agent/agent_reporting.hpp"
#include "test_executable.hpp"
#include "test_workspace.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

#define MINT_EXPECT(condition, message) EXPECT_TRUE(condition) << (message)

using mint::test::read_text;
using mint::test::TemporaryDirectory;
using mint::test::write_text;

class ScriptedModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        MINT_EXPECT(tools.size() == 3, "agent exposes exactly three tools in v0.1");
        if (calls_++ == 0) {
            const mint::Json arguments = {{"path", "README.md"}};
            const mint::Json raw_call = {
                {"id", "test-read"},
                {"type", "function"},
                {"function", {{"name", "read_file"}, {"arguments", arguments.dump()}}}};
            return {.assistant_message = {{"role", "assistant"},
                                          {"content", nullptr},
                                          {"tool_calls", mint::Json::array({raw_call})}},
                    .text = {},
                    .tool_calls = {{"test-read", "read_file", arguments}}};
        }

        MINT_EXPECT(messages.back().at("role") == "tool", "tool result is appended to context");
        const auto result = mint::Json::parse(messages.back().at("content").get<std::string>());
        MINT_EXPECT(result.at("ok").get<bool>(), "tool result returned to model is successful");
        return {.assistant_message = {{"role", "assistant"}, {"content", "完成"}},
                .text = "完成",
                .tool_calls = {},
                .usage = {.available = true,
                          .prompt_tokens = 100,
                          .completion_tokens = 10,
                          .total_tokens = 110,
                          .cached_tokens = 80}};
    }

  private:
    int calls_ = 0;
};

class ContextBudgetModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json& messages, const mint::Json&) override {
        MINT_EXPECT(messages.dump().size() <= 16 * 1024,
                    "model context never exceeds configured byte budget");
        if (calls_ > 0) {
            bool found_summary = false;
            for (const auto& message : messages) {
                if (message.value("role", "") == "system" &&
                    message.value("content", "").find("Harness context summary") !=
                        std::string::npos) {
                    found_summary = true;
                }
            }
            MINT_EXPECT(found_summary, "compacted context carries an explicit harness summary");
        }
        if (calls_++ < 3) {
            const auto id = "large-read-" + std::to_string(calls_);
            const mint::Json arguments = {{"path", "large.txt"}};
            const mint::Json raw_call = {
                {"id", id},
                {"type", "function"},
                {"function", {{"name", "read_file"}, {"arguments", arguments.dump()}}}};
            return {.assistant_message = {{"role", "assistant"},
                                          {"content", nullptr},
                                          {"tool_calls", mint::Json::array({raw_call})}},
                    .text = {},
                    .tool_calls = {{id, "read_file", arguments}}};
        }
        return {.assistant_message = {{"role", "assistant"}, {"content", "上下文预算通过"}},
                .text = "上下文预算通过",
                .tool_calls = {}};
    }

  private:
    int calls_ = 0;
};

class ProviderBudgetModel final : public mint::ModelClient {
  public:
    mint::ModelRequestLimits request_limits(const mint::Json&) const override {
        return {.max_request_tokens = 8'000,
                .reserved_output_tokens = 1'024,
                .safety_margin_tokens = 256,
                .request_overhead_tokens = 512};
    }

    mint::ModelReply complete(const mint::Json& messages, const mint::Json&) override {
        constexpr std::size_t provider_context_bytes =
            (8'000 - 1'024 - 256 - 512) * mint::model_token_estimation::serialized_bytes_per_token;
        MINT_EXPECT(messages.dump().size() <= provider_context_bytes,
                    "provider request budget clamps a larger Agent byte budget");
        saw_compaction_ = saw_compaction_ ||
                          std::any_of(messages.begin(), messages.end(), [](const auto& message) {
                              return message.value("role", "") == "system" &&
                                     message.value("content", "").find("Harness context summary") !=
                                         std::string::npos;
                          });

        if (calls_++ < 3) {
            const auto id = "provider-budget-read-" + std::to_string(calls_);
            const mint::Json arguments = {{"path", "large.txt"}};
            const mint::Json raw_call = {
                {"id", id},
                {"type", "function"},
                {"function", {{"name", "read_file"}, {"arguments", arguments.dump()}}}};
            return {.assistant_message = {{"role", "assistant"},
                                          {"content", nullptr},
                                          {"tool_calls", mint::Json::array({raw_call})}},
                    .tool_calls = {{id, "read_file", arguments}}};
        }
        return {.assistant_message = {{"role", "assistant"}, {"content", "预算通过"}},
                .text = "预算通过"};
    }

    [[nodiscard]] bool saw_compaction() const noexcept {
        return saw_compaction_;
    }

  private:
    std::size_t calls_ = 0;
    bool saw_compaction_ = false;
};

class WritingModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        MINT_EXPECT(tools.size() == 6, "write-enabled agent exposes six tools");
        MINT_EXPECT(messages.at(0).at("content").get<std::string>().find("apply_patch") !=
                        std::string::npos,
                    "write-enabled system prompt explains apply_patch");

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

        MINT_EXPECT(messages.back().at("role") == "tool", "patch result is appended to context");
        const auto result = mint::Json::parse(messages.back().at("content").get<std::string>());
        MINT_EXPECT(result.at("ok").get<bool>(), "patch result returned to model is successful");
        return {.assistant_message = {{"role", "assistant"}, {"content", "修改完成"}},
                .text = "修改完成",
                .tool_calls = {}};
    }

  private:
    int calls_ = 0;
};

bool is_verification_ready(const mint::Json& message) {
    return message.value("role", "") == "user" && message.contains("content") &&
           message.at("content").is_string() &&
           message.at("content").get<std::string>().find("[Harness status]") != std::string::npos;
}

mint::Json latest_tool_result(const mint::Json& messages) {
    auto offset = std::size_t{0};
    if (is_verification_ready(messages.back())) {
        offset = 1;
    }
    const auto& message = messages.at(messages.size() - 1 - offset);
    MINT_EXPECT(message.at("role") == "tool", "script receives the latest tool result");
    return mint::Json::parse(message.at("content").get<std::string>());
}

std::size_t verification_ready_count(const mint::Json& messages) {
    return static_cast<std::size_t>(
        std::count_if(messages.begin(), messages.end(),
                      [](const mint::Json& message) { return is_verification_ready(message); }));
}

class PatchThenVerifyModel final : public mint::ModelClient {
  public:
    explicit PatchThenVerifyModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        MINT_EXPECT(tools.size() == 7, "write-and-command agent exposes seven tools");
        const auto system_prompt = messages.at(0).at("content").get<std::string>();
        MINT_EXPECT(system_prompt.find("apply_patch") != std::string::npos,
                    "validation system prompt explains apply_patch");
        MINT_EXPECT(system_prompt.find("run_command") != std::string::npos,
                    "validation system prompt explains run_command");

        if (calls_ == 0) {
            ++calls_;
            return tool_reply("e2e-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Broken\n"},
                               {"new_text", "# Fixed\n"}});
        }

        if (calls_ == 2) {
            MINT_EXPECT(is_verification_ready(messages.back()),
                        "passing verification appends a completion prompt");
        }
        const auto previous = latest_tool_result(messages);
        MINT_EXPECT(previous.at("ok").get<bool>(), "previous e2e tool result succeeded");

        if (calls_ == 1) {
            ++calls_;
            return tool_reply("e2e-verify", "run_command",
                              {{"program", program_},
                               {"args", mint::Json::array({"--command-helper", "verify",
                                                           "README.md", "# Fixed\n"})},
                               {"cwd", "."},
                               {"timeout_seconds", 5}});
        }

        MINT_EXPECT(previous.at("status") == "exited", "verification command exited normally");
        MINT_EXPECT(previous.at("exit_code") == 0, "verification command passed");
        MINT_EXPECT(previous.at("output").get<std::string>().find("verification passed") !=
                        std::string::npos,
                    "verification evidence is returned to the model");
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
            MINT_EXPECT(!tools.empty(), "the repair turn keeps workspace tools available");
            return tool_reply("limit-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Broken\n"},
                               {"new_text", "# Fixed\n"}});
        case 1:
            MINT_EXPECT(!tools.empty(), "the verification turn keeps command tools available");
            return tool_reply("limit-verify", "run_command",
                              {{"program", program_},
                               {"args", mint::Json::array({"--command-helper", "verify",
                                                           "README.md", "# Fixed\n"})},
                               {"cwd", "."},
                               {"timeout_seconds", 5}});
        default:
            MINT_EXPECT(tools.empty(), "the reserved final-answer turn cannot request more tools");
            MINT_EXPECT(is_verification_ready(messages.back()),
                        "the final-answer turn receives verified completion context");
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
        MINT_EXPECT(tools.size() == 7, "verification-gated agent exposes seven tools");
        const auto system_prompt = messages.at(0).at("content").get<std::string>();
        MINT_EXPECT(system_prompt.find("Harness policy requires verification") != std::string::npos,
                    "system prompt explains the required verification gate");

        switch (calls_++) {
        case 0:
            return tool_reply("retry-first-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Broken\n"},
                               {"new_text", "# Almost\n"}});
        case 1:
            MINT_EXPECT(latest_tool_result(messages).at("ok").get<bool>(),
                        "first patch succeeded before failed verification");
            return verification_call("retry-first-verify");
        case 2: {
            const auto failed = latest_tool_result(messages);
            MINT_EXPECT(failed.at("exit_code") == 9,
                        "first verification exposes the expected failure");
            return final_reply("错误地提前结束");
        }
        case 3:
            MINT_EXPECT(messages.back().at("role") == "user",
                        "harness gate appends a continuation requirement");
            MINT_EXPECT(messages.back().at("content").get<std::string>().find(
                            "unverified changes") != std::string::npos,
                        "continuation requirement explains unverified changes");
            gate_seen_ = true;
            return tool_reply("retry-second-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Almost\n"},
                               {"new_text", "# Fixed\n"}});
        case 4:
            MINT_EXPECT(latest_tool_result(messages).at("ok").get<bool>(),
                        "second patch succeeded after the gate");
            return verification_call("retry-second-verify");
        default: {
            MINT_EXPECT(is_verification_ready(messages.back()),
                        "repaired task receives the verified completion prompt");
            const auto passed = latest_tool_result(messages);
            MINT_EXPECT(passed.at("exit_code") == 0, "second verification passes after the repair");
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
            MINT_EXPECT(is_verification_ready(messages.back()) &&
                            verification_ready_count(messages) == 1,
                        "passing verification emits one completion prompt");
            MINT_EXPECT(latest_tool_result(messages).at("exit_code") == 0,
                        "initial verification passes before the later failure");
            return tool_reply("regression-fail", "run_command",
                              {{"program", program_},
                               {"args", mint::Json::array({"--command-helper", "fail"})},
                               {"cwd", "."},
                               {"timeout_seconds", 5}});
        default:
            MINT_EXPECT(verification_ready_count(messages) == 1,
                        "later failed command does not duplicate the completion prompt");
            MINT_EXPECT(latest_tool_result(messages).at("exit_code") == 7,
                        "later command exposes the regression failure");
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
            MINT_EXPECT(result.at("ok").get<bool>(), "restored patch succeeds before verification");
            return model_tool_reply("session-verify", "run_command",
                                    {{"program", program_},
                                     {"args", mint::Json::array({"--command-helper", "verify",
                                                                 "README.md", "# Fixed\n"})},
                                     {"cwd", "."},
                                     {"timeout_seconds", 5}});
        }
        MINT_EXPECT(is_verification_ready(messages.back()),
                    "resumed passing verification appends a completion prompt");
        MINT_EXPECT(result.at("exit_code") == 0, "resumed verification command passes");
        return {.assistant_message = {{"role", "assistant"}, {"content", "恢复后验证完成"}},
                .text = "恢复后验证完成",
                .tool_calls = {}};
    }

  private:
    std::string program_;
    int calls_ = 0;
};

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

TEST(AgentLoopTest, CompletesReadOnlyTask) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Loop test\n");

    mint::ToolRegistry tools(workspace);
    ScriptedModel model;
    std::ostringstream log;
    mint::Agent agent(model, tools, log);

    const auto result = agent.run("读取 README 后回答");
    MINT_EXPECT(result.completed, "agent reaches a final answer");
    MINT_EXPECT(result.answer == "完成", "agent returns model final answer");
    MINT_EXPECT(result.turns == 2, "agent performs tool turn then final turn");
    MINT_EXPECT(result.execution.tool_calls == 1, "agent summary counts the read tool call");
    MINT_EXPECT(result.execution.successful_tool_calls == 1,
                "agent summary counts the successful read tool");
    MINT_EXPECT(result.model.calls == 2 && result.model.attempts == 2 &&
                    result.model.usage_reports == 1 && result.model.total_tokens == 110 &&
                    result.model.cached_tokens == 80,
                "agent aggregates model attempts and token usage across turns");
    MINT_EXPECT(log.str().find("read_file") != std::string::npos, "agent log shows tool call");
    MINT_EXPECT(log.str().find("缓存 80，命中 80%") != std::string::npos,
                "agent log shows observable prompt cache usage");
}

TEST(AgentLoopTest, EnforcesContextBudget) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "large.txt", std::string(128 * 1024, 'x'));

    mint::ToolRegistry tools(workspace);
    ContextBudgetModel model;
    std::ostringstream output;
    mint::Agent agent(model, tools, output,
                      mint::AgentOptions{.max_turns = 6, .max_context_bytes = 16 * 1024});
    const auto result = agent.run("重复读取大文件并验证上下文压缩");
    MINT_EXPECT(result.completed && result.answer == "上下文预算通过",
                "agent completes after multiple compacted large tool results");
    MINT_EXPECT(result.execution.tool_calls == 3,
                "context compaction does not alter executed tool history");
}

TEST(AgentLoopTest, EnforcesProviderRequestBudgetBelowManagedContextBudget) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "large.txt", std::string(128 * 1024, 'x'));

    mint::ToolRegistry tools(workspace);
    ProviderBudgetModel model;
    std::ostringstream output;
    mint::Agent agent(model, tools, output,
                      mint::AgentOptions{.max_turns = 6, .max_context_bytes = 128 * 1024});
    const auto result = agent.run("重复读取大文件并服从模型请求预算");

    MINT_EXPECT(result.completed && result.answer == "预算通过",
                "agent completes under the provider request budget");
    MINT_EXPECT(result.execution.tool_calls == 3 && model.saw_compaction(),
                "provider budget compacts history without changing executed tools");
}

TEST(AgentLoopTest, ContextCompactionPreservesFailureEvidence) {
    const mint::Json arguments = {{"path", "large.txt"}};
    const mint::Json tool_call = {
        {"id", "failed-read"},
        {"type", "function"},
        {"function", {{"name", "read_file"}, {"arguments", arguments.dump()}}}};
    mint::Json large_diagnostics = mint::Json::array();
    for (int index = 0; index < 2000; ++index) {
        large_diagnostics.push_back(index);
    }
    const mint::Json failed_result = {{"ok", false},
                                      {"status", "failed"},
                                      {"error", "compile failed"},
                                      {"resource_limited", true},
                                      {"resource_limit", "memory"},
                                      {"resource_limits", {{"memory_bytes", 134217728}}},
                                      {"diagnostics", std::move(large_diagnostics)}};
    const mint::Json messages = mint::Json::array(
        {{{"role", "system"}, {"content", "system"}},
         {{"role", "user"}, {"content", "task"}},
         {{"role", "assistant"}, {"content", nullptr}, {"tool_calls", {tool_call}}},
         {{"role", "tool"}, {"tool_call_id", "failed-read"}, {"content", failed_result.dump()}}});

    const auto compacted = mint::agent_detail::compact_context(messages, 2048);
    MINT_EXPECT(compacted.payloads_compacted,
                "oversized latest tool group uses payload compaction");

    const auto tool_message =
        std::find_if(compacted.messages.begin(), compacted.messages.end(),
                     [](const mint::Json& message) { return message.value("role", "") == "tool"; });
    MINT_EXPECT(tool_message != compacted.messages.end(),
                "compaction retains the latest assistant/tool call pair");
    const auto retained = mint::Json::parse(tool_message->at("content").get<std::string>());
    MINT_EXPECT(retained.at("context_compacted").get<bool>() && !retained.at("ok").get<bool>() &&
                    retained.at("status") == "failed" && retained.at("error") == "compile failed" &&
                    retained.at("resource_limited").get<bool>() &&
                    retained.at("resource_limit") == "memory" &&
                    retained.at("resource_limits").at("memory_bytes") == 134217728 &&
                    !retained.contains("diagnostics"),
                "compaction omits bulk payload without changing failed evidence into success");
}

TEST(AgentLoopTest, ContextCompactionPreservesLatestProviderContinuationState) {
    const mint::Json raw_content = mint::Json::array(
        {{{"type", "thinking"}, {"thinking", "inspect"}, {"signature", std::string(256, 's')}},
         {{"type", "tool_use"},
          {"id", "toolu_1"},
          {"name", "read_file"},
          {"input", {{"path", "README.md"}}}}});
    const mint::Json tool_call = {
        {"id", "toolu_1"},
        {"type", "function"},
        {"function", {{"name", "read_file"}, {"arguments", R"({"path":"README.md"})"}}}};
    const mint::Json messages = mint::Json::array(
        {{{"role", "system"}, {"content", "system"}},
         {{"role", "user"}, {"content", "task"}},
         {{"role", "assistant"},
          {"content", nullptr},
          {"tool_calls", {tool_call}},
          {"_mint_provider_state", {{"adapter", "anthropic_messages"}, {"content", raw_content}}}},
         {{"role", "tool"},
          {"tool_call_id", "toolu_1"},
          {"content", mint::Json({{"ok", true}, {"content", std::string(12'000, 'x')}}).dump()}}});

    const auto compacted = mint::agent_detail::compact_context(messages, 2048);
    const auto assistant = std::find_if(
        compacted.messages.begin(), compacted.messages.end(),
        [](const mint::Json& message) { return message.value("role", "") == "assistant"; });
    ASSERT_NE(assistant, compacted.messages.end());
    EXPECT_EQ(assistant->at("_mint_provider_state").at("content"), raw_content);

    auto oversized = messages;
    oversized.at(2).at("_mint_provider_state").at("content").at(0)["signature"] =
        std::string(4096, 's');
    EXPECT_THROW((void)mint::agent_detail::compact_context(oversized, 2048), std::runtime_error);
}

TEST(AgentLoopTest, ContextCompactionPreservesSignedChatToolCalls) {
    const mint::Json assistant = {
        {"role", "assistant"},
        {"content", nullptr},
        {"reasoning_content", "inspect before calling"},
        {"tool_calls",
         mint::Json::array(
             {{{"id", "call_1"},
               {"type", "function"},
               {"function", {{"name", "read_file"}, {"arguments", R"({"path":"README.md"})"}}},
               {"extra_content", {{"google", {{"thought_signature", std::string(256, 's')}}}}}}})}};
    const mint::Json messages = mint::Json::array(
        {{{"role", "system"}, {"content", "system"}},
         {{"role", "user"}, {"content", "task"}},
         assistant,
         {{"role", "tool"},
          {"tool_call_id", "call_1"},
          {"content", mint::Json({{"ok", true}, {"content", std::string(12'000, 'x')}}).dump()}}});

    const auto compacted = mint::agent_detail::compact_context(messages, 2048);
    const auto retained = std::find_if(
        compacted.messages.begin(), compacted.messages.end(),
        [](const mint::Json& message) { return message.value("role", "") == "assistant"; });
    ASSERT_NE(retained, compacted.messages.end());
    EXPECT_EQ(*retained, assistant);

    auto oversized = messages;
    oversized.at(2).at("tool_calls").at(0).at("function")["arguments"] = std::string(4096, 'a');
    EXPECT_THROW((void)mint::agent_detail::compact_context(oversized, 2048), std::runtime_error);
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
    MINT_EXPECT(machine.at("schema_version") == 1 && machine.at("status") == "completed" &&
                    machine.at("completed").get<bool>(),
                "machine result has a versioned completion contract");
    MINT_EXPECT(machine.at("execution").at("tool_calls") == 3,
                "machine result exposes the execution summary");
    MINT_EXPECT(machine.at("verification_status") == "not_required",
                "machine result exposes explicit verification state");

    std::ifstream input(event_path, std::ios::binary);
    std::string raw{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    MINT_EXPECT(raw.find("Event secret body") == std::string::npos &&
                    raw.find("\"query\":\"Agent\"") == std::string::npos,
                "JSONL trace omits file contents, search text and raw tool output");
    std::istringstream lines(raw);
    std::string line;
    std::size_t expected_sequence = 1;
    std::vector<std::string> types;
    while (std::getline(lines, line)) {
        const auto event = mint::Json::parse(line);
        MINT_EXPECT(event.at("schema_version") == 1,
                    "every JSONL event carries its schema version");
        MINT_EXPECT(event.at("seq") == expected_sequence++, "JSONL event sequence is monotonic");
        types.push_back(event.at("type").get<std::string>());
    }
    MINT_EXPECT(!types.empty() && types.front() == "task_started" &&
                    types.back() == "task_finished",
                "event trace brackets the complete task lifecycle");
    MINT_EXPECT(std::find(types.begin(), types.end(), "tool_started") != types.end() &&
                    std::find(types.begin(), types.end(), "tool_completed") != types.end(),
                "event trace records sanitized tool lifecycle events");
}

TEST(RuntimeFilesTest, ProtectsRuntimeFiles) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "existing.txt";
    write_text(target, "must not be overwritten\n");

    const auto append_path = temporary.path() / "append-events.jsonl";
    {
        mint::EventLog first(append_path);
        first.emit("first");
    }
    {
        mint::EventLog second(append_path, true);
        second.emit("second");
    }
    std::ifstream appended_input(append_path, std::ios::binary);
    std::string first_line;
    std::string second_line;
    std::getline(appended_input, first_line);
    std::getline(appended_input, second_line);
    MINT_EXPECT(mint::Json::parse(first_line).at("seq") == 1 &&
                    mint::Json::parse(second_line).at("seq") == 2,
                "resumed JSONL logging continues the prior sequence");

    const auto partial_path = temporary.path() / "partial-events.jsonl";
    write_text(partial_path, R"({"schema_version":1,"seq":4,"type":"stable","data":{}})"
                             "\n{\"schema_version\":1,\"seq\":5");
    {
        mint::EventLog recovered(partial_path, true);
        recovered.emit("after_crash");
    }
    std::ifstream recovered_input(partial_path, std::ios::binary);
    std::string recovered_line;
    std::vector<std::string> recovered_lines;
    while (std::getline(recovered_input, recovered_line)) {
        recovered_lines.push_back(recovered_line);
    }
    MINT_EXPECT(recovered_lines.size() == 3 &&
                    mint::Json::parse(recovered_lines.back()).at("seq") == 5,
                "JSONL recovery separates a crash-truncated final line from new events");

    std::error_code symlink_error;
    const auto event_link = temporary.path() / "events-link.jsonl";
    std::filesystem::create_symlink(target, event_link, symlink_error);
    if (!symlink_error) {
        bool event_rejected = false;
        bool session_rejected = false;
        try {
            mint::EventLog events(event_link);
        } catch (const std::invalid_argument&) {
            event_rejected = true;
        }
        const auto session_link = temporary.path() / "session-link.json";
        std::filesystem::create_symlink(target, session_link, symlink_error);
        try {
            mint::SessionStore session(session_link);
        } catch (const std::invalid_argument&) {
            session_rejected = true;
        }
        MINT_EXPECT(event_rejected && session_rejected,
                    "runtime files reject symbolic-link targets");
        MINT_EXPECT(read_text(target) == "must not be overwritten\n",
                    "runtime symlink rejection leaves the target unchanged");
    }

    std::error_code hard_link_error;
    const auto hard_link = temporary.path() / "events-hardlink.jsonl";
    std::filesystem::create_hard_link(target, hard_link, hard_link_error);
    if (!hard_link_error) {
        bool rejected = false;
        try {
            mint::EventLog events(hard_link);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        MINT_EXPECT(rejected, "event log rejects an existing file with multiple hard links");
        MINT_EXPECT(read_text(target) == "must not be overwritten\n",
                    "hard-link rejection leaves the shared inode unchanged");
    }

    const auto existing_directory = temporary.path() / "runtime-directory";
    std::filesystem::create_directory(existing_directory);
    bool event_directory_rejected = false;
    bool session_directory_rejected = false;
    try {
        mint::EventLog events(existing_directory);
    } catch (const std::invalid_argument&) {
        event_directory_rejected = true;
    }
    try {
        mint::SessionStore session(existing_directory);
    } catch (const std::invalid_argument&) {
        session_directory_rejected = true;
    }
    MINT_EXPECT(event_directory_rejected && session_directory_rejected,
                "runtime output paths reject existing non-regular files");
}

TEST(AgentLoopTest, CompletesWriteTask) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Before\n");

    mint::ToolRegistry tools(workspace, mint::ToolRegistryOptions{.allow_write = true});
    WritingModel model;
    std::ostringstream log;
    mint::Agent agent(model, tools, log);

    const auto result = agent.run("修改 README 标题");
    MINT_EXPECT(result.completed, "write-enabled agent reaches a final answer");
    MINT_EXPECT(result.answer == "修改完成", "write-enabled agent returns final answer");
    MINT_EXPECT(read_text(workspace / "README.md") == "# After\n",
                "agent loop executes apply_patch");
    MINT_EXPECT(result.execution.file_changes == 1,
                "write agent summary counts the file modification");
    MINT_EXPECT(result.changes.files == std::vector<std::string>{"README.md"},
                "write agent result exposes the changed file list");
    MINT_EXPECT(result.changes.unified_diff.find("-# Before\n+# After\n") != std::string::npos,
                "write agent result exposes the unified diff");
    MINT_EXPECT(result.verification_status == "not_run",
                "unverified write is marked not_run when the gate is disabled");
    const auto write_log = log.str();
    MINT_EXPECT(write_log.find("README.md") != std::string::npos,
                "patch log includes the target path");
    const auto before_final = write_log.substr(0, write_log.find("[最终回答]"));
    MINT_EXPECT(before_final.find("# Before") == std::string::npos,
                "patch log does not dump old file contents");
    MINT_EXPECT(before_final.find("# After") == std::string::npos,
                "patch log does not dump new file contents");
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
    MINT_EXPECT(result.completed, "patch-then-verify agent reaches a final answer");
    MINT_EXPECT(result.answer == "修改并验证完成",
                "patch-then-verify agent returns the verified answer");
    MINT_EXPECT(result.turns == 3, "agent performs patch, verification command, then final answer");
    MINT_EXPECT(result.execution.tool_calls == 2,
                "end-to-end summary counts patch and command tools");
    MINT_EXPECT(result.execution.file_changes == 1, "end-to-end summary counts the patch");
    MINT_EXPECT(result.execution.command_calls == 1,
                "end-to-end summary counts the verification command");
    MINT_EXPECT(result.execution.commands_passed == 1,
                "end-to-end summary records the passing verification");
    MINT_EXPECT(result.execution.commands_failed == 0, "end-to-end summary has no failed commands");
    MINT_EXPECT(result.verification_status == "passed",
                "end-to-end result records passed verification");
    MINT_EXPECT(result.changes.files == std::vector<std::string>{"README.md"},
                "end-to-end result lists the modified file");
    MINT_EXPECT(read_text(workspace / "README.md") == "# Fixed\n",
                "end-to-end loop leaves the requested file change");
    MINT_EXPECT(log.str().find("apply_patch") != std::string::npos,
                "end-to-end log records the patch tool");
    MINT_EXPECT(log.str().find("run_command") != std::string::npos,
                "end-to-end log records the verification command");
    const auto e2e_log = log.str();
    const auto e2e_before_final = e2e_log.substr(0, e2e_log.find("[最终回答]"));
    MINT_EXPECT(e2e_before_final.find("# Fixed") == std::string::npos,
                "end-to-end tool-call logs do not dump patch or command arguments");
    MINT_EXPECT(e2e_log.find("+# Fixed") != std::string::npos,
                "end-to-end final state prints the audited diff");
    std::ifstream event_input(event_path, std::ios::binary);
    const std::string event_log{std::istreambuf_iterator<char>(event_input),
                                std::istreambuf_iterator<char>()};
    MINT_EXPECT(event_log.find("\"type\":\"verification_ready\"") != std::string::npos,
                "event trace records that verification is ready for completion");
    MINT_EXPECT(event_log.find("Harness status") == std::string::npos,
                "event trace does not copy the model-facing completion prompt");
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

    MINT_EXPECT(result.completed && result.status == "completed",
                "verified work receives a reserved final-answer turn");
    MINT_EXPECT(result.answer == "轮数边界内验证完成",
                "the reserved turn preserves the model's final answer");
    MINT_EXPECT(result.turns == 3 && model.final_answer_turn_seen(),
                "the action budget excludes one tool-free final-answer turn");
    MINT_EXPECT(result.verification_status == "passed",
                "the reserved turn cannot weaken the verification gate");
    MINT_EXPECT(read_text(workspace / "README.md") == "# Fixed\n",
                "the verified workspace change remains intact");
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
    MINT_EXPECT(result.completed, "verification-gated agent eventually completes");
    MINT_EXPECT(result.answer == "失败后继续修复并验证完成",
                "premature final answer is not accepted");
    MINT_EXPECT(result.turns == 6,
                "agent uses patch, failed verify, rejected final, patch, pass, final");
    MINT_EXPECT(model.gate_seen(), "scripted model receives the harness continuation requirement");
    MINT_EXPECT(result.execution.file_changes == 2,
                "execution summary counts both repair attempts");
    MINT_EXPECT(result.execution.command_calls == 2,
                "execution summary counts both verification attempts");
    MINT_EXPECT(result.execution.commands_failed == 1,
                "execution summary records the first failed verification");
    MINT_EXPECT(result.execution.commands_passed == 1,
                "execution summary records the final passing verification");
    MINT_EXPECT(result.verification_status == "passed", "final verification status is passed");
    MINT_EXPECT(read_text(workspace / "README.md") == "# Fixed\n",
                "second repair leaves the verified contents");
    MINT_EXPECT(result.changes.files == std::vector<std::string>{"README.md"},
                "change journal collapses repeated edits into one file");
    MINT_EXPECT(result.changes.unified_diff.find("-# Broken\n+# Fixed\n") != std::string::npos,
                "final diff compares the original baseline with verified contents");
    MINT_EXPECT(result.changes.unified_diff.find("Almost") == std::string::npos,
                "intermediate failed contents do not pollute the final diff");
    MINT_EXPECT(log.str().find("[继续]") != std::string::npos,
                "console trace records the rejected premature completion");
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
    MINT_EXPECT(!result.completed,
                "a later failed command prevents completion despite an earlier pass");
    MINT_EXPECT(result.verification_status == "failed",
                "the latest post-write command controls verification status");
    MINT_EXPECT(result.execution.commands_passed == 1,
                "regression scenario records the initial pass");
    MINT_EXPECT(result.execution.commands_failed == 1,
                "regression scenario records the later failure");
    MINT_EXPECT(log.str().find("[继续]") != std::string::npos,
                "verification gate rejects completion after the later failure");
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
    MINT_EXPECT(!result.completed && result.status == "max_turns",
                "approval denial prevents the model from ending the task");
    MINT_EXPECT(result.verification_status == "denied",
                "verification state distinguishes approval denial from test failure");
    MINT_EXPECT(result.execution.commands_denied == 1,
                "execution summary counts the denied command separately");
    MINT_EXPECT(log.str().find("[继续]") != std::string::npos,
                "verification gate requests continuation after denial");
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

    MINT_EXPECT(!result.completed && result.status == "cancelled",
                "agent exposes an explicit cancelled terminal state");
    MINT_EXPECT(result.stop_reason == "user_cancelled", "cancelled result records the stop reason");
    MINT_EXPECT(result.execution.commands_cancelled == 1,
                "execution summary counts the cancelled child command");
    MINT_EXPECT(elapsed < 1500, "agent cancellation terminates the child process group promptly");
    const auto machine = mint::agent_result_to_json(result);
    MINT_EXPECT(machine.at("status") == "cancelled" && !machine.at("completed").get<bool>(),
                "machine result preserves cancellation without pretending completion");
}

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
    MINT_EXPECT(interrupted.status == "cancelled" && !interrupted.completed,
                "task cancellation stops the first invocation explicitly");
    MINT_EXPECT(read_text(workspace / "README.md") == "# Broken\n",
                "pending tool is checkpointed before execution when cancellation arrives");
    const auto checkpoint = session.load();
    MINT_EXPECT(checkpoint.at("schema_version") == mint::session_schema_version &&
                    checkpoint.at("status") == "cancelled" &&
                    checkpoint.at("pending_tool_calls").size() == 1 &&
                    checkpoint.at("in_flight_tool_call").is_null(),
                "session stores the pending call at a stable resume point");
    MINT_EXPECT(checkpoint.at("model").at("response_header_max_request_tokens").is_null() &&
                    checkpoint.at("model").at("request_token_estimate_bytes_per_token") == 2,
                "session preserves an explicitly unknown provider header budget");

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
    MINT_EXPECT(rejected_incomplete_current,
                "current schema rejects a checkpoint missing its durable in-flight marker");

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
    MINT_EXPECT(rejected_tool_limit_change,
                "resume rejects tool budget changes that could alter pending work");

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
    MINT_EXPECT(blocked_inflight_replay,
                "resume blocks an ambiguous side-effecting in-flight tool by default");

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
    MINT_EXPECT(
        retry_result.completed && read_text(workspace / "README.md") == "# Fixed\n",
        "explicit retry authorization replays the in-flight tool and completes verification");
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
    MINT_EXPECT(migrated.completed &&
                    legacy_session.load().at("schema_version") == mint::session_schema_version,
                "current mint restores a v2 checkpoint and rewrites it with the current schema");
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
    MINT_EXPECT(
        migrated_v4.completed &&
            legacy_v4_session.load().at("schema_version") == mint::session_schema_version &&
            legacy_v4_session.load().at("change_journal").at("schema_version") ==
                mint::workspace_change_schema_version,
        "current mint restores an untainted v4 checkpoint and rewrites both safety schemas");
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
    MINT_EXPECT(rejected_policy_downgrade,
                "resume rejects a different per-command approval policy");

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
    MINT_EXPECT(resumed.completed && resumed.status == "completed",
                "resumed session executes the pending patch and reaches completion");
    MINT_EXPECT(resumed.answer == "恢复后验证完成" && resumed.turns == 3,
                "resume preserves prior turns and continues the original conversation");
    MINT_EXPECT(resumed.execution.file_changes == 1 && resumed.execution.commands_passed == 1,
                "resume preserves and extends the execution summary");
    MINT_EXPECT(resumed.verification_status == "passed",
                "resumed task still satisfies the verification gate");
    MINT_EXPECT(read_text(workspace / "README.md") == "# Fixed\n",
                "restored pending patch changes the workspace exactly once");
    MINT_EXPECT(resumed.changes.unified_diff.find("-# Broken\n+# Fixed\n") != std::string::npos,
                "restored change journal preserves the original-to-final diff");
    MINT_EXPECT(session.load().at("status") == "completed",
                "final stable checkpoint records terminal completion");
}

} // namespace

#undef MINT_EXPECT
