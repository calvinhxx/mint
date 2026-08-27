#include "diagnostic_log.hpp"

#include <stdexcept>

#include <gtest/gtest.h>

namespace {

class DiagnosticLogTest : public testing::Test {
  protected:
    void TearDown() override {
        mint::diagnostics::configure({});
    }
};

TEST_F(DiagnosticLogTest, UsesWarnAsTheQuietDefault) {
    mint::diagnostics::configure({});

    EXPECT_EQ(mint::diagnostics::current_level(), "warn");
}

TEST_F(DiagnosticLogTest, AcceptsSupportedLevelsAndAliases) {
    mint::diagnostics::configure("debug");
    EXPECT_EQ(mint::diagnostics::current_level(), "debug");

    mint::diagnostics::configure("warning");
    EXPECT_EQ(mint::diagnostics::current_level(), "warn");

    mint::diagnostics::configure("off");
    EXPECT_EQ(mint::diagnostics::current_level(), "off");
}

TEST_F(DiagnosticLogTest, AppliesSeverityThresholds) {
    using mint::diagnostics::Level;

    mint::diagnostics::configure("info");

    EXPECT_FALSE(mint::diagnostics::enabled(Level::debug));
    EXPECT_TRUE(mint::diagnostics::enabled(Level::info));
    EXPECT_TRUE(mint::diagnostics::enabled(Level::warning));
}

TEST_F(DiagnosticLogTest, EmitsStructuredEventsWithoutAffectingControlFlow) {
    using mint::diagnostics::Level;

    mint::diagnostics::configure("debug");

    EXPECT_NO_THROW(
        mint::diagnostics::emit(Level::debug, "test.event", {{"count", 2}, {"completed", true}}));
    EXPECT_NO_THROW(mint::diagnostics::flush());
}

TEST_F(DiagnosticLogTest, RejectsUnknownLevels) {
    EXPECT_THROW(mint::diagnostics::configure("verbose"), std::invalid_argument);
}

} // namespace
