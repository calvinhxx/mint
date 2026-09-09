#include "agent/agent_command_internal.hpp"

#include "test_workspace.hpp"

#include <gtest/gtest.h>

namespace {

using mint::test::TemporaryDirectory;
using mint::test::write_text;

TEST(AgentConfigPathsTest, SharedConfigProtectsFilesAndStagingWithoutBlockingTheirParent) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "public project notes");
    write_text(workspace / "model.json", "private model settings");
    write_text(workspace / "credentials.json", "synthetic private credential");
    write_text(workspace / "model.lock", "private lock");
    write_text(workspace / ".model-staging" / "pending.tmp", "synthetic pending credential");
    mint::cli::CommandLine command;
    command.state_dir = workspace;
    mint::ToolRegistry tools(
        workspace,
        mint::ToolRegistryOptions{.protected_paths = mint::cli::command_detail::protected_paths(
                                      command, std::nullopt, {}, std::nullopt, std::nullopt)});

    const auto listed = mint::Json::parse(tools.execute({"list", "list_files", {{"path", "."}}}));
    ASSERT_TRUE(listed.at("ok").get<bool>());
    EXPECT_TRUE(mint::test::has_entry(listed.at("entries"), "README.md"));
    for (const auto* path : {"model.json", "credentials.json", "model.lock", ".model-staging"}) {
        EXPECT_FALSE(mint::test::has_entry(listed.at("entries"), path));
    }
    for (const auto* path :
         {"model.json", "credentials.json", "model.lock", ".model-staging/pending.tmp"}) {
        const auto result =
            mint::Json::parse(tools.execute({"read", "read_file", {{"path", path}}}));
        EXPECT_FALSE(result.at("ok").get<bool>()) << path;
        EXPECT_EQ(result.dump().find("synthetic"), std::string::npos);
    }
    const auto search = mint::Json::parse(
        tools.execute({"search", "search_text", {{"path", "."}, {"query", "synthetic"}}}));
    ASSERT_TRUE(search.at("ok").get<bool>());
    EXPECT_TRUE(search.at("hits").empty());
    const auto read =
        mint::Json::parse(tools.execute({"read", "read_file", {{"path", "README.md"}}}));
    EXPECT_TRUE(read.at("ok").get<bool>());
}

TEST(AgentConfigPathsTest, ExplicitOverrideRemainsProtected) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "override.json", "synthetic override secret");
    write_text(workspace / "README.md", "public project notes");
    mint::cli::CommandLine command;
    command.state_dir = temporary.path() / "state";
    command.config_specified = true;
    command.config = workspace / "override.json";
    mint::ToolRegistry tools(
        workspace,
        mint::ToolRegistryOptions{.protected_paths = mint::cli::command_detail::protected_paths(
                                      command, std::nullopt, {}, std::nullopt, std::nullopt)});
    const auto result =
        mint::Json::parse(tools.execute({"read", "read_file", {{"path", "override.json"}}}));
    EXPECT_FALSE(result.at("ok").get<bool>());
    const auto listed = mint::Json::parse(tools.execute({"list", "list_files", {{"path", "."}}}));
    EXPECT_TRUE(listed.at("ok").get<bool>());
    EXPECT_TRUE(mint::test::has_entry(listed.at("entries"), "README.md"));
}

} // namespace
