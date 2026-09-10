#include "mint/platform/path.hpp"
#include "mint/platform/system.hpp"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace {

TEST(PlatformPathTest, PreservesUnicodeAcrossNativeAndGenericRepresentations) {
    const std::string text = "项目/配置/🟢.json";
    auto path = mint::platform::path_from_utf8(text);
    EXPECT_EQ(path, std::filesystem::path(u8"项目/配置/🟢.json"));
    path.make_preferred();
    EXPECT_EQ(mint::platform::path_to_generic_utf8(path), text);
    EXPECT_EQ(mint::platform::path_from_utf8(mint::platform::path_to_utf8(path)), path);
    EXPECT_EQ(mint::platform::path_to_utf8(path.filename()), "🟢.json");
}

TEST(PlatformPathTest, ConversionDoesNotResolveRelativePathsOrEmptyInput) {
    const std::string text = "项目/../配置.json";
    const auto path = mint::platform::path_from_utf8(text);
    EXPECT_TRUE(path.is_relative());
    EXPECT_EQ(mint::platform::path_to_generic_utf8(path), text);
    EXPECT_TRUE(mint::platform::path_from_utf8("").empty());
    EXPECT_TRUE(mint::platform::path_to_utf8({}).empty());
    EXPECT_TRUE(mint::platform::path_to_generic_utf8({}).empty());
}

TEST(PlatformSystemTest, UtcConversionHandlesLeapDaysAndReturnsIndependentValues) {
    const auto epoch = mint::platform::utc_time(0);
    const auto leap_day = mint::platform::utc_time(951782400);
    ASSERT_TRUE(epoch.has_value());
    ASSERT_TRUE(leap_day.has_value());
    EXPECT_EQ(epoch->tm_year, 70);
    EXPECT_EQ(epoch->tm_mon, 0);
    EXPECT_EQ(epoch->tm_mday, 1);
    EXPECT_EQ(epoch->tm_hour, 0);
    EXPECT_EQ(epoch->tm_min, 0);
    EXPECT_EQ(epoch->tm_sec, 0);
    EXPECT_EQ(leap_day->tm_year, 100);
    EXPECT_EQ(leap_day->tm_mon, 1);
    EXPECT_EQ(leap_day->tm_mday, 29);
    EXPECT_EQ(leap_day->tm_yday, 59);
    EXPECT_EQ(leap_day->tm_isdst, 0);
}

TEST(PlatformSystemTest, ProcessIdentityIsPositiveAndStable) {
    const auto process = mint::platform::process_id();
    EXPECT_GT(process, 0);
    EXPECT_EQ(mint::platform::process_id(), process);
}

} // namespace
