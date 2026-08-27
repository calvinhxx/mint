#include "console.hpp"

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

} // namespace
