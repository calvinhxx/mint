#include "provider_command.hpp"

#include "mint/infrastructure/config.hpp"
#include "mint/infrastructure/model_provider_client.hpp"
#include "model_protocol.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("mint-provider-profile-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

void write_json(const std::filesystem::path& path, const mint::Json& value) {
    std::ofstream output(path);
    ASSERT_TRUE(output.is_open());
    output << value.dump(2) << '\n';
    ASSERT_TRUE(output.good());
}

mint::ModelProviderConfig
config_for(std::string api_url, mint::ModelAdapter adapter = mint::ModelAdapter::chat_completions) {
    mint::ModelProviderConfig config;
    config.api_url = std::move(api_url);
    config.model = "test-model";
    config.adapter = adapter;
    return config;
}

mint::Json messages() {
    return mint::Json::array({{{"role", "user"}, {"content", "inspect"}}});
}

mint::Json tools() {
    return mint::Json::array(
        {{{"type", "function"},
          {"function",
           {{"name", "read_file"},
            {"description", "Read a file"},
            {"parameters", {{"type", "object"}, {"properties", mint::Json::object()}}}}}}});
}

std::filesystem::path fixed_config(std::string_view name) {
    return std::filesystem::path(MINT_PROVIDER_CONFIG_DIR) / name;
}

mint::cli::CommandLine parse_command(std::initializer_list<std::string> arguments) {
    std::vector<std::string> storage(arguments);
    std::vector<char*> values;
    values.reserve(storage.size());
    for (auto& argument : storage) {
        values.push_back(argument.data());
    }
    return mint::cli::parse_arguments(static_cast<int>(values.size()), values.data());
}

void expect_config_rejected(const mint::Json& value, std::string_view expected_message) {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "invalid.json";
    write_json(path, value);

    try {
        (void)mint::load_model_provider_config(path);
        FAIL() << "expected invalid provider config";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find(expected_message), std::string::npos)
            << error.what();
    }
}

TEST(ProviderProfileContractTest, DetectsOfficialEndpointsAndExplicitProxyProfiles) {
    auto openai = config_for("https://api.openai.com/v1/responses", mint::ModelAdapter::responses);
    auto profile = mint::resolve_model_provider_profile(openai);
    EXPECT_EQ(profile.provider, mint::ModelProvider::openai);
    EXPECT_EQ(profile.source, mint::ModelProviderSource::endpoint);
    EXPECT_EQ(profile.capabilities.token_limit_parameter,
              mint::ModelTokenLimitParameter::max_output_tokens);
    EXPECT_TRUE(profile.capabilities.stateless_reasoning_replay);

    profile = mint::resolve_model_provider_profile(
        config_for("https://api.groq.com/openai/v1/chat/completions"));
    EXPECT_EQ(profile.provider, mint::ModelProvider::groq);
    EXPECT_EQ(profile.capabilities.token_limit_parameter,
              mint::ModelTokenLimitParameter::max_completion_tokens);
    EXPECT_TRUE(profile.capabilities.stream_usage);

    profile = mint::resolve_model_provider_profile(
        config_for("https://api.deepseek.com/chat/completions"));
    EXPECT_EQ(profile.provider, mint::ModelProvider::deepseek);
    EXPECT_EQ(profile.capabilities.token_limit_parameter,
              mint::ModelTokenLimitParameter::max_tokens);
    EXPECT_FALSE(profile.capabilities.explicit_tool_choice);
    EXPECT_TRUE(profile.capabilities.chat_reasoning_replay);
    EXPECT_TRUE(profile.capabilities.requires_tool_call_content);

    auto proxy = config_for("https://gateway.example.test/v1/chat/completions");
    proxy.provider = mint::ModelProvider::groq;
    profile = mint::resolve_model_provider_profile(proxy);
    EXPECT_EQ(profile.provider, mint::ModelProvider::groq);
    EXPECT_EQ(profile.source, mint::ModelProviderSource::explicit_config);

    profile = mint::resolve_model_provider_profile(
        config_for("http://127.0.0.1:8080/v1/chat/completions"));
    EXPECT_EQ(profile.provider, mint::ModelProvider::custom);
    EXPECT_EQ(profile.source, mint::ModelProviderSource::compatibility_default);

    EXPECT_THROW((void)mint::resolve_model_provider_profile(
                     config_for("https://api.openai.com@proxy.example.test/v1/responses")),
                 std::invalid_argument);
}

TEST(ProviderProfileContractTest, BuildsRequestsFromResolvedCapabilities) {
    auto deepseek = config_for("https://api.deepseek.com/chat/completions");
    deepseek.stream = true;
    deepseek.max_completion_tokens = 321;
    auto continuation = messages();
    continuation.push_back(
        {{"role", "assistant"},
         {"content", nullptr},
         {"reasoning_content", "opaque reasoning"},
         {"tool_calls", mint::Json::array({{{"id", "call-1"},
                                            {"type", "function"},
                                            {"function",
                                             {{"name", "read_file"},
                                              {"arguments", R"({"path":"README.md"})"}}}}})}});
    continuation.push_back(
        {{"role", "tool"}, {"tool_call_id", "call-1"}, {"content", R"({"ok":true})"}});
    const auto deepseek_request =
        mint::detail::build_provider_request(deepseek, continuation, tools());
    EXPECT_EQ(deepseek_request.at("max_tokens"), 321);
    EXPECT_FALSE(deepseek_request.contains("max_completion_tokens"));
    EXPECT_TRUE(deepseek_request.at("stream_options").at("include_usage"));
    EXPECT_FALSE(deepseek_request.contains("tool_choice"));
    EXPECT_EQ(deepseek_request.at("messages").at(1).at("reasoning_content"), "opaque reasoning");
    EXPECT_EQ(deepseek_request.at("messages").at(1).at("content"), "");

    auto custom = config_for("http://127.0.0.1:8080/v1/chat/completions");
    custom.provider = mint::ModelProvider::custom;
    custom.stream = true;
    custom.capabilities = mint::ModelProviderCapabilities{
        .function_tools = true,
        .streaming = true,
        .stream_usage = false,
        .stateless_reasoning_replay = false,
        .token_limit_parameter = mint::ModelTokenLimitParameter::max_tokens};
    const auto custom_request = mint::detail::build_provider_request(custom, continuation, tools());
    EXPECT_TRUE(custom_request.contains("max_tokens"));
    EXPECT_FALSE(custom_request.contains("stream_options"));
    EXPECT_EQ(custom_request.at("tool_choice"), "auto");
    EXPECT_FALSE(custom_request.at("messages").at(1).contains("reasoning_content"));
    EXPECT_TRUE(custom_request.at("messages").at(1).at("content").is_null());

    custom.capabilities->function_tools = false;
    EXPECT_THROW((void)mint::detail::build_provider_request(custom, messages(), tools()),
                 std::invalid_argument);

    auto responses =
        config_for("https://gateway.example.test/v1/responses", mint::ModelAdapter::responses);
    responses.provider = mint::ModelProvider::custom;
    responses.capabilities = mint::ModelProviderCapabilities{
        .function_tools = true,
        .streaming = true,
        .stream_usage = false,
        .stateless_reasoning_replay = false,
        .token_limit_parameter = mint::ModelTokenLimitParameter::max_output_tokens};
    const auto responses_request =
        mint::detail::build_provider_request(responses, messages(), tools());
    EXPECT_EQ(responses_request.at("store"), false);
    EXPECT_FALSE(responses_request.contains("include"));
}

TEST(ProviderConfigContractTest, LoadsCommittedRegressionProfilesWithoutSecrets) {
    struct ExpectedProfile {
        const char* file;
        mint::ModelProvider provider;
        mint::ModelAdapter adapter;
        const char* api_key_env;
    };
    const ExpectedProfile expected[] = {
        {"openai-responses.json", mint::ModelProvider::openai, mint::ModelAdapter::responses,
         "OPENAI_API_KEY"},
        {"groq-chat.json", mint::ModelProvider::groq, mint::ModelAdapter::chat_completions,
         "GROQ_API_KEY"},
        {"deepseek-chat.json", mint::ModelProvider::deepseek, mint::ModelAdapter::chat_completions,
         "DEEPSEEK_API_KEY"},
        {"custom-chat.json", mint::ModelProvider::custom, mint::ModelAdapter::chat_completions,
         "MINT_MODEL_API_KEY"},
    };

    for (const auto& item : expected) {
        SCOPED_TRACE(item.file);
        const auto config = mint::load_model_provider_config(fixed_config(item.file));
        const auto profile = mint::resolve_model_provider_profile(config);
        EXPECT_EQ(profile.provider, item.provider);
        EXPECT_EQ(profile.adapter, item.adapter);
        EXPECT_EQ(profile.source, mint::ModelProviderSource::explicit_config);
        EXPECT_EQ(config.api_key_env, item.api_key_env);
        EXPECT_TRUE(config.api_key.empty());
        EXPECT_FALSE(config.model.empty());
    }
}

TEST(ProviderConfigContractTest, RejectsContradictoryProfilesAndCredentials) {
    expect_config_rejected({{"provider", "groq"},
                            {"adapter", "responses"},
                            {"api_url", "https://api.groq.com/openai/v1/responses"},
                            {"model", "test-model"}},
                           "chat_completions");
    expect_config_rejected({{"api_url", "https://example.test/v1/chat/completions"},
                            {"api_key", "secret"},
                            {"api_key_env", "MODEL_API_KEY"},
                            {"model", "test-model"}},
                           "不能同时设置");
    expect_config_rejected({{"provider", "openai"},
                            {"api_url", "https://api.openai.com/v1/chat/completions"},
                            {"model", "test-model"},
                            {"capabilities", {{"function_tools", false}}}},
                           "只有 custom provider");
    expect_config_rejected({{"provider", "custom"},
                            {"api_url", "https://example.test/v1/chat/completions"},
                            {"model", "test-model"},
                            {"capabilities", {{"unknown", true}}}},
                           "未知 capability");
    expect_config_rejected({{"provider", "custom"},
                            {"adapter", "responses"},
                            {"api_url", "https://example.test/v1/responses"},
                            {"model", "test-model"},
                            {"capabilities", {{"chat_reasoning_replay", true}}}},
                           "Chat 专属能力");
}

TEST(ProviderCliContractTest, ReportsCapabilitiesWithoutReadingOrPrintingApiKeys) {
    mint::cli::CommandLine command_line;
    command_line.mode = mint::cli::CommandMode::provider;
    command_line.config = fixed_config("groq-chat.json");
    command_line.json_output = true;

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    mint::cli::Console console(input, output, error);
    EXPECT_EQ(mint::cli::run_provider_command(command_line, console), 0);

    const auto report = mint::Json::parse(output.str());
    EXPECT_EQ(report.at("provider"), "groq");
    EXPECT_EQ(report.at("source"), "config");
    EXPECT_EQ(report.at("adapter"), "chat_completions");
    EXPECT_EQ(report.at("authentication"), "environment");
    EXPECT_EQ(report.at("api_key_env"), "GROQ_API_KEY");
    EXPECT_EQ(report.at("capabilities").at("token_limit_parameter"), "max_completion_tokens");
    EXPECT_TRUE(report.at("capabilities").at("explicit_tool_choice"));
    EXPECT_FALSE(report.at("capabilities").at("chat_reasoning_replay"));
    EXPECT_FALSE(report.at("capabilities").at("requires_tool_call_content"));
    EXPECT_FALSE(report.contains("api_key"));

    TemporaryDirectory temporary;
    const auto inline_key_config = temporary.path() / "inline.json";
    write_json(inline_key_config,
               {{"provider", "custom"},
                {"api_url", "https://example.test/v1/chat/completions?api_key=url-secret"},
                {"api_key", "do-not-print-this"},
                {"model", "test-model"}});
    command_line.config = inline_key_config;
    output.str({});
    output.clear();
    EXPECT_EQ(mint::cli::run_provider_command(command_line, console), 0);
    EXPECT_EQ(output.str().find("do-not-print-this"), std::string::npos);
    EXPECT_EQ(output.str().find("url-secret"), std::string::npos);
}

TEST(ProviderCliContractTest, DistinguishesOfflineInspectionFromExplicitLiveTest) {
    const auto inspect = parse_command({"mint", "provider", "--config", "provider.json", "--json"});
    EXPECT_EQ(inspect.mode, mint::cli::CommandMode::provider);
    EXPECT_EQ(inspect.provider_action, mint::cli::ProviderCommandAction::inspect);
    EXPECT_EQ(inspect.config, "provider.json");
    EXPECT_TRUE(inspect.json_output);

    const auto live =
        parse_command({"mint", "provider", "test", "--config", "provider.json", "--json"});
    EXPECT_EQ(live.mode, mint::cli::CommandMode::provider);
    EXPECT_EQ(live.provider_action, mint::cli::ProviderCommandAction::test);
    EXPECT_EQ(live.config, "provider.json");
    EXPECT_TRUE(live.json_output);

    EXPECT_THROW((void)parse_command({"mint", "provider", "--allow-write"}), std::invalid_argument);
    EXPECT_THROW((void)parse_command({"mint", "provider", "--root", "."}), std::invalid_argument);
    EXPECT_THROW((void)parse_command({"mint", "provider", "run a task"}), std::invalid_argument);
    EXPECT_THROW((void)parse_command({"mint", "provider", "probe"}), std::invalid_argument);
}

} // namespace
