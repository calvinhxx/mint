#include "mint/domain/runtime_settings.hpp"

#include <stdexcept>

#include <gtest/gtest.h>

namespace {

TEST(RuntimeSettings, DefaultsStayWithinHardBounds) {
    const mint::ToolRuntimeSettings settings;

    EXPECT_NO_THROW(mint::validate_tool_runtime_settings(settings));
    EXPECT_GE(settings.read_file_bytes, mint::runtime_bounds::min_read_file_bytes);
    EXPECT_LE(settings.read_file_bytes, mint::runtime_bounds::max_read_file_bytes);
    EXPECT_LE(settings.search_file_bytes, mint::runtime_bounds::max_search_file_bytes);
    EXPECT_LE(settings.command_output_bytes, mint::runtime_bounds::max_command_output_bytes);
}

TEST(RuntimeSettings, ParsesPartialOverridesAndKeepsOtherDefaults) {
    const auto settings =
        mint::parse_tool_runtime_settings({{"read_file_bytes", 4096}, {"search_max_hits", 25}});

    EXPECT_EQ(settings.read_file_bytes, 4096U);
    EXPECT_EQ(settings.search_max_hits, 25U);
    EXPECT_EQ(settings.list_max_entries, mint::runtime_defaults::list_max_entries);
    EXPECT_EQ(settings.command_output_bytes, mint::runtime_defaults::command_output_bytes);
}

TEST(RuntimeSettings, JsonRoundTripPreservesEveryField) {
    mint::ToolRuntimeSettings expected;
    expected.read_file_bytes = 2048;
    expected.list_max_entries = 50;
    expected.search_file_bytes = 512 * 1024;
    expected.search_max_hits = 20;
    expected.search_max_files = 400;
    expected.command_output_bytes = 4096;

    const auto actual =
        mint::parse_tool_runtime_settings(mint::tool_runtime_settings_to_json(expected));

    EXPECT_EQ(actual, expected);
}

TEST(RuntimeSettings, RejectsUnknownAndOutOfRangeValues) {
    EXPECT_THROW((void)mint::parse_tool_runtime_settings({{"unknown", 1}}), std::invalid_argument);
    EXPECT_THROW((void)mint::parse_tool_runtime_settings({{"search_max_hits", 0}}),
                 std::invalid_argument);
    EXPECT_THROW((void)mint::parse_tool_runtime_settings(
                     {{"read_file_bytes", mint::runtime_bounds::max_read_file_bytes + 1}}),
                 std::invalid_argument);
}

} // namespace
