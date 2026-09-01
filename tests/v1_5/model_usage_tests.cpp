#include "mint/application/agent.hpp"

#include "agent/agent_model_summary.hpp"

#include <gtest/gtest.h>

namespace {

TEST(ModelUsageContractTest, UsesNormalizedPromptTokensAsCacheHitDenominator) {
    EXPECT_DOUBLE_EQ(*mint::model_usage::cache_hit_rate(75, 100), 0.75);
    EXPECT_NEAR(*mint::model_usage::cache_hit_rate(1, 3), 1.0 / 3.0, 1e-12);
    EXPECT_FALSE(mint::model_usage::cache_hit_rate(9, 0).has_value());
    EXPECT_DOUBLE_EQ(*mint::model_usage::cache_hit_rate(120, 100), 1.0);
}

TEST(ModelUsageContractTest, SerializesTheSameRateForUsageAndSummary) {
    const mint::ModelUsage usage{.available = true,
                                 .prompt_tokens = 100,
                                 .completion_tokens = 10,
                                 .total_tokens = 110,
                                 .cached_tokens = 75};
    const auto usage_json = mint::agent_detail::model_usage_json(usage);

    mint::ModelSummary summary;
    mint::ModelReply reply;
    reply.usage = usage;
    mint::agent_detail::record_model_call(summary, reply);
    const auto summary_json = mint::agent_detail::model_summary_to_json(summary);

    EXPECT_DOUBLE_EQ(usage_json.at("cache_hit_rate").get<double>(), 0.75);
    EXPECT_DOUBLE_EQ(summary_json.at("cache_hit_rate").get<double>(), 0.75);
}

TEST(ModelUsageContractTest, SerializesNullRateWithoutAnInputDenominator) {
    const mint::ModelUsage usage{.available = true, .completion_tokens = 4, .total_tokens = 4};
    const auto usage_json = mint::agent_detail::model_usage_json(usage);

    mint::ModelSummary summary;
    mint::ModelReply reply;
    reply.usage = usage;
    mint::agent_detail::record_model_call(summary, reply);
    const auto summary_json = mint::agent_detail::model_summary_to_json(summary);

    EXPECT_TRUE(usage_json.at("cache_hit_rate").is_null());
    EXPECT_TRUE(summary_json.at("cache_hit_rate").is_null());
}

} // namespace
