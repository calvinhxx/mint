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

TEST(ModelUsageContractTest, DistinguishesExplicitZeroCacheHitsFromMissingCacheUsage) {
    mint::ModelUsage usage{.available = true, .prompt_tokens = 100, .total_tokens = 100};
    EXPECT_FALSE(mint::model_usage::cache_hit_rate(usage).has_value());
    EXPECT_FALSE(
        mint::agent_detail::model_usage_json(usage).at("cache_usage_available").get<bool>());
    usage.cache_usage_available = true;
    ASSERT_TRUE(mint::model_usage::cache_hit_rate(usage).has_value());
    EXPECT_DOUBLE_EQ(*mint::model_usage::cache_hit_rate(usage), 0.0);
    EXPECT_TRUE(
        mint::agent_detail::model_usage_json(usage).at("cache_usage_available").get<bool>());
}

TEST(ModelUsageContractTest, DoesNotCountUnknownCacheUsageAsZeroHitsInSummary) {
    mint::ModelSummary summary;
    mint::ModelReply known;
    known.usage = {.available = true,
                   .prompt_tokens = 100,
                   .total_tokens = 100,
                   .cached_tokens = 50,
                   .cache_usage_available = true};
    mint::agent_detail::record_model_call(summary, known);
    EXPECT_DOUBLE_EQ(
        mint::agent_detail::model_summary_to_json(summary).at("cache_hit_rate").get<double>(), 0.5);
    mint::ModelReply unknown;
    unknown.usage = {.available = true, .prompt_tokens = 100, .total_tokens = 100};
    mint::agent_detail::record_model_call(summary, unknown);
    const auto serialized = mint::agent_detail::model_summary_to_json(summary);
    EXPECT_EQ(serialized.at("usage_reports"), 2U);
    EXPECT_EQ(serialized.at("cache_usage_reports"), 1U);
    EXPECT_EQ(serialized.at("cached_tokens"), 50U);
    EXPECT_TRUE(serialized.at("cache_hit_rate").is_null());
    const auto restored = mint::agent_detail::model_summary_from_json(serialized);
    EXPECT_EQ(restored.cache_usage_reports, 1U);
    EXPECT_TRUE(mint::agent_detail::model_summary_to_json(restored).at("cache_hit_rate").is_null());
}

TEST(ModelUsageContractTest, RestoresLegacyCacheTotalsWithoutInventingCoverage) {
    mint::ModelSummary summary;
    mint::ModelReply known;
    known.usage = {
        .available = true, .prompt_tokens = 100, .total_tokens = 100, .cached_tokens = 50};
    mint::agent_detail::record_model_call(summary, known);
    auto legacy = mint::agent_detail::model_summary_to_json(summary);
    legacy.erase("cache_usage_reports");
    const auto restored = mint::agent_detail::model_summary_from_json(legacy);
    EXPECT_EQ(restored.cached_tokens, 50U);
    EXPECT_EQ(restored.cache_usage_reports, 0U);
    EXPECT_TRUE(mint::agent_detail::model_summary_to_json(restored).at("cache_hit_rate").is_null());
}

TEST(ModelUsageContractTest, MissingEntireUsageReportDoesNotProduceATaskCacheRate) {
    mint::ModelSummary summary;
    mint::ModelReply known;
    known.usage = {
        .available = true, .prompt_tokens = 100, .total_tokens = 100, .cached_tokens = 50};
    mint::agent_detail::record_model_call(summary, known);
    mint::agent_detail::record_model_call(summary, mint::ModelReply{});
    const auto result = mint::agent_detail::model_summary_to_json(summary);
    EXPECT_EQ(result.at("calls"), 2U);
    EXPECT_EQ(result.at("usage_reports"), 1U);
    EXPECT_EQ(result.at("cache_usage_reports"), 1U);
    EXPECT_TRUE(result.at("cache_hit_rate").is_null());
}

} // namespace
