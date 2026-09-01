#include "console.hpp"

#include "mint/runtime/terminal_text.hpp"

#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

TEST(ConsoleTest, KeepsStandardOutputAndErrorSeparate) {
    std::istringstream input("yes\n");
    std::ostringstream output;
    std::ostringstream error;
    mint::cli::Console console(input, output, error);

    std::string answer;
    ASSERT_TRUE(console.read_line(answer));
    console.write("answer=", answer);
    console.write_line(" code=", 0);
    console.write_error_line("warning=", 1);

    EXPECT_EQ(output.str(), "answer=yes code=0\n");
    EXPECT_EQ(error.str(), "warning=1\n");
}

TEST(ConsoleTest, ReportsInputExhaustionWithoutChangingOutput) {
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    mint::cli::Console console(input, output, error);

    std::string value = "unchanged";

    EXPECT_FALSE(console.read_line(value));
    EXPECT_EQ(value, "unchanged");
    EXPECT_TRUE(output.str().empty());
    EXPECT_TRUE(error.str().empty());
}

TEST(TerminalTextTest, EscapesTerminalAndBidirectionalControlsWithoutDamagingUtf8) {
    const auto unsafe = std::string("普通中文\n\t") + '\x1B' + "]52;c;clipboard" + '\x07' + '\x1B' +
                        "[2J" + "\xC2\x9B" + "31m" + "\xE2\x80\xAE";

    EXPECT_EQ(mint::escape_terminal_text(unsafe),
              "普通中文\n\t\\u001B]52;c;clipboard\\u0007\\u001B[2J\\u009B31m\\u202E");
}

TEST(TerminalTextTest, RendersInvalidUtf8BytesInsteadOfForwardingThem) {
    const auto invalid = std::string("before") + static_cast<char>(0xFF) + "after";

    EXPECT_EQ(mint::escape_terminal_text(invalid), "before\\xFFafter");
}

TEST(TerminalTextTest, SingleLineFieldsAlsoEscapeLayoutCharacters) {
    EXPECT_EQ(mint::escape_terminal_field("before\n\tafter"), "before\\u000A\\u0009after");
}

} // namespace
