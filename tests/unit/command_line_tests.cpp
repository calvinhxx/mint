#include "command_line.hpp"
#include "console.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

mint::cli::CommandLine parse(std::vector<std::string> values) {
    std::vector<char*> arguments;
    arguments.reserve(values.size());
    for (auto& value : values) {
        arguments.push_back(value.data());
    }
    return mint::cli::parse_arguments(static_cast<int>(arguments.size()), arguments.data());
}

TEST(CommandLineTest, AcceptsManagedJsonInteractionMode) {
    const auto command = parse({"mint", "run", "--json", "--interaction-jsonl", "检查项目"});

    EXPECT_EQ(command.mode, mint::cli::CommandMode::run);
    EXPECT_TRUE(command.json_output);
    EXPECT_TRUE(command.interaction_jsonl);
    EXPECT_EQ(command.question, "检查项目");
}

TEST(CommandLineTest, InteractionModeRequiresMachineOutput) {
    EXPECT_THROW((void)parse({"mint", "run", "--interaction-jsonl", "检查项目"}),
                 std::invalid_argument);
}

TEST(CommandLineTest, ManagedResumeAcceptsInteractionAndCancelFile) {
    const auto cancel_file =
        std::filesystem::temp_directory_path() /
        ("mint-resume-cancel-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto command = parse({"mint", "resume", "--task", "task-7", "--json",
                                "--interaction-jsonl", "--cancel-file", cancel_file.string()});

    EXPECT_EQ(command.mode, mint::cli::CommandMode::resume);
    EXPECT_EQ(command.task_id, "task-7");
    EXPECT_EQ(command.cancel_file, cancel_file);
    EXPECT_TRUE(command.interaction_jsonl);
}

TEST(CommandLineTest, CancelFileRequiresInteractionMode) {
    EXPECT_THROW((void)parse({"mint", "run", "--json", "--cancel-file", "/tmp/cancel", "检查项目"}),
                 std::invalid_argument);
}

TEST(CommandLineTest, CancelFileNeverAcceptsAnExistingPath) {
    const auto marker =
        std::filesystem::temp_directory_path() /
        ("mint-existing-cancel-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream output(marker);
        output << "belongs to the caller";
    }

    EXPECT_THROW((void)parse({"mint", "run", "--json", "--interaction-jsonl", "--cancel-file",
                              marker.string(), "检查项目"}),
                 std::invalid_argument);
    EXPECT_TRUE(std::filesystem::exists(marker));
    std::filesystem::remove(marker);
}

TEST(CommandLineTest, InteractionModeDoesNotLeakIntoHumanOrProviderFlows) {
    EXPECT_THROW((void)parse({"mint", "provider", "--json", "--interaction-jsonl"}),
                 std::invalid_argument);
    EXPECT_THROW((void)parse({"mint", "init", "--json", "--interaction-jsonl"}),
                 std::invalid_argument);
}

TEST(CommandLineTest, AcceptsIndependentLocalLogOptions) {
    const auto command = parse({"mint", "provider", "--log-level", "warn", "--log-file-level",
                                "debug", "--log-dir", "/tmp/mint-logs", "--json"});

    EXPECT_EQ(command.log_level, "warn");
    EXPECT_EQ(command.log_file_level, "debug");
    EXPECT_EQ(command.log_dir, std::filesystem::path("/tmp/mint-logs"));
}

TEST(CommandLineTest, LocalLogOptionsRequireValues) {
    EXPECT_THROW((void)parse({"mint", "run", "--log-file-level"}), std::invalid_argument);
    EXPECT_THROW((void)parse({"mint", "run", "--log-dir"}), std::invalid_argument);
}

TEST(CommandLineTest, KeepsLegacyWorkflowOutOfDefaultHelp) {
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    mint::cli::Console console(input, output, error);

    mint::cli::print_help(console, "mint");

    EXPECT_EQ(output.str().find("兼容工作流"), std::string::npos);
    EXPECT_EQ(output.str().find("--allow-write"), std::string::npos);
    EXPECT_NE(output.str().find("--help-legacy"), std::string::npos);
}

TEST(CommandLineTest, ExposesLegacyHelpOnlyWhenRequested) {
    const auto command = parse({"mint", "--help-legacy"});
    EXPECT_TRUE(command.help);
    EXPECT_TRUE(command.legacy_help);

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    mint::cli::Console console(input, output, error);
    mint::cli::print_legacy_help(console, "mint");

    EXPECT_NE(output.str().find("旧版兼容模式"), std::string::npos);
    EXPECT_NE(output.str().find("--allow-write"), std::string::npos);
}

} // namespace
