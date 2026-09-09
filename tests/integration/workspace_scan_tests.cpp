#include "mint/runtime/task_control.hpp"
#include "mint/tools/tool_registry.hpp"

#include "test_workspace.hpp"
#include "workspace/path_identity.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

namespace {

using mint::test::TemporaryDirectory;
using mint::test::write_text;

TEST(WorkspaceScanTest, EmptyProtectedPathsDoNotHideWorkspace) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "files";
    write_text(workspace / "README.md", "visible\n");
    struct RestoreDirectory {
        std::filesystem::path previous = std::filesystem::current_path();
        ~RestoreDirectory() {
            std::error_code ignored;
            std::filesystem::current_path(previous, ignored);
        }
    } restore;
    std::filesystem::current_path(workspace);
    mint::ToolRegistry tools(".", mint::ToolRegistryOptions{.protected_paths = {{}}});
    const auto result = mint::Json::parse(tools.execute({"list", "list_files", {{"path", "."}}}));
    ASSERT_TRUE(result.at("ok"));
    ASSERT_EQ(result.at("entries").size(), 1U);
    EXPECT_EQ(result.at("entries").front().at("path"), "README.md");
}

TEST(WorkspaceScanTest, SearchPrunesProtectedSubtrees) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "files";
    write_text(workspace / "private" / "nested" / "data.txt", "needle private\n");
    write_text(workspace / "visible.txt", "needle visible\n");
    mint::ToolRuntimeSettings limits;
    limits.workspace_snapshot_entries = 2;
    mint::ToolRegistry tools(
        workspace,
        mint::ToolRegistryOptions{.protected_paths = {workspace / "private"}, .runtime = limits});
    const auto result =
        mint::Json::parse(tools.execute({"search", "search_text", {{"query", "needle"}}}));
    ASSERT_TRUE(result.at("ok"));
    EXPECT_FALSE(result.at("truncated"));
    EXPECT_EQ(result.at("scanned_files"), 1U);
    ASSERT_EQ(result.at("hits").size(), 1U);
    EXPECT_EQ(result.at("hits").front().at("path"), "visible.txt");
}

TEST(WorkspaceScanTest, TraversalBudgetIncludesHiddenEntriesAndEmptyDirectories) {
    for (bool directories : {false, true}) {
        SCOPED_TRACE(directories);
        TemporaryDirectory temporary;
        const auto workspace = temporary.path() / "files";
        std::filesystem::create_directory(workspace);
        for (int index = 0; index < 4; ++index) {
            if (directories) {
                std::filesystem::create_directory(workspace / std::to_string(index));
            } else {
                write_text(workspace / (".env." + std::to_string(index)), "hidden\n");
            }
        }
        mint::ToolRuntimeSettings limits;
        limits.workspace_snapshot_entries = 2;
        mint::ToolRegistry tools(workspace, mint::ToolRegistryOptions{.runtime = limits});
        const auto listed = mint::Json::parse(
            tools.execute({"list", "list_files", {{"path", "."}, {"max_depth", 1}}}));
        ASSERT_TRUE(listed.at("ok"));
        EXPECT_TRUE(listed.at("truncated"));
        EXPECT_EQ(listed.at("entries").size(), directories ? 2U : 0U);
        const auto searched =
            mint::Json::parse(tools.execute({"search", "search_text", {{"query", "missing"}}}));
        ASSERT_TRUE(searched.at("ok"));
        EXPECT_TRUE(searched.at("truncated"));
        EXPECT_EQ(searched.at("scanned_files"), 0U);
        EXPECT_TRUE(searched.at("hits").empty());
    }
}

TEST(WorkspaceScanTest, SearchKeepsLineNumbersAndSkipsBinaryData) {
    TemporaryDirectory temporary;
    write_text(temporary.path() / "text.txt", "needle\n\nNEEDLE\r\nneedle at end");
    write_text(temporary.path() / "binary.dat", std::string(4096, 'x') + '\0' + "needle\n");
    mint::ToolRegistry tools(temporary.path());
    const auto result =
        mint::Json::parse(tools.execute({"search", "search_text", {{"query", "needle"}}}));
    ASSERT_TRUE(result.at("ok"));
    EXPECT_FALSE(result.at("truncated"));
    EXPECT_EQ(result.at("scanned_files"), 2U);
    ASSERT_EQ(result.at("hits").size(), 3U);
    EXPECT_EQ(result.at("hits").at(0).at("line"), 1U);
    EXPECT_EQ(result.at("hits").at(1).at("line"), 3U);
    EXPECT_EQ(result.at("hits").at(2).at("line"), 4U);
    for (const auto& hit : result.at("hits")) {
        EXPECT_EQ(hit.at("path"), "text.txt");
    }
}

TEST(WorkspaceScanTest, ReadToolsRespectCancellationAndDeadlines) {
    TemporaryDirectory temporary;
    write_text(temporary.path() / "text.txt", "needle\n");
    const auto cancelled = std::make_shared<mint::TaskControl>();
    cancelled->request_cancel();
    const auto expired = std::make_shared<mint::TaskControl>(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    for (const auto& control : {cancelled, expired}) {
        mint::ToolRegistry tools(temporary.path(),
                                 mint::ToolRegistryOptions{.task_control = control});
        for (const mint::ToolCall& call :
             {mint::ToolCall{"list", "list_files", {{"path", "."}}},
              mint::ToolCall{"read", "read_file", {{"path", "text.txt"}}},
              mint::ToolCall{"search", "search_text", {{"query", "needle"}}}}) {
            SCOPED_TRACE(call.name);
            const auto result = mint::Json::parse(tools.execute(call));
            EXPECT_FALSE(result.at("ok"));
            EXPECT_TRUE(result.contains("error"));
        }
    }
}

TEST(PathIdentityTest, DirectoryEntriesRemainDistinctFromLinkedObjects) {
    TemporaryDirectory temporary;
    const auto original = temporary.path() / "original.txt";
    const auto linked = temporary.path() / "linked.txt";
    write_text(original, "source\n");
    std::filesystem::create_hard_link(original, linked);
    EXPECT_TRUE(mint::tools::detail::same_path_identity(original, linked));
    EXPECT_FALSE(mint::tools::detail::same_path_entry_identity(original, linked));
    EXPECT_FALSE(mint::tools::detail::is_entry_within(original, linked));
}

TEST(WorkspaceScanTest, HardLinksCannotExposeSensitiveFilesThroughOrdinaryNames) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "files";
    write_text(workspace / ".env", "needle private\n");
    write_text(temporary.path() / "outside.txt", "needle outside\n");
    std::filesystem::create_hard_link(workspace / ".env", workspace / "notes.txt");
    std::filesystem::create_hard_link(temporary.path() / "outside.txt", workspace / "copy.txt");
    mint::ToolRegistry tools(workspace, mint::ToolRegistryOptions{.allow_write = true});
    for (const auto* path : {"notes.txt", "copy.txt"}) {
        SCOPED_TRACE(path);
        const auto read = mint::Json::parse(tools.execute({"read", "read_file", {{"path", path}}}));
        EXPECT_FALSE(read.at("ok"));
        const auto patch = mint::Json::parse(tools.execute({"patch",
                                                            "apply_patch",
                                                            {{"path", path},
                                                             {"operation", "replace"},
                                                             {"old_text", "needle"},
                                                             {"new_text", "changed"}}}));
        EXPECT_FALSE(patch.at("ok"));
    }
    const auto listed = mint::Json::parse(tools.execute({"list", "list_files", {{"path", "."}}}));
    ASSERT_TRUE(listed.at("ok"));
    EXPECT_TRUE(listed.at("entries").empty());
    const auto searched =
        mint::Json::parse(tools.execute({"search", "search_text", {{"query", "needle"}}}));
    ASSERT_TRUE(searched.at("ok"));
    EXPECT_TRUE(searched.at("hits").empty());
    EXPECT_EQ(mint::test::read_text(workspace / ".env"), "needle private\n");
    EXPECT_EQ(mint::test::read_text(temporary.path() / "outside.txt"), "needle outside\n");
}

TEST(PathIdentityTest, ContainmentResolvesParentsButPreservesFinalSymlink) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto outside = temporary.path() / "outside";
    write_text(workspace / "text.txt", "workspace\n");
    write_text(outside / "text.txt", "outside\n");
    std::error_code error;
    std::filesystem::create_directory_symlink(outside, workspace / "link", error);
    if (error) {
        GTEST_SKIP() << "Directory symlinks are unavailable: " << error.message();
    }
    EXPECT_TRUE(mint::tools::detail::is_entry_within(workspace, workspace / "link"));
    EXPECT_FALSE(mint::tools::detail::is_entry_within(workspace, workspace / "link" / "text.txt"));
    EXPECT_FALSE(mint::tools::detail::same_path_entry_identity(workspace / "link", outside));
    EXPECT_TRUE(mint::tools::detail::same_path_identity(workspace / "link", outside));
    EXPECT_TRUE(mint::tools::detail::is_entry_within(workspace, workspace / "new" / "file.txt"));
    EXPECT_FALSE(mint::tools::detail::is_entry_within(workspace, temporary.path() / "workspace2"));
}

} // namespace
