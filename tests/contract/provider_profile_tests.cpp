#include "provider_command.hpp"

#include "mint/infrastructure/config.hpp"
#include "mint/infrastructure/model_provider_client.hpp"
#include "model/model_protocol.hpp"

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

void expect_only_token_limit(const mint::Json& request, std::string_view expected_field,
                             long expected_value) {
    constexpr std::string_view fields[] = {"max_completion_tokens", "max_tokens",
                                           "max_output_tokens"};
    for (const auto field : fields) {
        SCOPED_TRACE(field);
        const auto name = std::string(field);
        if (field == expected_field) {
            ASSERT_TRUE(request.contains(name));
            EXPECT_EQ(request.at(name), expected_value);
        } else {
            EXPECT_FALSE(request.contains(name));
        }
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

TEST(ProviderConfigContractTest, ResolvesOfficialDefaultsAndLegacyBaseUrls) {
    TemporaryDirectory temporary;

    const auto deepseek_path = temporary.path() / "deepseek.json";
    write_json(deepseek_path, {{"provider", "deepseek"}, {"model", "deepseek-v4-flash"}});
    EXPECT_EQ(mint::load_model_provider_config(deepseek_path).api_url,
              "https://api.deepseek.com/chat/completions");

    const auto openai_path = temporary.path() / "openai.json";
    write_json(openai_path,
               {{"provider", "openai"}, {"adapter", "responses"}, {"model", "test-model"}});
    EXPECT_EQ(mint::load_model_provider_config(openai_path).api_url,
              "https://api.openai.com/v1/responses");

    const auto groq_path = temporary.path() / "groq.json";
    write_json(groq_path, {{"api_url", "https://api.groq.com/"}, {"model", "test-model"}});
    const auto groq = mint::load_model_provider_config(groq_path);
    EXPECT_EQ(groq.api_url, "https://api.groq.com/openai/v1/chat/completions");
    EXPECT_EQ(mint::resolve_model_provider_profile(groq).provider, mint::ModelProvider::groq);
}

TEST(ProviderConfigContractTest, KeepsExplicitEndpointsAndRejectsAmbiguousFields) {
    TemporaryDirectory temporary;
    const auto custom_path = temporary.path() / "custom.json";
    write_json(custom_path, {{"provider", "custom"},
                             {"endpoint", "http://127.0.0.1:8080/v1/chat/completions"},
                             {"model", "test-model"}});
    EXPECT_EQ(mint::load_model_provider_config(custom_path).api_url,
              "http://127.0.0.1:8080/v1/chat/completions");

    expect_config_rejected({{"model", "test-model"}}, "必须配置完整 endpoint");
    expect_config_rejected({{"provider", "custom"}, {"model", "test-model"}},
                           "必须配置完整 endpoint");
    expect_config_rejected({{"provider", "custom"},
                            {"endpoint", "https://example.test/v1/chat/completions"},
                            {"api_url", "https://example.test/v1/chat/completions"},
                            {"model", "test-model"}},
                           "不能同时设置");
    expect_config_rejected(
        {{"api_url", "https://api.deepseek.com?token=secret"}, {"model", "test-model"}}, "query");
}

TEST(ProviderProfileContractTest, AllowsPlaintextOnlyForExactLoopbackHosts) {
    for (const auto endpoint :
         {"http://localhost:8080/v1/chat/completions", "http://LOCALHOST:8080/v1/chat/completions",
          "http://127.0.0.1:8080/v1/chat/completions",
          "http://127.255.1.2:65535/v1/chat/completions", "http://[::1]:8080/v1/chat/completions",
          "http://[0:0:0:0:0:0:0:1]:8080/v1/chat/completions"}) {
        SCOPED_TRACE(endpoint);
        EXPECT_NO_THROW((void)mint::resolve_model_provider_profile(config_for(endpoint)));
    }

    const auto expect_rejected = [](std::string endpoint, std::string_view message) {
        try {
            (void)mint::resolve_model_provider_profile(config_for(std::move(endpoint)));
            FAIL() << "expected unsafe or invalid plaintext endpoint to be rejected";
        } catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find(message), std::string::npos) << error.what();
        }
    };
    for (const auto endpoint :
         {"http://models.example.test/v1/chat/completions",
          "http://localhost.example.test/v1/chat/completions",
          "http://127.0.0.1.example.test/v1/chat/completions",
          "http://128.0.0.1/v1/chat/completions", "http://0177.0.0.1/v1/chat/completions",
          "http://127.0.0.1./v1/chat/completions", "http://[::2]/v1/chat/completions",
          "http://[::ffff:127.0.0.1]/v1/chat/completions"}) {
        SCOPED_TRACE(endpoint);
        expect_rejected(endpoint, "https");
    }
    expect_rejected("http://127.0.0.1:invalid/v1/chat/completions", "端口");
    expect_rejected("http://[::1]:70000/v1/chat/completions", "端口");
    expect_rejected("http://::1:8080/v1/chat/completions", "方括号");
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

TEST(ProviderProfileContractTest, EmitsExactlyOneTokenLimitFieldPerAdapterDialect) {
    auto groq = config_for("https://api.groq.com/openai/v1/chat/completions");
    groq.max_completion_tokens = 321;
    expect_only_token_limit(mint::detail::build_provider_request(groq, messages(), tools()),
                            "max_completion_tokens", 321);

    auto deepseek = config_for("https://api.deepseek.com/chat/completions");
    deepseek.max_completion_tokens = 321;
    expect_only_token_limit(mint::detail::build_provider_request(deepseek, messages(), tools()),
                            "max_tokens", 321);

    auto moonshot = config_for("https://api.moonshot.ai/v1/chat/completions");
    moonshot.max_completion_tokens = 321;
    expect_only_token_limit(mint::detail::build_provider_request(moonshot, messages(), tools()),
                            "max_completion_tokens", 321);

    auto responses =
        config_for("https://api.openai.com/v1/responses", mint::ModelAdapter::responses);
    responses.max_completion_tokens = 321;
    expect_only_token_limit(mint::detail::build_provider_request(responses, messages(), tools()),
                            "max_output_tokens", 321);
}

TEST(ProviderProfileContractTest, EnablesEncryptedReasoningOnlyWhereSupported) {
    auto openai = config_for("https://api.openai.com/v1/responses", mint::ModelAdapter::responses);
    const auto openai_request = mint::detail::build_provider_request(openai, messages(), tools());
    ASSERT_TRUE(openai_request.contains("include"));
    EXPECT_EQ(openai_request.at("include").at(0), "reasoning.encrypted_content");

    auto xai = config_for("https://api.x.ai/v1/responses", mint::ModelAdapter::responses);
    const auto xai_request = mint::detail::build_provider_request(xai, messages(), tools());
    ASSERT_TRUE(xai_request.contains("include"));
    EXPECT_EQ(xai_request.at("include").at(0), "reasoning.encrypted_content");

    auto custom =
        config_for("https://gateway.example.test/v1/responses", mint::ModelAdapter::responses);
    custom.capabilities = mint::ModelProviderCapabilities{
        .function_tools = true,
        .streaming = true,
        .stream_usage = false,
        .stateless_reasoning_replay = false,
        .token_limit_parameter = mint::ModelTokenLimitParameter::max_output_tokens};
    const auto custom_request = mint::detail::build_provider_request(custom, messages(), tools());
    EXPECT_FALSE(custom_request.contains("include"));
}

TEST(ProviderRequestBudgetContractTest, LoadsBudgetAndRejectsUnsafeBoundaries) {
    TemporaryDirectory temporary;
    const auto valid_path = temporary.path() / "budget.json";
    write_json(valid_path, {{"provider", "custom"},
                            {"api_url", "http://127.0.0.1:8080/v1/chat/completions"},
                            {"model", "test-model"},
                            {"max_completion_tokens", 1024},
                            {"max_request_tokens", 8192},
                            {"request_token_safety_margin", 512},
                            {"request_token_estimate_bytes_per_token", 1}});

    const auto config = mint::load_model_provider_config(valid_path);
    EXPECT_EQ(config.max_request_tokens, 8192);
    EXPECT_EQ(config.request_token_safety_margin, 512);
    EXPECT_EQ(config.request_token_estimate_bytes_per_token, 1);

    expect_config_rejected({{"api_url", "https://example.test/v1/chat/completions"},
                            {"model", "test-model"},
                            {"max_request_tokens", -1}},
                           "max_request_tokens");
    expect_config_rejected({{"api_url", "https://example.test/v1/chat/completions"},
                            {"model", "test-model"},
                            {"max_completion_tokens", 1024},
                            {"max_request_tokens", 1280},
                            {"request_token_safety_margin", 256}},
                           "max_request_tokens");
    expect_config_rejected({{"api_url", "https://example.test/v1/chat/completions"},
                            {"model", "test-model"},
                            {"max_request_tokens", 512},
                            {"request_token_safety_margin", 512}},
                           "request_token_safety_margin");
    expect_config_rejected({{"api_url", "https://example.test/v1/chat/completions"},
                            {"model", "test-model"},
                            {"request_token_estimate_bytes_per_token", 0}},
                           "request_token_estimate_bytes_per_token");
    expect_config_rejected({{"api_url", "https://example.test/v1/chat/completions"},
                            {"model", "test-model"},
                            {"request_token_estimate_bytes_per_token", 9}},
                           "request_token_estimate_bytes_per_token");
}

TEST(ProviderRequestBudgetContractTest, SaturatesReservedTokensWithoutUnsignedUnderflow) {
    const mint::ModelRequestLimits unbounded;
    EXPECT_FALSE(unbounded.bounded());
    EXPECT_EQ(unbounded.available_input_tokens(), 0U);

    const mint::ModelRequestLimits usable{.max_request_tokens = 8192,
                                          .reserved_output_tokens = 1024,
                                          .safety_margin_tokens = 256,
                                          .request_overhead_tokens = 512};
    EXPECT_TRUE(usable.bounded());
    EXPECT_EQ(usable.available_input_tokens(), 6400U);
    EXPECT_EQ(usable.available_input_bytes(), 12'800U);
    EXPECT_EQ(usable.estimated_tokens(301), 151U);

    const mint::ModelRequestLimits dense_input{.max_request_tokens = 8192,
                                               .reserved_output_tokens = 1024,
                                               .safety_margin_tokens = 256,
                                               .request_overhead_tokens = 512,
                                               .request_token_estimate_bytes_per_token = 1};
    EXPECT_EQ(dense_input.available_input_bytes(), 6400U);
    EXPECT_EQ(dense_input.estimated_tokens(301), 301U);

    const mint::ModelRequestLimits exhausted{.max_request_tokens = 1024,
                                             .reserved_output_tokens = 768,
                                             .safety_margin_tokens = 128,
                                             .request_overhead_tokens = 128};
    EXPECT_EQ(exhausted.available_input_tokens(), 0U);

    const mint::ModelRequestLimits exceeded{.max_request_tokens = 1024,
                                            .reserved_output_tokens = 900,
                                            .safety_margin_tokens = 200,
                                            .request_overhead_tokens = 300};
    EXPECT_EQ(exceeded.available_request_tokens(), 0U);
    EXPECT_EQ(exceeded.available_input_tokens(), 0U);
}

TEST(ProviderRequestBudgetContractTest, UsesCompatibilityBudgetBeforeHeadersAreAvailable) {
    auto config = config_for("http://127.0.0.1:8080/v1/chat/completions");
    config.provider = mint::ModelProvider::custom;
    mint::ModelProviderClient client(config);

    const auto without_tools = client.request_limits(mint::Json::array());
    const auto with_tools = client.request_limits(tools());
    EXPECT_EQ(with_tools.max_request_tokens, 8000U);
    EXPECT_EQ(with_tools.max_request_tokens_source, mint::ModelRequestLimitSource::automatic);
    EXPECT_EQ(with_tools.response_header_max_request_tokens, 0U);
    EXPECT_EQ(with_tools.request_token_estimate_bytes_per_token, 2U);
    EXPECT_EQ(with_tools.reserved_output_tokens, 1024U);
    EXPECT_EQ(with_tools.safety_margin_tokens, 256U);
    EXPECT_GT(with_tools.request_overhead_tokens, without_tools.request_overhead_tokens);
    EXPECT_GT(with_tools.available_input_tokens(), 0U);

    config.max_request_tokens = 16'384;
    mint::ModelProviderClient explicitly_bounded(config);
    const auto explicit_limits = explicitly_bounded.request_limits(tools());
    EXPECT_EQ(explicit_limits.max_request_tokens, 16'384U);
    EXPECT_EQ(explicit_limits.max_request_tokens_source, mint::ModelRequestLimitSource::config);
}

TEST(ProviderConfigContractTest, LoadsCommittedProviderTemplatesWithoutSecrets) {
    struct ExpectedProfile {
        const char* file;
        mint::ModelProvider provider;
        mint::ModelAdapter adapter;
        const char* api_key_env;
        const char* endpoint;
    };
    const ExpectedProfile expected[] = {
        {"openai-responses.json", mint::ModelProvider::openai, mint::ModelAdapter::responses,
         "OPENAI_API_KEY", "https://api.openai.com/v1/responses"},
        {"openai-codex.json", mint::ModelProvider::openai, mint::ModelAdapter::responses,
         "OPENAI_API_KEY", "https://api.openai.com/v1/responses"},
        {"claude-messages.json", mint::ModelProvider::anthropic,
         mint::ModelAdapter::anthropic_messages, "ANTHROPIC_API_KEY",
         "https://api.anthropic.com/v1/messages"},
        {"gemini-chat.json", mint::ModelProvider::google, mint::ModelAdapter::chat_completions,
         "GEMINI_API_KEY",
         "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions"},
        {"grok-responses.json", mint::ModelProvider::xai, mint::ModelAdapter::responses,
         "XAI_API_KEY", "https://api.x.ai/v1/responses"},
        {"kimi-chat.json", mint::ModelProvider::moonshot, mint::ModelAdapter::chat_completions,
         "MOONSHOT_API_KEY", "https://api.moonshot.ai/v1/chat/completions"},
        {"groq-chat.json", mint::ModelProvider::groq, mint::ModelAdapter::chat_completions,
         "GROQ_API_KEY", "https://api.groq.com/openai/v1/chat/completions"},
        {"deepseek-chat.json", mint::ModelProvider::deepseek, mint::ModelAdapter::chat_completions,
         "DEEPSEEK_API_KEY", "https://api.deepseek.com/chat/completions"},
        {"custom-chat.json", mint::ModelProvider::custom, mint::ModelAdapter::chat_completions,
         "MINT_MODEL_API_KEY", "http://127.0.0.1:8080/v1/chat/completions"},
    };

    for (const auto& item : expected) {
        SCOPED_TRACE(item.file);
        const auto config = mint::load_model_provider_config(fixed_config(item.file));
        const auto profile = mint::resolve_model_provider_profile(config);
        EXPECT_EQ(profile.provider, item.provider);
        EXPECT_EQ(profile.adapter, item.adapter);
        EXPECT_EQ(profile.source, mint::ModelProviderSource::explicit_config);
        EXPECT_EQ(config.api_key_env, item.api_key_env);
        EXPECT_EQ(config.api_url, item.endpoint);
        EXPECT_TRUE(config.api_key.empty());
        EXPECT_FALSE(config.model.empty());
    }
}

TEST(ProviderConfigContractTest, AcceptsMainstreamProductAliasesWithSafeDefaults) {
    TemporaryDirectory temporary;
    struct AliasExpectation {
        const char* alias;
        mint::ModelProvider provider;
        mint::ModelAdapter adapter;
        const char* endpoint;
    };
    const AliasExpectation aliases[] = {
        {"claude", mint::ModelProvider::anthropic, mint::ModelAdapter::anthropic_messages,
         "https://api.anthropic.com/v1/messages"},
        {"codex", mint::ModelProvider::openai, mint::ModelAdapter::responses,
         "https://api.openai.com/v1/responses"},
        {"gemini", mint::ModelProvider::google, mint::ModelAdapter::chat_completions,
         "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions"},
        {"grok", mint::ModelProvider::xai, mint::ModelAdapter::chat_completions,
         "https://api.x.ai/v1/chat/completions"},
        {"kimi", mint::ModelProvider::moonshot, mint::ModelAdapter::chat_completions,
         "https://api.moonshot.ai/v1/chat/completions"},
    };

    for (const auto& item : aliases) {
        SCOPED_TRACE(item.alias);
        const auto path = temporary.path() / (std::string(item.alias) + ".json");
        write_json(path, {{"provider", item.alias}, {"model", "test-model"}});
        const auto config = mint::load_model_provider_config(path);
        const auto profile = mint::resolve_model_provider_profile(config);
        EXPECT_EQ(profile.provider, item.provider);
        EXPECT_EQ(profile.adapter, item.adapter);
        EXPECT_EQ(config.api_url, item.endpoint);
    }
}

TEST(ProviderConfigContractTest, RejectsContradictoryProfilesAndCredentials) {
    expect_config_rejected({{"provider", "groq"},
                            {"adapter", "responses"},
                            {"api_url", "https://api.groq.com/openai/v1/responses"},
                            {"model", "test-model"}},
                           "不支持 responses");
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

TEST(ProviderConfigContractTest, RejectsRemotePlaintextWithOrWithoutCredentials) {
    expect_config_rejected(
        {{"api_url", "http://models.example.test/v1/chat/completions"}, {"model", "test-model"}},
        "https");
    expect_config_rejected({{"api_url", "http://models.example.test/v1/chat/completions"},
                            {"api_key", "must-not-be-sent"},
                            {"model", "test-model"}},
                           "https");
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
    EXPECT_EQ(report.at("limits").at("max_request_tokens"), 8000);
    EXPECT_EQ(report.at("limits").at("max_request_tokens_source"), "config");
    EXPECT_TRUE(report.at("limits").at("response_header_max_request_tokens").is_null());
    EXPECT_EQ(report.at("limits").at("request_token_safety_margin"), 256);
    EXPECT_EQ(report.at("limits").at("request_token_estimate_bytes_per_token"), 2);
    EXPECT_EQ(report.at("limits").at("max_completion_tokens"), 1024);
    EXPECT_EQ(report.at("limits").at("max_attempts_per_request"), 3);
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
                {"model", "test-model"},
                {"max_request_tokens", 8192},
                {"request_token_safety_margin", 384}});
    command_line.config = inline_key_config;
    output.str({});
    output.clear();
    EXPECT_EQ(mint::cli::run_provider_command(command_line, console), 0);
    EXPECT_EQ(output.str().find("do-not-print-this"), std::string::npos);
    EXPECT_EQ(output.str().find("url-secret"), std::string::npos);
    const auto inline_report = mint::Json::parse(output.str());
    EXPECT_EQ(inline_report.at("limits").at("max_request_tokens"), 8192);
    EXPECT_EQ(inline_report.at("limits").at("max_request_tokens_source"), "config");
    EXPECT_TRUE(inline_report.at("limits").at("response_header_max_request_tokens").is_null());
    EXPECT_EQ(inline_report.at("limits").at("request_token_safety_margin"), 384);
    EXPECT_EQ(inline_report.at("limits").at("request_token_estimate_bytes_per_token"), 2);
}

TEST(ProviderCliContractTest, HumanReportEscapesTerminalControls) {
    TemporaryDirectory temporary;
    const auto config_path = temporary.path() / "terminal.json";
    const auto unsafe_model =
        std::string("model-") + '\x1B' + "]0;provider" + '\x07' + "-" + "\xE2\x80\xAE" + "txt";
    write_json(config_path, {{"provider", "custom"},
                             {"api_url", "https://example.test/v1/chat/completions"},
                             {"model", unsafe_model}});

    mint::cli::CommandLine command_line;
    command_line.mode = mint::cli::CommandMode::provider;
    command_line.config = config_path;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    mint::cli::Console console(input, output, error);

    EXPECT_EQ(mint::cli::run_provider_command(command_line, console), 0);
    EXPECT_TRUE(error.str().empty());
    EXPECT_EQ(output.str().find('\x1B'), std::string::npos);
    EXPECT_EQ(output.str().find("\xE2\x80\xAE"), std::string::npos);
    EXPECT_NE(output.str().find("model-\\u001B]0;provider\\u0007-\\u202Etxt"), std::string::npos);
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
