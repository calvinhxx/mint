#include "infrastructure/cli/mint_cli_protocol.hpp"

#include <gtest/gtest.h>

namespace {

using mint::desktop::ApprovalKind;
using mint::desktop::RunEventKind;
using mint::infrastructure::desktop::InteractionKind;
using mint::infrastructure::desktop::MintCliProtocol;

TEST(DesktopProtocol, NormalizesEventJsonIntoDesktopTypes) {
    const auto event = MintCliProtocol::parse_event(
        R"({"seq":3,"timestamp":"now","type":"tool_started","data":{"name":"read_file","turn":2,"arguments_summary":{"path":"README.md"}}})");

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, RunEventKind::tool_started);
    EXPECT_EQ(event->name, "read_file");
    EXPECT_EQ(event->target, "README.md");
    ASSERT_TRUE(event->turn.has_value());
    EXPECT_EQ(*event->turn, 2U);
}

TEST(DesktopProtocol, RejectsUnrelatedObjectsAndIncompleteTaskResults) {
    for (const auto* payload : {"{}", R"({"status":""})", R"({"status":"completed"})",
                                R"({"status":"completed","answer":3})",
                                R"({"error":"failed","status":"failed","answer":"bad"})"}) {
        EXPECT_FALSE(MintCliProtocol::parse_result(payload).has_value()) << payload;
    }
    const auto result = MintCliProtocol::parse_result(
        R"({"status":"max_turns","answer":"Reached the turn limit; the task can resume."})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, "max_turns");
}

TEST(DesktopProtocol, NormalizesApprovalAndFinalUsage) {
    const auto interaction = MintCliProtocol::parse_interaction(
        R"({"channel":"mint_interaction","type":"changeset_approval","data":{"paths":["a.cpp","b.cpp"],"unified_diff":"diff","diff_truncated":false}})");
    ASSERT_TRUE(interaction.has_value());
    EXPECT_EQ(interaction->kind, InteractionKind::approval);
    ASSERT_TRUE(interaction->approval.has_value());
    EXPECT_EQ(interaction->approval->kind, ApprovalKind::changeset);
    EXPECT_EQ(interaction->approval->paths.size(), 2U);

    const auto result = MintCliProtocol::parse_result(
        R"({"status":"completed","answer":"Done","changes":{"unified_diff":"diff"},"model":{"calls":2,"total_tokens":80,"prompt_tokens":40,"cached_tokens":10,"usage_reports":2,"cache_usage_reports":2}})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->answer, "Done");
    EXPECT_EQ(result->usage.calls, 2U);
    EXPECT_EQ(result->usage.total_tokens, 80U);
    EXPECT_EQ(result->usage.cache_hit_percent(), 25U);
}

TEST(DesktopProtocol, MissingOrPartialCacheReportsDoNotBecomeZeroPercent) {
    for (
        const auto* metadata :
        {R"({"prompt_tokens":40,"cached_tokens":0})",
         R"({"calls":2,"prompt_tokens":40,"cached_tokens":10,"usage_reports":2,"cache_usage_reports":1})",
         R"({"calls":2,"prompt_tokens":40,"cached_tokens":10,"usage_reports":1,"cache_usage_reports":1})"}) {
        const auto result = MintCliProtocol::parse_result(
            std::string(R"({"status":"completed","answer":"Done","model":)") + metadata + "}");
        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(result->usage.cache_hit_percent().has_value());
    }
    const auto reported_zero = MintCliProtocol::parse_result(
        R"({"status":"completed","answer":"Done","model":{"calls":2,"prompt_tokens":40,"cached_tokens":0,"usage_reports":2,"cache_usage_reports":2}})");
    ASSERT_TRUE(reported_zero.has_value());
    EXPECT_EQ(reported_zero->usage.cache_hit_percent(), 0U);
}

} // namespace
