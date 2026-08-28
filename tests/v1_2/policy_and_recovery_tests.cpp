#include "mint/application/agent.hpp"
#include "mint/infrastructure/session_store.hpp"
#include "mint/tools/tool_registry.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

#define MINT_EXPECT(condition, message) EXPECT_TRUE(condition) << (message)

std::filesystem::path test_executable;

void write_text(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not write " + path.generic_string());
    }
    output << content;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not read " + path.generic_string());
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ =
            std::filesystem::temp_directory_path() / ("mint-v1-2-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(path_ / "workspace" / "src");
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

int run_helper(int argc, char** argv) {
    if (argc < 3) {
        return 64;
    }
    const std::string mode = argv[2];
    if (mode == "echo") {
        std::cout << "inspection passed\n";
        return 0;
    }
    if (mode == "verify" && argc == 5) {
        return read_text(argv[3]) == argv[4] ? 0 : 9;
    }
    return 65;
}

mint::ModelReply tool_reply(std::string id, std::string name, mint::Json arguments) {
    const mint::Json raw_call = {{"id", id},
                                 {"type", "function"},
                                 {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
    mint::ModelReply reply;
    reply.assistant_message = {
        {"role", "assistant"}, {"content", nullptr}, {"tool_calls", mint::Json::array({raw_call})}};
    reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
    reply.metadata = {.adapter = "test", .model = "recipe-gate"};
    return reply;
}

mint::ModelReply final_reply(std::string text) {
    mint::ModelReply reply;
    reply.assistant_message = {{"role", "assistant"}, {"content", text}};
    reply.text = std::move(text);
    reply.metadata = {.adapter = "test", .model = "recipe-gate"};
    return reply;
}

class RecipeGateModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        bool saw_recipe = false;
        bool saw_raw_command = false;
        for (const auto& tool : tools) {
            const auto name = tool.at("function").value("name", "");
            saw_recipe = saw_recipe || name == "run_recipe";
            saw_raw_command = saw_raw_command || name == "run_command";
        }
        MINT_EXPECT(saw_recipe && !saw_raw_command,
                    "policy recipe mode hides raw command arguments from the model");

        switch (calls_++) {
        case 0:
            return tool_reply("gate-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Broken\n"},
                               {"new_text", "# Fixed\n"}});
        case 1:
            return tool_reply("gate-inspect", "run_recipe", {{"recipe", "inspect"}});
        case 2:
            return final_reply("premature");
        case 3:
            MINT_EXPECT(messages.back().at("role") == "user" &&
                            messages.back().at("content").get<std::string>().find(
                                "unverified changes") != std::string::npos,
                        "harness rejects a non-verification recipe as final evidence");
            gate_seen_ = true;
            return tool_reply("gate-verify", "run_recipe", {{"recipe", "verify"}});
        default:
            return final_reply("verified");
        }
    }

    [[nodiscard]] bool gate_seen() const noexcept {
        return gate_seen_;
    }

  private:
    int calls_ = 0;
    bool gate_seen_ = false;
};

class FinalizeAfterReadModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json& messages, const mint::Json&) override {
        MINT_EXPECT(messages.back().at("role") == "tool",
                    "read-only in-flight replay returns a tool result before the model resumes");
        const auto result = mint::Json::parse(messages.back().at("content").get<std::string>());
        MINT_EXPECT(result.at("ok").get<bool>() &&
                        result.at("content").get<std::string>() == "safe replay\n",
                    "read-only replay returns current workspace evidence");
        return final_reply("resumed safely");
    }
};

TEST(PolicyContractTest, VerificationRecipeGate) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Broken\n");
    const auto program = test_executable.generic_string();

    mint::ToolRegistry tools(
        workspace,
        mint::ToolRegistryOptions{
            .allow_write = true,
            .allowed_write_paths = {"README.md"},
            .command_recipes = {{.name = "inspect",
                                 .program = program,
                                 .args = {"--helper", "echo"},
                                 .timeout_seconds = 5,
                                 .verification = false},
                                {.name = "verify",
                                 .program = program,
                                 .args = {"--helper", "verify", "README.md", "# Fixed\n"},
                                 .timeout_seconds = 5,
                                 .verification = true}},
            .policy_fingerprint = "test-policy"});
    RecipeGateModel model;
    std::ostringstream output;
    mint::Agent agent(model, tools, output,
                      mint::AgentOptions{.max_turns = 6, .require_verification_after_write = true});
    const auto result = agent.run("repair and verify");
    MINT_EXPECT(result.completed && result.answer == "verified" && model.gate_seen(),
                "verification gate continues after a successful non-verification recipe");
    MINT_EXPECT(result.execution.recipe_calls == 2 && result.execution.verification_commands == 1 &&
                    result.verification_status == "passed",
                "only verification-marked recipes satisfy the post-write gate");
}

TEST(RecoveryContractTest, ReadOnlyInflightAutomaticallyReplays) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto session_path = temporary.path() / "session.json";
    write_text(workspace / "README.md", "safe replay\n");
    mint::SessionStore session(session_path);

    const mint::Json call = {
        {"id", "read-inflight"}, {"name", "read_file"}, {"arguments", {{"path", "README.md"}}}};
    const mint::Json raw_call = {
        {"id", "read-inflight"},
        {"type", "function"},
        {"function", {{"name", "read_file"}, {"arguments", R"({"path":"README.md"})"}}}};
    session.save(
        {{"schema_version", 3},
         {"status", "running"},
         {"workspace_root", std::filesystem::weakly_canonical(workspace).generic_string()},
         {"user_request", "read then finish"},
         {"messages", mint::Json::array({{{"role", "system"}, {"content", "test"}},
                                         {{"role", "user"}, {"content", "read"}},
                                         {{"role", "assistant"},
                                          {"content", nullptr},
                                          {"tool_calls", mint::Json::array({raw_call})}}})},
         {"turns", 1},
         {"duration_ms", 1},
         {"stop_reason", nullptr},
         {"answer", ""},
         {"verification_status", "not_required"},
         {"execution",
          {{"tool_calls", 0},
           {"successful_tool_calls", 0},
           {"tool_errors", 0},
           {"file_changes", 0},
           {"command_calls", 0},
           {"recipe_calls", 0},
           {"verification_commands", 0},
           {"commands_passed", 0},
           {"commands_failed", 0},
           {"commands_timed_out", 0},
           {"commands_cancelled", 0},
           {"commands_denied", 0},
           {"last_file_change_call", 0},
           {"last_command_call", 0},
           {"last_command_outcome", "not_run"},
           {"last_command_verification_eligible", false}}},
         {"model",
          {{"calls", 1},
           {"attempts", 1},
           {"retries", 0},
           {"usage_reports", 0},
           {"prompt_tokens", 0},
           {"completion_tokens", 0},
           {"total_tokens", 0},
           {"cached_tokens", 0},
           {"duration_ms", 1},
           {"adapter", "test"},
           {"model", "test"},
           {"last_response_id", nullptr}}},
         {"pending_tool_calls", mint::Json::array({call})},
         {"in_flight_tool_call", call},
         {"change_journal", {{"schema_version", 2}, {"entries", mint::Json::array()}}},
         {"capabilities",
          {{"allow_write", false},
           {"allowed_write_paths", mint::Json::array()},
           {"allowed_programs", mint::Json::array()},
           {"command_recipes", mint::Json::array()},
           {"policy_fingerprint", ""},
           {"approve_each_command", false},
           {"approve_each_changeset", false},
           {"require_verification", false},
           {"command_sandboxed", false},
           {"command_sandbox_backend", "none"},
           {"max_context_bytes", 24 * 1024}}}});

    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.protected_paths = {session_path}});
    FinalizeAfterReadModel model;
    std::ostringstream output;
    mint::Agent agent(
        model, tools, output,
        mint::AgentOptions{.max_turns = 1, .session_store = &session, .resume_session = true});
    const auto result = agent.run("");
    MINT_EXPECT(result.completed && result.answer == "resumed safely" &&
                    result.execution.tool_calls == 1,
                "read-only in-flight work is replayed automatically and exactly once in this run");
    MINT_EXPECT(session.load().at("in_flight_tool_call").is_null(),
                "completed replay clears the durable in-flight barrier");
}

} // namespace

#undef MINT_EXPECT

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--helper") {
        return run_helper(argc, argv);
    }
    std::error_code error;
    test_executable = std::filesystem::weakly_canonical(argv[0], error);
    if (error || test_executable.empty()) {
        std::cerr << "could not resolve test executable\n";
        return 1;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
