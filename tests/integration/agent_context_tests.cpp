#include "mint/application/agent.hpp"
#include "mint/tools/tool_registry.hpp"

#include "agent/agent_context.hpp"
#include "test_workspace.hpp"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace {

using mint::test::read_text;
using mint::test::TemporaryDirectory;
using mint::test::write_text;

class ContextBudgetModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json& messages, const mint::Json&) override {
        EXPECT_TRUE(messages.dump().size() <= 16 * 1024)
            << ("model context never exceeds configured byte budget");
        if (calls_ > 0) {
            bool found_summary = false;
            for (const auto& message : messages) {
                if (message.value("role", "") == "system" &&
                    message.value("content", "").find("Harness context summary") !=
                        std::string::npos) {
                    found_summary = true;
                }
            }
            EXPECT_TRUE(found_summary) << ("compacted context carries an explicit harness summary");
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
        EXPECT_TRUE(messages.dump().size() <= provider_context_bytes)
            << ("provider request budget clamps a larger Agent byte budget");
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
    EXPECT_TRUE(result.completed && result.answer == "上下文预算通过")
        << ("agent completes after multiple compacted large tool results");
    EXPECT_TRUE(result.execution.tool_calls == 3)
        << ("context compaction does not alter executed tool history");
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

    EXPECT_TRUE(result.completed && result.answer == "预算通过")
        << ("agent completes under the provider request budget");
    EXPECT_TRUE(result.execution.tool_calls == 3 && model.saw_compaction())
        << ("provider budget compacts history without changing executed tools");
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
    EXPECT_TRUE(compacted.payloads_compacted)
        << ("oversized latest tool group uses payload compaction");

    const auto tool_message =
        std::find_if(compacted.messages.begin(), compacted.messages.end(),
                     [](const mint::Json& message) { return message.value("role", "") == "tool"; });
    EXPECT_TRUE(tool_message != compacted.messages.end())
        << ("compaction retains the latest assistant/tool call pair");
    const auto retained = mint::Json::parse(tool_message->at("content").get<std::string>());
    EXPECT_TRUE(retained.at("context_compacted").get<bool>() && !retained.at("ok").get<bool>() &&
                retained.at("status") == "failed" && retained.at("error") == "compile failed" &&
                retained.at("resource_limited").get<bool>() &&
                retained.at("resource_limit") == "memory" &&
                retained.at("resource_limits").at("memory_bytes") == 134217728 &&
                !retained.contains("diagnostics"))
        << ("compaction omits bulk payload without changing failed evidence into success");
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

void append_context_tool(mint::Json& messages, const std::string& id, const std::string& name,
                         const mint::Json& arguments, const mint::Json& result) {
    messages.push_back({{"role", "assistant"},
                        {"content", nullptr},
                        {"tool_calls",
                         {{{"id", id},
                           {"type", "function"},
                           {"function", {{"name", name}, {"arguments", arguments.dump()}}}}}}});
    messages.push_back({{"role", "tool"}, {"tool_call_id", id}, {"content", result.dump()}});
}

TEST(AgentLoopTest, ContextCompactionRetainsDirectorySearchAndFileEvidence) {
    mint::Json messages = {{{"role", "system"}, {"content", "System policy."}},
                           {{"role", "user"}, {"content", "Evaluate this project."}}};
    mint::Json entries = mint::Json::array();
    mint::Json hits = mint::Json::array();
    entries.push_back({{"path", ".DS_Store"}, {"type", "file"}});
    entries.push_back({{"path", ".clang-format"}, {"type", "file"}});
    for (int index = 0; index < 200; ++index) {
        entries.push_back(
            {{"path", ".github/hidden/deep/" + std::to_string(index)}, {"type", "file"}});
        hits.push_back({{"path", "README.md"},
                        {"line", index + 1},
                        {"text", "C++ application architecture " + std::string(80, 'x')}});
    }
    entries.push_back({{"path", "README.md"}, {"type", "file"}});
    entries.push_back({{"path", "src"}, {"type", "directory"}});
    append_context_tool(messages, "files", "list_files", {{"path", "."}},
                        {{"ok", true}, {"entries", entries}});
    append_context_tool(messages, "search", "search_text", {{"query", "architecture"}},
                        {{"ok", true}, {"hits", hits}});
    const auto content =
        std::string("# Mint is a C++ agent\nUNTRUSTED_FILE_MARKER\n") + std::string(16000, 'x');
    append_context_tool(messages, "read", "read_file", {{"path", "README.md"}},
                        {{"ok", true},
                         {"path", "README.md"},
                         {"offset", std::size_t{64}},
                         {"next_offset", 64 + content.size()},
                         {"content", content}});
    const auto original = messages;
    const auto compacted = mint::agent_detail::compact_context(messages, 9014);

    EXPECT_EQ(messages, original);
    EXPECT_LE(compacted.sent_bytes, 9014U);
    std::size_t retained_tools = 0;
    for (const auto& entry : compacted.messages) {
        if (entry.value("role", "") == "system") {
            EXPECT_EQ(entry.dump().find("UNTRUSTED_FILE_MARKER"), std::string::npos);
        }
        if (entry.value("role", "") != "tool") {
            continue;
        }
        ++retained_tools;
        const auto result = mint::Json::parse(entry.at("content").get<std::string>());
        if (entry.at("tool_call_id") == "files") {
            ASSERT_FALSE(result.at("entries").empty());
            EXPECT_EQ(result.at("entries").front().at("path"), "README.md");
            EXPECT_GT(result.at("context_omitted_items").at("entries").get<std::size_t>(), 0U);
        } else if (entry.at("tool_call_id") == "search") {
            ASSERT_FALSE(result.at("hits").empty());
            EXPECT_EQ(result.at("hits").front().at("path"), "README.md");
            EXPECT_EQ(result.at("hits").front().at("line"), 1);
        } else {
            const auto excerpt = result.at("content").get<std::string>();
            EXPECT_TRUE(content.starts_with(excerpt));
            EXPECT_NE(excerpt.find("C++ agent"), std::string::npos);
            EXPECT_EQ(result.at("context_next_offset"), 64 + excerpt.size());
            EXPECT_EQ(result.at("next_offset"), 64 + content.size());
        }
    }
    EXPECT_EQ(retained_tools, 3U);
}

TEST(AgentLoopTest, RequestContextPreservesSentPrefixBetweenRebuilds) {
    mint::Json messages = {{{"role", "system"}, {"content", "policy"}},
                           {{"role", "user"}, {"content", "task"}}};
    mint::agent_detail::ContextWindow window;
    (void)window.prepare(messages, 9014);
    append_context_tool(
        messages, "first", "read_file", {{"path", "README.md"}},
        {{"ok", true}, {"content", "Original evidence " + std::string(16000, 'x')}});
    const auto first = window.prepare(messages, 9014);
    ASSERT_TRUE(first.payloads_compacted);
    EXPECT_FALSE(first.history_rebuilt);
    append_context_tool(messages, "second", "read_file", {{"path", "CMakeLists.txt"}},
                        {{"ok", true}, {"content", "cmake_minimum_required(VERSION 3.24)"}});
    const auto second = window.prepare(messages, 9014);

    ASSERT_GE(second.messages.size(), first.messages.size());
    for (std::size_t index = 0; index < first.messages.size(); ++index) {
        EXPECT_EQ(second.messages.at(index), first.messages.at(index));
    }
    EXPECT_FALSE(second.history_rebuilt);
    EXPECT_GE(second.reused_prefix_bytes, first.sent_bytes - 1);
    EXPECT_EQ(window.prepare(messages, 9014).messages, second.messages);
    EXPECT_EQ(mint::Json::parse(messages.at(3).at("content").get<std::string>())
                  .at("content")
                  .get<std::string>()
                  .size(),
              16018U);
}

TEST(AgentLoopTest, ContextRebuildRetainsLaterUserConstraintsAndExactToolArguments) {
    mint::Json messages = {{{"role", "system"}, {"content", "policy"}},
                           {{"role", "user"}, {"content", "task"}}};
    append_context_tool(messages, "before", "read_file", {{"path", "README.md"}},
                        {{"ok", true}, {"content", std::string(12000, 'x')}});
    const mint::Json constraint = {{"role", "user"}, {"content", "Do not modify any files."}};
    messages.push_back(constraint);
    append_context_tool(messages, "after", "read_file", {{"path", "CMakeLists.txt"}},
                        {{"ok", true}, {"content", std::string(12000, 'y')}});
    const auto compacted = mint::agent_detail::compact_context(messages, 2048);
    EXPECT_NE(std::find(compacted.messages.begin(), compacted.messages.end(), constraint),
              compacted.messages.end());
    EXPECT_EQ(compacted.messages.at(compacted.messages.size() - 2),
              messages.at(messages.size() - 2));
    EXPECT_LE(compacted.sent_bytes, 2048U);
}

TEST(AgentLoopTest, ContextWindowRebuildsFromFullCheckpointWithoutMutatingIt) {
    mint::Json messages = {{{"role", "system"}, {"content", "policy"}},
                           {{"role", "user"}, {"content", "task"}}};
    mint::agent_detail::ContextWindow window;
    std::size_t rebuilds = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        append_context_tool(messages, "read-" + std::to_string(index), "read_file",
                            {{"path", "file-" + std::to_string(index)}},
                            {{"ok", true}, {"content", "Evidence " + std::string(16000, 'x')}});
        const auto result = window.prepare(messages, 9014);
        rebuilds += result.history_rebuilt ? 1 : 0;
        EXPECT_LE(result.sent_bytes, 9014U);
        EXPECT_EQ(result.messages.back().at("tool_call_id"), "read-" + std::to_string(index));
    }
    EXPECT_GT(rebuilds, 0U);
    EXPECT_LT(rebuilds, 7U);
    const auto original = messages;
    mint::agent_detail::ContextWindow restored;
    const auto projection = restored.prepare(messages, 9014);
    EXPECT_EQ(messages, original);
    EXPECT_LE(projection.sent_bytes, 9014U);
    EXPECT_EQ(projection.messages.back().at("tool_call_id"), "read-7");
    EXPECT_EQ(messages.size(), 18U);
}

TEST(AgentLoopTest, ContextCompactionAcceptsObjectAndNullToolArgumentsWithoutRewritingThem) {
    for (const auto& arguments : {mint::Json{{"path", "README.md"}}, mint::Json(nullptr)}) {
        mint::Json messages = {{{"role", "system"}, {"content", "policy"}},
                               {{"role", "user"}, {"content", "task"}}};
        append_context_tool(messages, "read", "read_file", {{"path", "README.md"}},
                            {{"ok", true}, {"content", std::string(12000, 'x')}});
        messages.at(2).at("tool_calls").at(0).at("function")["arguments"] = arguments;
        const auto compacted = mint::agent_detail::compact_context(messages, 2048);
        EXPECT_EQ(compacted.messages.at(compacted.messages.size() - 2), messages.at(2));
        EXPECT_LE(compacted.sent_bytes, 2048U);
    }
}

TEST(AgentLoopTest, CompactedProjectEvidenceCompletesWithoutRepeatedDiscovery) {
    class EvidenceDrivenModel final : public mint::ModelClient {
      public:
        mint::ModelRequestLimits request_limits(const mint::Json&) const override {
            return {.max_request_tokens = 8000,
                    .reserved_output_tokens = 1024,
                    .safety_margin_tokens = 256,
                    .request_overhead_tokens = 2213};
        }

        mint::ModelReply complete(const mint::Json& messages, const mint::Json&) override {
            auto name = std::string("list_files");
            mint::Json arguments = {{"path", "."}, {"max_depth", 4}};
            if (messages.back().value("role", "") == "tool") {
                const auto result =
                    mint::Json::parse(messages.back().at("content").get<std::string>());
                if (result.value("content", "").find("PROJECT_EVIDENCE") != std::string::npos) {
                    saw_compacted_read = result.value("context_compacted", false);
                    return {.assistant_message = {{"role", "assistant"},
                                                  {"content", "Evidence retained"}},
                            .text = "Evidence retained"};
                }
                if (result.contains("entries") &&
                    std::any_of(
                        result.at("entries").begin(), result.at("entries").end(),
                        [](const mint::Json& entry) { return entry.at("path") == "README.md"; })) {
                    saw_compacted_listing = result.value("context_compacted", false);
                    name = "read_file";
                    arguments = {{"path", "README.md"}};
                }
            }
            const auto id = "evidence-" + std::to_string(++calls_);
            return {.assistant_message =
                        {{"role", "assistant"},
                         {"content", nullptr},
                         {"tool_calls",
                          {{{"id", id},
                            {"type", "function"},
                            {"function", {{"name", name}, {"arguments", arguments.dump()}}}}}}},
                    .tool_calls = {{id, name, arguments}}};
        }

        bool saw_compacted_listing = false;
        bool saw_compacted_read = false;

      private:
        std::size_t calls_ = 0;
    };
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md",
               "PROJECT_EVIDENCE: A C++ agent\n" + std::string(16000, 'x'));
    // EN: Exceed the context byte budget without hitting the directory tool's entry limit.
    // ZH-CN: 超过上下文字节预算，但不触发目录工具自身的条目上限。
    for (std::size_t index = 0; index < 150; ++index) {
        write_text(workspace / "source/deep/modules" /
                       ("file-" + std::to_string(index) + std::string(40, 'x') + ".cpp"),
                   "// source\n");
    }
    mint::ToolRegistry tools(workspace);
    const auto listing = mint::Json::parse(
        tools.execute({"fixture-list", "list_files", {{"path", "."}, {"max_depth", 4}}}));
    ASSERT_FALSE(listing.at("truncated").get<bool>()) << listing.dump();
    ASSERT_GT(listing.dump().size(), 9014U);
    ASSERT_TRUE(
        std::any_of(listing.at("entries").begin(), listing.at("entries").end(),
                    [](const mint::Json& entry) { return entry.at("path") == "README.md"; }))
        << listing.dump();
    EvidenceDrivenModel model;
    std::ostringstream output;
    mint::Agent agent(model, tools, output,
                      mint::AgentOptions{.max_turns = 4, .max_context_bytes = 16 * 1024});
    const auto result = agent.run("评价项目");
    EXPECT_TRUE(result.completed);
    EXPECT_EQ(result.turns, 3U);
    EXPECT_EQ(result.execution.tool_calls, 2U);
    EXPECT_TRUE(model.saw_compacted_listing);
    EXPECT_TRUE(model.saw_compacted_read);
}

} // namespace
