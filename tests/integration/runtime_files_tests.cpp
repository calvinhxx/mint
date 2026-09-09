#include "mint/infrastructure/event_log.hpp"
#include "mint/infrastructure/session_store.hpp"

#include "test_workspace.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using mint::test::read_text;
using mint::test::TemporaryDirectory;
using mint::test::write_text;

TEST(RuntimeFilesTest, ProtectsRuntimeFiles) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "existing.txt";
    write_text(target, "must not be overwritten\n");

    const auto append_path = temporary.path() / "append-events.jsonl";
    {
        mint::EventLog first(append_path);
        first.emit("first");
    }
    {
        mint::EventLog second(append_path, true);
        second.emit("second");
    }
    std::ifstream appended_input(append_path, std::ios::binary);
    std::string first_line;
    std::string second_line;
    std::getline(appended_input, first_line);
    std::getline(appended_input, second_line);
    EXPECT_TRUE(mint::Json::parse(first_line).at("seq") == 1 &&
                mint::Json::parse(second_line).at("seq") == 2)
        << ("resumed JSONL logging continues the prior sequence");

    const auto partial_path = temporary.path() / "partial-events.jsonl";
    write_text(partial_path, R"({"schema_version":1,"seq":4,"type":"stable","data":{}})"
                             "\n{\"schema_version\":1,\"seq\":5");
    {
        mint::EventLog recovered(partial_path, true);
        recovered.emit("after_crash");
    }
    std::ifstream recovered_input(partial_path, std::ios::binary);
    std::string recovered_line;
    std::vector<std::string> recovered_lines;
    while (std::getline(recovered_input, recovered_line)) {
        recovered_lines.push_back(recovered_line);
    }
    EXPECT_TRUE(recovered_lines.size() == 3 &&
                mint::Json::parse(recovered_lines.back()).at("seq") == 5)
        << ("JSONL recovery separates a crash-truncated final line from new events");

    std::error_code symlink_error;
    const auto event_link = temporary.path() / "events-link.jsonl";
    std::filesystem::create_symlink(target, event_link, symlink_error);
    if (!symlink_error) {
        bool event_rejected = false;
        bool session_rejected = false;
        try {
            mint::EventLog events(event_link);
        } catch (const std::invalid_argument&) {
            event_rejected = true;
        }
        const auto session_link = temporary.path() / "session-link.json";
        std::filesystem::create_symlink(target, session_link, symlink_error);
        try {
            mint::SessionStore session(session_link);
        } catch (const std::invalid_argument&) {
            session_rejected = true;
        }
        EXPECT_TRUE(event_rejected && session_rejected)
            << ("runtime files reject symbolic-link targets");
        EXPECT_TRUE(read_text(target) == "must not be overwritten\n")
            << ("runtime symlink rejection leaves the target unchanged");
    }

    std::error_code hard_link_error;
    const auto hard_link = temporary.path() / "events-hardlink.jsonl";
    std::filesystem::create_hard_link(target, hard_link, hard_link_error);
    if (!hard_link_error) {
        bool rejected = false;
        try {
            mint::EventLog events(hard_link);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected) << ("event log rejects an existing file with multiple hard links");
        EXPECT_TRUE(read_text(target) == "must not be overwritten\n")
            << ("hard-link rejection leaves the shared inode unchanged");
    }

    const auto existing_directory = temporary.path() / "runtime-directory";
    std::filesystem::create_directory(existing_directory);
    bool event_directory_rejected = false;
    bool session_directory_rejected = false;
    try {
        mint::EventLog events(existing_directory);
    } catch (const std::invalid_argument&) {
        event_directory_rejected = true;
    }
    try {
        mint::SessionStore session(existing_directory);
    } catch (const std::invalid_argument&) {
        session_directory_rejected = true;
    }
    EXPECT_TRUE(event_directory_rejected && session_directory_rejected)
        << ("runtime output paths reject existing non-regular files");
}

} // namespace
