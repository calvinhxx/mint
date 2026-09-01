#include "mint/application/agent.hpp"
#include "mint/infrastructure/event_log.hpp"
#include "mint/infrastructure/session_store.hpp"
#include "mint/ports/model_client.hpp"
#include "mint/tools/tool_registry.hpp"

#include "agent/agent_model_summary.hpp"
#include "test_workspace.hpp"

#include <cstddef>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace {

using mint::test::read_text;
using mint::test::TemporaryDirectory;
using mint::test::write_text;

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

mint::ModelReply with_reported_usage(mint::ModelReply reply, std::size_t total_tokens) {
    reply.usage = {.available = true,
                   .prompt_tokens = total_tokens,
                   .completion_tokens = 0,
                   .total_tokens = total_tokens,
                   .cached_tokens = 0};
    return reply;
}

class ExactBoundaryToolModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        ++calls_;
        return with_reported_usage(model_tool_reply("budget-boundary-patch", "apply_patch",
                                                    {{"path", "README.md"},
                                                     {"operation", "replace"},
                                                     {"old_text", "# Before\n"},
                                                     {"new_text", "# After\n"}}),
                                   100);
    }

    [[nodiscard]] std::size_t calls() const noexcept {
        return calls_;
    }

  private:
    std::size_t calls_ = 0;
};

class ExactBoundaryFinalModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        mint::ModelReply reply{
            .assistant_message = {{"role", "assistant"}, {"content", "边界内完成"}},
            .text = "边界内完成"};
        return with_reported_usage(std::move(reply), 100);
    }
};

class CumulativeBudgetModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        if (calls_++ == 0) {
            return with_reported_usage(
                model_tool_reply("budget-read", "read_file", {{"path", "README.md"}}), 60);
        }
        return with_reported_usage(model_tool_reply("over-budget-patch", "apply_patch",
                                                    {{"path", "README.md"},
                                                     {"operation", "replace"},
                                                     {"old_text", "# Before\n"},
                                                     {"new_text", "# After\n"}}),
                                   50);
    }

  private:
    std::size_t calls_ = 0;
};

class MissingUsageModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        if (calls_++ == 0) {
            return model_tool_reply("unreported-patch", "apply_patch",
                                    {{"path", "README.md"},
                                     {"operation", "replace"},
                                     {"old_text", "# Before\n"},
                                     {"new_text", "# After\n"}});
        }
        return {.assistant_message = {{"role", "assistant"}, {"content", "完成"}}, .text = "完成"};
    }

  private:
    std::size_t calls_ = 0;
};

class ResumeBudgetFirstModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        return with_reported_usage(
            model_tool_reply("resume-budget-read", "read_file", {{"path", "README.md"}}), 60);
    }
};

class ResumeBudgetSecondModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        return with_reported_usage(model_tool_reply("resume-over-budget-patch", "apply_patch",
                                                    {{"path", "README.md"},
                                                     {"operation", "replace"},
                                                     {"old_text", "# Before\n"},
                                                     {"new_text", "# After\n"}}),
                                   50);
    }
};

TEST(AgentTokenBudgetTest, StopsBeforeToolAtExactTaskTokenBoundary) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Before\n");

    mint::ToolRegistry tools(workspace, mint::ToolRegistryOptions{.allow_write = true});
    ExactBoundaryToolModel model;
    std::ostringstream output;
    mint::Agent agent(model, tools, output, mint::AgentOptions{.max_total_tokens = 100});
    const auto result = agent.run("修改 README");

    EXPECT_EQ(result.status, "budget_exhausted");
    EXPECT_EQ(result.stop_reason, "max_total_tokens_exhausted");
    EXPECT_FALSE(result.completed);
    EXPECT_EQ(result.execution.tool_calls, 0U);
    EXPECT_EQ(result.model.total_tokens, 100U);
    EXPECT_EQ(model.calls(), 1U);
    EXPECT_EQ(read_text(workspace / "README.md"), "# Before\n");

    const auto machine = mint::agent_result_to_json(result);
    const auto& budget = machine.at("model").at("token_budget");
    EXPECT_EQ(budget.at("max_total_tokens"), 100U);
    EXPECT_EQ(budget.at("reported_total_tokens"), 100U);
    EXPECT_EQ(budget.at("usage_coverage"), "complete");
    EXPECT_EQ(budget.at("enforcement"), "reported_usage");
    EXPECT_TRUE(budget.at("exhausted").get<bool>());
}

TEST(AgentTokenBudgetTest, AcceptsFinalAnswerAtExactTaskTokenBoundary) {
    TemporaryDirectory temporary;
    mint::ToolRegistry tools(temporary.path() / "workspace");
    ExactBoundaryFinalModel model;
    std::ostringstream output;
    mint::Agent agent(model, tools, output, mint::AgentOptions{.max_total_tokens = 100});

    const auto result = agent.run("直接回答");
    EXPECT_TRUE(result.completed);
    EXPECT_EQ(result.status, "completed");
    EXPECT_EQ(result.answer, "边界内完成");
    EXPECT_EQ(result.model.total_tokens, 100U);
    EXPECT_TRUE(mint::agent_result_to_json(result)
                    .at("model")
                    .at("token_budget")
                    .at("exhausted")
                    .get<bool>());
}

TEST(AgentTokenBudgetTest, StopsPendingSideEffectAfterCumulativeTokenOverrun) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto event_path = temporary.path() / "budget-events.jsonl";
    write_text(workspace / "README.md", "# Before\n");

    mint::ToolRegistry tools(workspace, mint::ToolRegistryOptions{.allow_write = true});
    CumulativeBudgetModel model;
    mint::EventLog events(event_path);
    std::ostringstream output;
    mint::Agent agent(model, tools, output,
                      mint::AgentOptions{.max_turns = 4, .max_total_tokens = 100},
                      mint::AgentServices{.event_sink = &events});
    const auto result = agent.run("先读再改 README");

    EXPECT_EQ(result.status, "budget_exhausted");
    EXPECT_EQ(result.execution.tool_calls, 1U);
    EXPECT_EQ(result.execution.file_changes, 0U);
    EXPECT_EQ(result.model.calls, 2U);
    EXPECT_EQ(result.model.usage_reports, 2U);
    EXPECT_EQ(result.model.total_tokens, 110U);
    EXPECT_EQ(read_text(workspace / "README.md"), "# Before\n");

    std::ifstream input(event_path, std::ios::binary);
    std::string line;
    bool saw_exhausted = false;
    while (std::getline(input, line)) {
        const auto event = mint::Json::parse(line);
        if (event.at("type") != "token_budget_exhausted") {
            continue;
        }
        const auto& data = event.at("data");
        saw_exhausted = data.at("max_total_tokens") == 100 &&
                        data.at("reported_total_tokens") == 110 &&
                        data.at("pending_tool_call_count") == 1;
    }
    EXPECT_TRUE(saw_exhausted);
}

TEST(AgentTokenBudgetTest, ReportsUnavailableUsageWithoutPretendingToEnforceBudget) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Before\n");

    mint::ToolRegistry tools(workspace, mint::ToolRegistryOptions{.allow_write = true});
    MissingUsageModel model;
    std::ostringstream output;
    mint::Agent agent(model, tools, output,
                      mint::AgentOptions{.max_turns = 3, .max_total_tokens = 1});
    const auto result = agent.run("修改 README");

    EXPECT_TRUE(result.completed);
    EXPECT_EQ(result.execution.file_changes, 1U);
    EXPECT_EQ(result.model.calls, 2U);
    EXPECT_EQ(result.model.usage_reports, 0U);
    EXPECT_EQ(result.model.total_tokens, 0U);
    EXPECT_EQ(read_text(workspace / "README.md"), "# After\n");

    const auto budget = mint::agent_result_to_json(result).at("model").at("token_budget");
    EXPECT_EQ(budget.at("usage_coverage"), "unavailable");
    EXPECT_EQ(budget.at("enforcement"), "unavailable");
    EXPECT_FALSE(budget.at("exhausted").get<bool>());
    EXPECT_NE(output.str().find("provider 未返回 usage，无法按累计 Token 停止"), std::string::npos);
}

TEST(AgentTokenBudgetTest, AccountingSaturatesInsteadOfWrappingPastBudget) {
    mint::ModelSummary summary;
    summary.max_total_tokens = 100;
    summary.total_tokens = std::numeric_limits<std::size_t>::max() - 5;
    const mint::ModelReply reply{.usage = {.available = true,
                                           .prompt_tokens = 10,
                                           .completion_tokens = 10,
                                           .total_tokens = 10,
                                           .cached_tokens = 0}};

    mint::agent_detail::record_model_call(summary, reply);

    EXPECT_EQ(summary.total_tokens, std::numeric_limits<std::size_t>::max());
    EXPECT_TRUE(mint::agent_detail::token_budget_exhausted(summary));
}

TEST(AgentTokenBudgetTest, RestoredUsageCountsTowardTaskBudget) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto session_path = temporary.path() / "token-budget-session.json";
    write_text(workspace / "README.md", "# Before\n");
    mint::SessionStore session(session_path);

    mint::ToolRegistry first_tools(
        workspace,
        mint::ToolRegistryOptions{.protected_paths = {session_path}, .allow_write = true});
    ResumeBudgetFirstModel first_model;
    std::ostringstream first_output;
    mint::Agent first_agent(first_model, first_tools, first_output,
                            mint::AgentOptions{.max_turns = 1, .max_total_tokens = 100},
                            mint::AgentServices{.session_repository = &session});
    const auto first_result = first_agent.run("读取后尝试修改 README");

    EXPECT_EQ(first_result.status, "max_turns");
    EXPECT_EQ(first_result.model.total_tokens, 60U);
    const auto first_checkpoint = session.load();
    EXPECT_EQ(first_checkpoint.at("capabilities").at("max_total_tokens"), 100U);
    EXPECT_EQ(first_checkpoint.at("model").at("token_budget").at("reported_total_tokens"), 60U);

    mint::ToolRegistry resumed_tools(
        workspace,
        mint::ToolRegistryOptions{.protected_paths = {session_path}, .allow_write = true});
    ResumeBudgetSecondModel resumed_model;
    std::ostringstream resumed_output;
    mint::Agent resumed_agent(
        resumed_model, resumed_tools, resumed_output,
        mint::AgentOptions{.max_turns = 1, .max_total_tokens = 100, .resume_session = true},
        mint::AgentServices{.session_repository = &session});
    const auto resumed = resumed_agent.run("");

    EXPECT_EQ(resumed.status, "budget_exhausted");
    EXPECT_EQ(resumed.stop_reason, "max_total_tokens_exhausted");
    EXPECT_EQ(resumed.model.total_tokens, 110U);
    EXPECT_EQ(resumed.execution.tool_calls, 1U);
    EXPECT_EQ(read_text(workspace / "README.md"), "# Before\n");
    EXPECT_EQ(session.load().at("status"), "budget_exhausted");

    mint::ToolRegistry terminal_tools(
        workspace,
        mint::ToolRegistryOptions{.protected_paths = {session_path}, .allow_write = true});
    ResumeBudgetSecondModel terminal_model;
    std::ostringstream terminal_output;
    mint::Agent terminal_agent(
        terminal_model, terminal_tools, terminal_output,
        mint::AgentOptions{.max_turns = 1, .max_total_tokens = 100, .resume_session = true},
        mint::AgentServices{.session_repository = &session});
    EXPECT_THROW((void)terminal_agent.run(""), std::invalid_argument);
}

} // namespace
