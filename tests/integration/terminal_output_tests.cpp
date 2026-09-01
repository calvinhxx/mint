#include "mint/application/agent.hpp"
#include "mint/tools/tool_registry.hpp"

#include "agent_command.hpp"
#include "console.hpp"
#include "scripted_http_server.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("mint-terminal-output-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
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

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output) << "could not create " << path;
    output << text;
}

class TerminalPayloadModel final : public mint::ModelClient {
  public:
    explicit TerminalPayloadModel(std::string answer) : answer_(std::move(answer)) {}

    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        return {.assistant_message = {{"role", "assistant"}, {"content", answer_}},
                .text = answer_};
    }

  private:
    std::string answer_;
};

TEST(TerminalOutputTest, FinalAnswerIsEscapedButMachineResultRetainsOriginalText) {
    const auto answer = std::string("普通中文 ") + '\x1B' + "]52;c;clipboard" + '\x07' + '\x1B' +
                        "[2J " + "\xE2\x80\xAE" + "txt";
    TerminalPayloadModel model(answer);
    mint::ToolRegistry tools(std::filesystem::temp_directory_path());
    std::ostringstream output;
    mint::Agent agent(model, tools, output);

    const auto result = agent.run("return one answer");

    EXPECT_TRUE(result.completed);
    EXPECT_EQ(result.answer, answer);
    EXPECT_EQ(mint::agent_result_to_json(result).at("answer"), answer);
    EXPECT_EQ(output.str().find('\x1B'), std::string::npos);
    EXPECT_EQ(output.str().find("\xE2\x80\xAE"), std::string::npos);
    EXPECT_NE(output.str().find("普通中文 \\u001B]52;c;clipboard\\u0007\\u001B[2J \\u202Etxt"),
              std::string::npos);
}

TEST(TerminalOutputTest, StreamingCliEscapesEachUntrustedDelta) {
    const auto answer = std::string("普通中文 ") + '\x1B' + "]52;c;clipboard" + '\x07' + '\x1B' +
                        "[2J " + "\xE2\x80\xAE" + "txt";
    const mint::Json final_response = {
        {"id", "response-terminal-output"},
        {"status", "completed"},
        {"model", "terminal-output-test"},
        {"output",
         mint::Json::array(
             {{{"id", "message-terminal-output"},
               {"type", "message"},
               {"role", "assistant"},
               {"status", "completed"},
               {"content", mint::Json::array({{{"type", "output_text"}, {"text", answer}}})}}})},
        {"usage", {{"input_tokens", 1}, {"output_tokens", 1}, {"total_tokens", 2}}}};
    const auto event = [](const mint::Json& value) { return "data: " + value.dump() + "\n\n"; };
    auto stream = event({{"type", "response.output_text.delta"},
                         {"item_id", "message-terminal-output"},
                         {"output_index", 0},
                         {"content_index", 0},
                         {"delta", answer}});
    stream += event({{"type", "response.completed"}, {"response", final_response}});
    mint::test::ScriptedHttpServer server(
        {{{.status = 200, .content_type = "text/event-stream", .body = std::move(stream)}}});

    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    std::filesystem::create_directories(workspace);
    const auto config = temporary.path() / "provider.json";
    write_text(config, mint::Json({{"adapter", "responses"},
                                   {"api_url", server.url("/v1/responses")},
                                   {"api_key", ""},
                                   {"model", "terminal-output-test"},
                                   {"connect_timeout_seconds", 2},
                                   {"request_timeout_seconds", 2},
                                   {"max_retries", 0},
                                   {"max_completion_tokens", 64},
                                   {"stream", true}})
                           .dump());

    mint::cli::CommandLine command_line;
    command_line.config = config;
    command_line.config_specified = true;
    command_line.root = workspace;
    command_line.root_specified = true;
    command_line.question = "return one answer";
    std::optional<mint::ManagedTaskPaths> managed_task;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    mint::cli::Console console(input, output, error);

    EXPECT_EQ(mint::cli::run_agent_command(std::move(command_line), managed_task, console), 0);
    server.wait();

    const auto expected = "普通中文 \\u001B]52;c;clipboard\\u0007\\u001B[2J \\u202Etxt";
    EXPECT_EQ(output.str().find('\x1B'), std::string::npos);
    EXPECT_EQ(error.str().find('\x1B'), std::string::npos);
    EXPECT_EQ(output.str().find("\xE2\x80\xAE"), std::string::npos);
    EXPECT_EQ(error.str().find("\xE2\x80\xAE"), std::string::npos);
    EXPECT_NE(output.str().find(expected), std::string::npos);
    EXPECT_NE(error.str().find(expected), std::string::npos);
}

} // namespace
