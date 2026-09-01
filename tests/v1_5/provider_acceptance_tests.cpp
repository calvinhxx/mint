#include "provider_acceptance.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr auto challenge = "mint-provider-acceptance-v1";
constexpr auto receipt = "MINT_PROVIDER_ACCEPTANCE_OK";

mint::ModelCallMetadata metadata(std::size_t attempts, std::size_t retries, long long duration_ms,
                                 std::size_t stream_events, std::size_t streamed_bytes) {
    return {.adapter = "responses",
            .provider = "custom",
            .model = "acceptance-model",
            .attempts = attempts,
            .retries = retries,
            .http_status = 200,
            .duration_ms = duration_ms,
            .streamed = true,
            .stream_events = stream_events,
            .streamed_bytes = streamed_bytes};
}

mint::ModelReply tool_reply(std::string argument = challenge) {
    mint::ModelReply reply;
    reply.assistant_message = {
        {"role", "assistant"},
        {"content", nullptr},
        {"tool_calls",
         mint::Json::array({{{"id", "call-acceptance"},
                             {"type", "function"},
                             {"function",
                              {{"name", "mint_acceptance_echo"},
                               {"arguments", mint::Json({{"challenge", argument}}).dump()}}}}})}};
    reply.tool_calls.push_back(
        {"call-acceptance", "mint_acceptance_echo", {{"challenge", std::move(argument)}}});
    reply.usage = {.available = true,
                   .prompt_tokens = 11,
                   .completion_tokens = 4,
                   .total_tokens = 15,
                   .cached_tokens = 1};
    reply.metadata = metadata(2, 1, 12, 3, 50);
    return reply;
}

mint::ModelReply final_reply(std::string text = receipt) {
    mint::ModelReply reply;
    reply.assistant_message = {{"role", "assistant"}, {"content", text}};
    reply.text = std::move(text);
    reply.usage = {.available = true,
                   .prompt_tokens = 15,
                   .completion_tokens = 3,
                   .total_tokens = 18,
                   .cached_tokens = 2};
    reply.metadata = metadata(1, 0, 8, 2, 40);
    return reply;
}

class ScriptedModelClient final : public mint::ModelClient {
  public:
    explicit ScriptedModelClient(std::vector<mint::ModelReply> replies)
        : replies_(std::move(replies)) {}

    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        messages_.push_back(messages);
        tools_.push_back(tools);
        if (next_ >= replies_.size()) {
            throw std::runtime_error("acceptance client received too many requests");
        }
        return std::move(replies_.at(next_++));
    }

    [[nodiscard]] const std::vector<mint::Json>& messages() const noexcept {
        return messages_;
    }

    [[nodiscard]] const std::vector<mint::Json>& tools() const noexcept {
        return tools_;
    }

  private:
    std::vector<mint::ModelReply> replies_;
    std::vector<mint::Json> messages_;
    std::vector<mint::Json> tools_;
    std::size_t next_ = 0;
};

TEST(ProviderAcceptanceContractTest, VerifiesTwoRequestToolRoundTripWithoutLeakingContent) {
    ScriptedModelClient client({tool_reply(), final_reply()});

    const auto report = mint::cli::provider_detail::run_provider_acceptance(client, true);

    EXPECT_EQ(report.at("requests"), 2);
    EXPECT_EQ(report.at("attempts"), 3);
    EXPECT_EQ(report.at("retries"), 1);
    EXPECT_EQ(report.at("duration_ms"), 20);
    EXPECT_EQ(report.at("streamed_requests"), 2);
    EXPECT_EQ(report.at("stream_events"), 5);
    EXPECT_EQ(report.at("streamed_bytes"), 90);
    EXPECT_EQ(report.at("reported_provider"), "custom");
    EXPECT_EQ(report.at("reported_adapter"), "responses");
    EXPECT_EQ(report.at("reported_model"), "acceptance-model");
    EXPECT_EQ(report.at("usage").at("reported_requests"), 2);
    EXPECT_EQ(report.at("usage").at("total_tokens"), 33);
    EXPECT_EQ(report.at("usage").at("cached_tokens"), 3);
    EXPECT_NEAR(report.at("usage").at("cache_hit_rate").get<double>(), 3.0 / 26.0, 1e-12);
    EXPECT_TRUE(report.at("checks").at("function_call"));
    EXPECT_TRUE(report.at("checks").at("arguments_round_trip"));
    EXPECT_TRUE(report.at("checks").at("tool_result_continuation"));
    EXPECT_EQ(report.dump().find(receipt), std::string::npos);

    ASSERT_EQ(client.messages().size(), 2);
    ASSERT_EQ(client.tools().size(), 2);
    ASSERT_EQ(client.messages().at(0).size(), 2);
    ASSERT_EQ(client.messages().at(1).size(), 4);
    const auto& tool_message = client.messages().at(1).back();
    EXPECT_EQ(tool_message.at("role"), "tool");
    EXPECT_EQ(tool_message.at("tool_call_id"), "call-acceptance");
    const auto tool_result = mint::Json::parse(tool_message.at("content").get<std::string>());
    EXPECT_TRUE(tool_result.at("ok"));
    EXPECT_EQ(tool_result.at("challenge"), challenge);
    EXPECT_EQ(tool_result.at("receipt"), receipt);
    EXPECT_EQ(client.tools().at(0), client.tools().at(1));
    EXPECT_EQ(client.tools().at(0).at(0).at("function").at("name"), "mint_acceptance_echo");
}

TEST(ProviderAcceptanceContractTest, ReportsNullCacheRateWithoutInputTokens) {
    auto first = tool_reply();
    first.usage.prompt_tokens = 0;
    first.usage.cached_tokens = 0;
    first.usage.total_tokens = first.usage.completion_tokens;
    auto second = final_reply();
    second.usage.prompt_tokens = 0;
    second.usage.cached_tokens = 0;
    second.usage.total_tokens = second.usage.completion_tokens;
    ScriptedModelClient client({std::move(first), std::move(second)});

    const auto report = mint::cli::provider_detail::run_provider_acceptance(client, true);

    EXPECT_TRUE(report.at("usage").at("cache_hit_rate").is_null());
}

TEST(ProviderAcceptanceContractTest, RejectsProtocolMismatches) {
    ScriptedModelClient wrong_arguments({tool_reply("wrong-challenge")});
    EXPECT_THROW((void)mint::cli::provider_detail::run_provider_acceptance(wrong_arguments, true),
                 std::runtime_error);
    EXPECT_EQ(wrong_arguments.messages().size(), 1);

    ScriptedModelClient missing_receipt(
        {tool_reply(), final_reply("compatibility check finished")});
    EXPECT_THROW((void)mint::cli::provider_detail::run_provider_acceptance(missing_receipt, true),
                 std::runtime_error);
    EXPECT_EQ(missing_receipt.messages().size(), 2);
}

} // namespace
