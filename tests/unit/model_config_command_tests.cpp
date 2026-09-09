#include "agent/agent_command_internal.hpp"
#include "command_line.hpp"
#include "config/model_config_command.hpp"
#include "provider/provider_command.hpp"

#include "mint/infrastructure/model_config_service.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

class ModelConfigCommandTest : public testing::Test {
  protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
               ("mint-config-cli-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(root);
        state = root / "state";
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    mint::Json execute(mint::cli::ConfigCommandAction action, std::string input = {},
                       std::filesystem::path source = {},
                       std::optional<std::string> expected_revision = std::nullopt) {
        std::istringstream in(std::move(input));
        std::ostringstream out;
        std::ostringstream error;
        mint::cli::Console console(in, out, error);
        mint::cli::CommandLine command;
        command.mode = mint::cli::CommandMode::config;
        command.config_action = action;
        command.stdin_json = action == mint::cli::ConfigCommandAction::set;
        command.json_output = true;
        command.state_dir = state;
        command.config_replace = expected_revision.has_value();
        command.config_expected_revision = expected_revision.value_or(std::string{});
        if (!source.empty()) {
            command.config = std::move(source);
            command.config_specified = true;
        }
        last_exit = mint::cli::run_model_config_command(command, console);
        last_output = out.str() + error.str();
        return mint::Json::parse(out.str());
    }

    static mint::Json update(std::string revision = {}) {
        return {{"expected_revision", std::move(revision)},
                {"adapter", "chat_completions"},
                {"base_url", "https://api.example.test/v1"},
                {"model", "offline-test-model"},
                {"api_key", "synthetic-config-test-secret"}};
    }

    std::filesystem::path root;
    std::filesystem::path state;
    int last_exit = -1;
    std::string last_output;
};

mint::cli::CommandLine parse(std::vector<std::string> args) {
    std::vector<char*> raw;
    for (auto& argument : args) {
        raw.push_back(argument.data());
    }
    return mint::cli::parse_arguments(static_cast<int>(raw.size()), raw.data());
}

TEST_F(ModelConfigCommandTest, ExposesStrictConfigurationCommandsAndSharedStateForConsumers) {
    const auto show = parse({"mint", "config", "show", "--state-dir", state.string(), "--json"});
    EXPECT_EQ(show.mode, mint::cli::CommandMode::config);
    EXPECT_EQ(show.config_action, mint::cli::ConfigCommandAction::show);
    EXPECT_EQ(show.state_dir, state);
    EXPECT_TRUE(show.config.empty());
    EXPECT_FALSE(show.config_specified);
    for (const auto& action : {"provider", "exec", "run", "resume"}) {
        const auto command = parse({"mint", action, "--state-dir", state.string()});
        EXPECT_TRUE(command.config.empty());
        EXPECT_FALSE(command.config_specified);
    }
    const auto set = parse({"mint", "config", "set", "--stdin-json", "--json"});
    EXPECT_TRUE(set.stdin_json);
    EXPECT_EQ(set.config_action, mint::cli::ConfigCommandAction::set);
    EXPECT_THROW((void)parse({"mint", "config", "set"}), std::invalid_argument);
    EXPECT_THROW((void)parse({"mint", "config", "set", "--stdin-json", "--config", "x"}),
                 std::invalid_argument);
    EXPECT_THROW((void)parse({"mint", "config", "import"}), std::invalid_argument);
    EXPECT_THROW((void)parse({"mint", "config", "show", "--stdin-json"}), std::invalid_argument);
    EXPECT_THROW((void)parse({"mint", "config", "show", "--config", ""}), std::invalid_argument);
    EXPECT_THROW((void)parse({"mint", "config", "import", "--config", ""}), std::invalid_argument);
    EXPECT_THROW((void)parse({"mint", "provider", "--config", ""}), std::invalid_argument);
    EXPECT_THROW((void)parse({"mint", "provider", "test", "--config", ""}), std::invalid_argument);
    EXPECT_THROW((void)parse({"mint", "exec", "--config", "", "inspect"}), std::invalid_argument);
    EXPECT_THROW((void)parse({"mint", "config", "--api-key", "fake-secret"}),
                 std::invalid_argument);
    EXPECT_EQ(parse({"mint", "provider", "--state-dir", state.string()}).state_dir, state);
    EXPECT_EQ(parse({"mint", "exec", "--state-dir", state.string(), "inspect"}).state_dir, state);
}

TEST_F(ModelConfigCommandTest, EmptyShowDoesNotRequireAProviderOrCredentials) {
    const auto shown = execute(mint::cli::ConfigCommandAction::show);
    EXPECT_EQ(last_exit, 0);
    EXPECT_FALSE(shown.at("configured"));
    EXPECT_EQ(shown.at("revision"), "");
    EXPECT_EQ(shown.at("config"), (state / "model.json").generic_string());
    EXPECT_FALSE(std::filesystem::exists(state / "model.json"));
}

TEST_F(ModelConfigCommandTest, ReplacementFlagsRequireAnImportAndANonemptyRevisionTogether) {
    const auto command = parse({"mint", "config", "import", "--config", "legacy.json", "--replace",
                                "--expected-revision", "revision-to-replace"});
    EXPECT_TRUE(command.config_replace);
    EXPECT_EQ(command.config_expected_revision, "revision-to-replace");
    const auto ordinary = parse({"mint", "config", "import", "--config", "legacy.json"});
    EXPECT_FALSE(ordinary.config_replace);
    EXPECT_TRUE(ordinary.config_expected_revision.empty());
    for (const auto& suffix :
         {std::vector<std::string>{"--replace"},
          std::vector<std::string>{"--expected-revision", "secret-not-to-echo"},
          std::vector<std::string>{"--replace", "--expected-revision"},
          std::vector<std::string>{"--replace", "--expected-revision", ""},
          std::vector<std::string>{"--replace", "--expected-revision", "--json"},
          std::vector<std::string>{"--replace", "--replace", "--expected-revision", "version"},
          std::vector<std::string>{"--replace", "--expected-revision", "version",
                                   "--expected-revision", "another-version"}}) {
        std::vector<std::string> arguments{"mint", "config", "import", "--config", "legacy.json"};
        arguments.insert(arguments.end(), suffix.begin(), suffix.end());
        try {
            (void)parse(arguments);
            FAIL() << "Replacement flags must be paired and unambiguous";
        } catch (const std::invalid_argument& error) {
            EXPECT_EQ(std::string(error.what()).find("secret-not-to-echo"), std::string::npos);
        }
    }
    EXPECT_THROW((void)parse({"mint", "config", "show", "--replace", "--expected-revision", "v"}),
                 std::invalid_argument);
    EXPECT_THROW((void)parse({"mint", "config", "set", "--stdin-json", "--replace",
                              "--expected-revision", "v"}),
                 std::invalid_argument);
}

TEST_F(ModelConfigCommandTest, ReplacementImportUsesRevisionAndNeverPrintsCredentials) {
    const auto initial = execute(mint::cli::ConfigCommandAction::set, update().dump());
    ASSERT_EQ(last_exit, 0);
    const auto source = root / "legacy-replacement.json";
    const std::string original =
        R"({"provider":"custom","adapter":"responses","endpoint":"https://legacy.example.test/v1/responses","model":"replacement-model","api_key":"synthetic-replacement-secret","max_retries":2})";
    {
        std::ofstream output(source);
        output << original;
    }
    const auto replaced = execute(mint::cli::ConfigCommandAction::import, {}, source,
                                  initial.at("revision").get<std::string>());
    ASSERT_EQ(last_exit, 0);
    EXPECT_EQ(replaced.at("operation"), "import");
    EXPECT_EQ(replaced.at("model"), "replacement-model");
    EXPECT_TRUE(replaced.at("saved_api_key"));
    EXPECT_FALSE(replaced.at("override"));
    EXPECT_EQ(last_output.find("synthetic-replacement-secret"), std::string::npos);
    EXPECT_EQ(last_output.find("synthetic-config-test-secret"), std::string::npos);
    mint::ModelConfigService service(state);
    EXPECT_EQ(service.load_runtime().api_key, "synthetic-replacement-secret");
    EXPECT_EQ(service.load_runtime().max_retries, 2);
    const auto conflict = execute(mint::cli::ConfigCommandAction::import, {}, source,
                                  initial.at("revision").get<std::string>());
    EXPECT_EQ(last_exit, 2);
    EXPECT_EQ(conflict.at("code"), "conflict");
    EXPECT_EQ(last_output.find("synthetic-replacement-secret"), std::string::npos);
    EXPECT_EQ(service.inspect().revision, replaced.at("revision").get<std::string>());
    const auto missing_revision =
        execute(mint::cli::ConfigCommandAction::import, {}, source, std::string{});
    EXPECT_EQ(last_exit, 1);
    EXPECT_EQ(missing_revision.at("code"), "invalid_input");
    EXPECT_EQ(service.inspect().revision, replaced.at("revision").get<std::string>());
    std::ifstream input(source);
    EXPECT_EQ(
        (std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()}),
        original);
}

TEST_F(ModelConfigCommandTest, ExecutionArtifactsCannotOverwriteSharedModelFiles) {
    mint::ModelConfigService service(state);
    mint::cli::CommandLine command;
    command.state_dir = state;
    command.root = root / "workspace";
    for (const auto& path :
         {service.config_path(), service.credentials_path(), state / "model.lock",
          state / ".model-staging", state / ".model-staging" / "session.json"}) {
        command.events_jsonl = path;
        EXPECT_THROW(mint::cli::command_detail::validate_runtime_paths(command),
                     std::invalid_argument);
        command.demo = true;
        EXPECT_THROW(mint::cli::command_detail::validate_runtime_paths(command),
                     std::invalid_argument);
        command.demo = false;
    }
}

TEST_F(ModelConfigCommandTest, SetShowAndProviderUseTheSameRedactedConfiguration) {
    const auto saved = execute(mint::cli::ConfigCommandAction::set, update().dump());
    ASSERT_EQ(last_exit, 0) << last_output;
    EXPECT_TRUE(saved.at("saved_api_key"));
    EXPECT_EQ(last_output.find("synthetic-config-test-secret"), std::string::npos);
    EXPECT_EQ(execute(mint::cli::ConfigCommandAction::show).at("revision"), saved.at("revision"));

    mint::ModelConfigService service(state);
    EXPECT_EQ(service.load_runtime().api_key, "synthetic-config-test-secret");
    EXPECT_EQ(service.load_runtime().model, "offline-test-model");

    mint::cli::CommandLine command;
    command.mode = mint::cli::CommandMode::provider;
    command.state_dir = state;
    command.json_output = true;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    mint::cli::Console console(input, output, error);
    EXPECT_EQ(mint::cli::run_provider_command(command, console), 0);
    const auto provider = mint::Json::parse(output.str());
    EXPECT_EQ(provider.at("model"), saved.at("model"));
    EXPECT_EQ(provider.at("config"), saved.at("config"));
    EXPECT_EQ(provider.at("authentication"), "local_store");
    EXPECT_EQ(output.str().find("synthetic-config-test-secret"), std::string::npos);
}

TEST_F(ModelConfigCommandTest, RejectsStaleRevisionWithoutOverwritingEitherFieldOrKey) {
    const auto first = execute(mint::cli::ConfigCommandAction::set, update().dump());
    ASSERT_EQ(last_exit, 0);
    auto second = update(first.at("revision").get<std::string>());
    second["model"] = "second-model";
    const auto saved = execute(mint::cli::ConfigCommandAction::set, second.dump());
    ASSERT_EQ(last_exit, 0);
    second["model"] = "stale-model";
    second["api_key"] = "stale-fake-key";
    const auto conflict = execute(mint::cli::ConfigCommandAction::set, second.dump());
    EXPECT_EQ(last_exit, 2);
    EXPECT_EQ(conflict.at("code"), "conflict");
    EXPECT_EQ(last_output.find("stale-fake-key"), std::string::npos);
    EXPECT_EQ(execute(mint::cli::ConfigCommandAction::show).at("revision"), saved.at("revision"));
    EXPECT_EQ(mint::ModelConfigService(state).load_runtime().model, "second-model");
}

TEST_F(ModelConfigCommandTest, RemoveKeyUsesTheSameRevisionAndDoesNotRestoreAHiddenFallback) {
    const auto saved = execute(mint::cli::ConfigCommandAction::set, update().dump());
    auto remove = update(saved.at("revision").get<std::string>());
    remove.erase("api_key");
    remove["remove_api_key"] = true;
    const auto removed = execute(mint::cli::ConfigCommandAction::set, remove.dump());
    EXPECT_EQ(last_exit, 0);
    EXPECT_FALSE(removed.at("saved_api_key"));
    EXPECT_TRUE(mint::ModelConfigService(state).load_runtime().api_key.empty());
}

TEST_F(ModelConfigCommandTest, ExplicitInspectionNeverChangesSharedDefaultsOrReusesTheirKey) {
    const auto saved = execute(mint::cli::ConfigCommandAction::set, update().dump());
    const auto source = root / "override.json";
    {
        std::ofstream out(source);
        out << R"({"provider":"custom","endpoint":"https://other.example.test/v1/responses","adapter":"responses","model":"another"})";
    }
    const auto shown = execute(mint::cli::ConfigCommandAction::show, {}, source);
    EXPECT_EQ(last_exit, 0);
    EXPECT_TRUE(shown.at("override"));
    EXPECT_EQ(shown.at("model"), "another");
    EXPECT_FALSE(shown.at("credential_available"));
    EXPECT_EQ(execute(mint::cli::ConfigCommandAction::show).at("revision"), saved.at("revision"));
    EXPECT_TRUE(mint::ModelConfigService(state).load_runtime(source).api_key.empty());
}

TEST_F(ModelConfigCommandTest, ImportIsOneTimeAndPreservesTheOriginalFile) {
    const auto source = root / "legacy.json";
    const std::string original =
        R"({"provider":"custom","adapter":"responses","endpoint":"https://api.example.test/v1/responses","model":"legacy","api_key":"synthetic-legacy-secret","max_retries":4})";
    {
        std::ofstream out(source);
        out << original;
    }
    const auto imported = execute(mint::cli::ConfigCommandAction::import, {}, source);
    ASSERT_EQ(last_exit, 0);
    EXPECT_TRUE(imported.at("saved_api_key"));
    EXPECT_EQ(last_output.find("synthetic-legacy-secret"), std::string::npos);
    std::ifstream input(source);
    const std::string after{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    EXPECT_EQ(after, original);
    EXPECT_EQ(mint::ModelConfigService(state).load_runtime().max_retries, 4);
    const auto conflict =
        execute(mint::cli::ConfigCommandAction::import, {}, root / "missing.json");
    EXPECT_EQ(last_exit, 2);
    EXPECT_EQ(conflict.at("code"), "conflict");
    EXPECT_EQ(mint::ModelConfigService(state).inspect().revision,
              imported.at("revision").get<std::string>());
    EXPECT_EQ(mint::ModelConfigService(state).load_runtime().api_key, "synthetic-legacy-secret");
}

TEST_F(ModelConfigCommandTest, InvalidOrOversizedInputNeverEchoesSubmittedValues) {
    for (const auto& input : {std::string(R"({"api_key":"never-print-this-secret",broken)"),
                              std::string(65 * 1024, 'x'),
                              std::string(R"({"unexpected":"never-print-this-secret"})")}) {
        const auto result = execute(mint::cli::ConfigCommandAction::set, input);
        EXPECT_EQ(last_exit, 1);
        EXPECT_EQ(result.at("code"), "invalid_input");
        EXPECT_EQ(last_output.find("never-print-this-secret"), std::string::npos);
    }
    auto bad = update();
    bad["base_url"] = "https://user:never-print-this-secret@example.test";
    EXPECT_EQ(execute(mint::cli::ConfigCommandAction::set, bad.dump()).at("code"), "invalid_input");
    EXPECT_EQ(last_output.find("never-print-this-secret"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(state / "model.json"));
}

} // namespace
