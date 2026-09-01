#include "mint/runtime/task_control.hpp"
#include "mint/tools/tool_registry.hpp"
#include "mint/version.hpp"

#include "command_helper.hpp"
#include "test_executable.hpp"
#include "test_workspace.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

#define MINT_EXPECT(condition, message) EXPECT_TRUE(condition) << (message)

using mint::test::expect_failure;
using mint::test::has_entry;
using mint::test::read_text;
using mint::test::TemporaryDirectory;
using mint::test::write_text;

std::filesystem::path test_executable;

void expect_contract_rejection(mint::ToolRegistry& tools, const mint::ToolCall& call,
                               std::string_view context) {
    const auto result = mint::Json::parse(tools.execute(call));
    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result.size(), 2U);
    EXPECT_EQ(result.at("ok"), false);
    ASSERT_TRUE(result.at("error").is_string());
    EXPECT_NE(result.at("error").get_ref<const std::string&>().find(context), std::string::npos);
}

TEST(ToolRegistryTest, ReadOnlyTools) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    std::filesystem::create_directories(workspace / ".git");
    write_text(workspace / "README.md", "# Demo Agent\nA tiny project.\n");
    write_text(workspace / "src" / "main.cpp", "int main() { return 0; }\n");
    write_text(workspace / "large.txt", std::string(24 * 1024, 'x'));
    write_text(workspace / "config.json", R"({"api_key":"must-not-leak"})");
    write_text(workspace / ".git" / "config", "token=must-not-leak\n");
    write_text(temporary.path() / "outside.txt", "secret\n");

    mint::ToolRegistry tools(
        workspace, mint::ToolRegistryOptions{.protected_paths = {workspace / "config.json"}});

    const auto listed =
        mint::Json::parse(tools.execute({"list", "list_files", {{"path", "."}, {"max_depth", 2}}}));
    MINT_EXPECT(listed.at("ok").get<bool>(), "list_files succeeds");
    MINT_EXPECT(has_entry(listed.at("entries"), "README.md"), "list_files sees README.md");
    MINT_EXPECT(has_entry(listed.at("entries"), "src/main.cpp"), "list_files respects depth");
    MINT_EXPECT(!has_entry(listed.at("entries"), "config.json"),
                "list_files hides the protected config file");
    MINT_EXPECT(!has_entry(listed.at("entries"), ".git"),
                "list_files hides ignored metadata directories");

    const auto direct_git_list =
        mint::Json::parse(tools.execute({"git-list", "list_files", {{"path", ".git"}}}));
    MINT_EXPECT(!direct_git_list.at("ok").get<bool>(),
                "list_files rejects a directly requested ignored directory");

    const auto read =
        mint::Json::parse(tools.execute({"read", "read_file", {{"path", "README.md"}}}));
    MINT_EXPECT(read.at("ok").get<bool>(), "read_file succeeds");
    MINT_EXPECT(read.at("content").get<std::string>().find("Demo Agent") != std::string::npos,
                "read_file returns text");

    const auto first_chunk =
        mint::Json::parse(tools.execute({"large-first", "read_file", {{"path", "large.txt"}}}));
    MINT_EXPECT(first_chunk.at("ok").get<bool>() &&
                    first_chunk.at("content").get<std::string>().size() == 16 * 1024 &&
                    first_chunk.at("truncated").get<bool>(),
                "read_file defaults to a bounded 16 KiB chunk");
    const auto second_chunk =
        mint::Json::parse(tools.execute({"large-second",
                                         "read_file",
                                         {{"path", "large.txt"},
                                          {"offset", first_chunk.at("next_offset")},
                                          {"max_bytes", 16 * 1024}}}));
    MINT_EXPECT(second_chunk.at("ok").get<bool>() && !second_chunk.at("truncated").get<bool>() &&
                    second_chunk.at("content").get<std::string>().size() == 8 * 1024,
                "read_file continues from next_offset without resending the first chunk");

    const auto protected_config =
        mint::Json::parse(tools.execute({"config", "read_file", {{"path", "config.json"}}}));
    MINT_EXPECT(!protected_config.at("ok").get<bool>(), "read_file rejects protected config");

    const auto git_config =
        mint::Json::parse(tools.execute({"git-config", "read_file", {{"path", ".git/config"}}}));
    MINT_EXPECT(!git_config.at("ok").get<bool>(),
                "read_file rejects a direct path inside ignored metadata");

    const auto searched = mint::Json::parse(tools.execute(
        {"search", "search_text", {{"path", "."}, {"query", "agent"}, {"case_sensitive", false}}}));
    MINT_EXPECT(searched.at("ok").get<bool>(), "search_text succeeds");
    MINT_EXPECT(searched.at("hits").size() == 1, "search_text finds one case-insensitive hit");
    MINT_EXPECT(searched.at("hits").at(0).at("line") == 1, "search_text reports line number");

    const auto secret_search = mint::Json::parse(
        tools.execute({"secret-search",
                       "search_text",
                       {{"path", "."}, {"query", "must-not-leak"}, {"case_sensitive", true}}}));
    MINT_EXPECT(secret_search.at("hits").empty(), "search_text skips protected config");

    const auto git_search = mint::Json::parse(tools.execute(
        {"git-search", "search_text", {{"path", ".git"}, {"query", "must-not-leak"}}}));
    MINT_EXPECT(!git_search.at("ok").get<bool>(),
                "search_text rejects a direct ignored metadata path");

    const auto escaped =
        mint::Json::parse(tools.execute({"escape", "read_file", {{"path", "../outside.txt"}}}));
    MINT_EXPECT(!escaped.at("ok").get<bool>(), "path traversal is rejected");

    std::error_code symlink_error;
    std::filesystem::create_symlink(temporary.path() / "outside.txt", workspace / "outside-link",
                                    symlink_error);
    if (!symlink_error) {
        const auto followed_symlink =
            mint::Json::parse(tools.execute({"symlink", "read_file", {{"path", "outside-link"}}}));
        MINT_EXPECT(!followed_symlink.at("ok").get<bool>(), "escaping symlink is rejected");
    }
}

TEST(ToolRegistryTest, RejectsArgumentsOutsidePublishedContracts) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "contract\n");

    mint::ToolRegistry tools(workspace, mint::ToolRegistryOptions{.allow_write = true});
    expect_contract_rejection(tools,
                              {"list-extra", "list_files", {{"path", "."}, {"unexpected", true}}},
                              "list_files 包含未知字段: unexpected");
    expect_contract_rejection(
        tools, {"read-extra", "read_file", {{"path", "README.md"}, {"unexpected", true}}},
        "read_file 包含未知字段: unexpected");
    expect_contract_rejection(
        tools, {"search-extra", "search_text", {{"query", "contract"}, {"unexpected", true}}},
        "search_text 包含未知字段: unexpected");
    expect_contract_rejection(
        tools,
        {"patch-extra",
         "apply_patch",
         {{"path", "new.txt"}, {"operation", "create"}, {"new_text", "new\n"}, {"old_text", ""}}},
        "apply_patch create 包含未知字段: old_text");
    expect_contract_rejection(
        tools,
        {"changeset-extra",
         "apply_changeset",
         {{"changes", mint::Json::array(
                          {{{"operation", "create"}, {"path", "new.txt"}, {"new_text", "new\n"}}})},
          {"unexpected", true}}},
        "apply_changeset 包含未知字段: unexpected");
    expect_contract_rejection(tools, {"changes-extra", "workspace_changes", {{"unexpected", true}}},
                              "workspace_changes 包含未知字段: unexpected");

    EXPECT_FALSE(std::filesystem::exists(workspace / "new.txt"));
}

TEST(ToolRegistryTest, ConfigurableRuntimeLimits) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "large.txt", std::string(4096, 'x'));
    write_text(workspace / "one.txt", "needle one\nneedle two\n");
    write_text(workspace / "two.txt", "needle three\n");

    mint::ToolRuntimeSettings limits;
    limits.read_file_bytes = 1024;
    limits.list_max_entries = 2;
    limits.search_file_bytes = 4096;
    limits.search_max_hits = 1;
    limits.search_max_files = 2;
    limits.command_output_bytes = 512;
    mint::ToolRegistry tools(workspace, mint::ToolRegistryOptions{.runtime = limits});

    const auto read =
        mint::Json::parse(tools.execute({"read", "read_file", {{"path", "large.txt"}}}));
    MINT_EXPECT(read.at("content").get<std::string>().size() == limits.read_file_bytes &&
                    read.at("truncated").get<bool>(),
                "policy-configured read_file chunk size is applied");

    const auto listed =
        mint::Json::parse(tools.execute({"list", "list_files", {{"path", "."}, {"max_depth", 1}}}));
    MINT_EXPECT(listed.at("entries").size() == limits.list_max_entries &&
                    listed.at("truncated").get<bool>(),
                "policy-configured list entry budget is applied");

    const auto searched = mint::Json::parse(
        tools.execute({"search", "search_text", {{"path", "."}, {"query", "NEEDLE"}}}));
    MINT_EXPECT(searched.at("hits").size() == limits.search_max_hits &&
                    searched.at("truncated").get<bool>(),
                "policy-configured search budget is applied with allocation-free ASCII matching");

    std::filesystem::create_directories(workspace / "oversized");
    write_text(workspace / "oversized" / "one.txt", std::string(2048, 'a'));
    write_text(workspace / "oversized" / "two.txt", std::string(2048, 'b'));
    write_text(workspace / "oversized" / "three.txt", std::string(2048, 'c'));
    auto scan_limits = limits;
    scan_limits.search_file_bytes = 1024;
    scan_limits.search_max_hits = 10;
    mint::ToolRegistry bounded_search(workspace, mint::ToolRegistryOptions{.runtime = scan_limits});
    const auto bounded = mint::Json::parse(bounded_search.execute(
        {"bounded-search", "search_text", {{"path", "oversized"}, {"query", "missing"}}}));
    MINT_EXPECT(bounded.at("scanned_files") == scan_limits.search_max_files &&
                    bounded.at("truncated").get<bool>(),
                "search file budget also caps oversized candidates before opening them");

    const auto definitions = tools.definitions().dump();
    MINT_EXPECT(definitions.find("Defaults to 1024") != std::string::npos,
                "tool schema reports the configured read chunk default");

    auto invalid = limits;
    invalid.search_max_hits = 0;
    expect_failure(
        [&] { (void)mint::ToolRegistry(workspace, mint::ToolRegistryOptions{.runtime = invalid}); },
        "programmatic tool limits are validated before use");
}

TEST(ToolRegistryTest, ApplyPatch) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    std::filesystem::create_directories(workspace / ".git");
    write_text(workspace / "README.md", "alpha\nbeta\n");
    write_text(workspace / "duplicate.txt", "repeat\nrepeat\n");
    std::string invalid_utf8 = "bad";
    invalid_utf8.push_back(static_cast<char>(0xC3));
    invalid_utf8.push_back('(');
    write_text(workspace / "invalid.txt", invalid_utf8);
    write_text(workspace / "config.json", R"({"api_key":"must-stay-secret"})");
    write_text(workspace / ".git" / "config", "token=must-stay-secret\n");
    write_text(temporary.path() / "outside.txt", "outside\n");

    const mint::ToolCall replace_readme{"patch-readme",
                                        "apply_patch",
                                        {{"path", "README.md"},
                                         {"operation", "replace"},
                                         {"old_text", "beta\n"},
                                         {"new_text", "gamma\n"}}};

    mint::ToolRegistry read_only_tools(workspace);
    MINT_EXPECT(read_only_tools.definitions().size() == 3,
                "write tool is hidden unless explicitly enabled");
    const auto disabled = mint::Json::parse(read_only_tools.execute(replace_readme));
    MINT_EXPECT(!disabled.at("ok").get<bool>(), "apply_patch rejects missing write authorization");
    MINT_EXPECT(read_text(workspace / "README.md") == "alpha\nbeta\n",
                "disabled apply_patch leaves the file unchanged");
    const auto disabled_changes = mint::Json::parse(
        read_only_tools.execute({"disabled-changes", "workspace_changes", mint::Json::object()}));
    MINT_EXPECT(!disabled_changes.at("ok").get<bool>(),
                "workspace_changes is unavailable without write authorization");

    mint::ToolRegistry tools(
        workspace, mint::ToolRegistryOptions{.protected_paths = {workspace / "config.json"},
                                             .allow_write = true});
    MINT_EXPECT(tools.definitions().size() == 6,
                "write-enabled registry exposes patch, changeset and workspace changes");

    const auto replaced = mint::Json::parse(tools.execute(replace_readme));
    MINT_EXPECT(replaced.at("ok").get<bool>(), "apply_patch replaces one exact block");
    MINT_EXPECT(read_text(workspace / "README.md") == "alpha\ngamma\n",
                "replace writes the expected contents");

    const auto created =
        mint::Json::parse(tools.execute({"patch-create",
                                         "apply_patch",
                                         {{"path", "src/generated.cpp"},
                                          {"operation", "create"},
                                          {"new_text", "int generated() { return 42; }\n"}}}));
    MINT_EXPECT(created.at("ok").get<bool>(), "apply_patch creates a new text file");
    MINT_EXPECT(read_text(workspace / "src" / "generated.cpp") ==
                    "int generated() { return 42; }\n",
                "create writes the expected contents");

    const auto changes =
        mint::Json::parse(tools.execute({"changes", "workspace_changes", mint::Json::object()}));
    MINT_EXPECT(changes.at("ok").get<bool>(), "workspace_changes succeeds");
    MINT_EXPECT(changes.at("changed_files").size() == 2,
                "workspace_changes reports modified and created files");
    const auto diff = changes.at("diff").get<std::string>();
    MINT_EXPECT(diff.find("--- a/README.md") != std::string::npos,
                "workspace_changes emits a modified-file header");
    MINT_EXPECT(diff.find("-beta\n+gamma\n") != std::string::npos,
                "workspace_changes emits the exact text replacement");
    MINT_EXPECT(diff.find("--- /dev/null\n+++ b/src/generated.cpp") != std::string::npos,
                "workspace_changes emits a created-file header");
    MINT_EXPECT(!changes.at("diff_truncated").get<bool>(), "small workspace diff is not truncated");

    const auto overwrite = mint::Json::parse(tools.execute(
        {"patch-overwrite",
         "apply_patch",
         {{"path", "README.md"}, {"operation", "create"}, {"new_text", "overwritten\n"}}}));
    MINT_EXPECT(!overwrite.at("ok").get<bool>(), "create never overwrites an existing file");

    const auto ambiguous = mint::Json::parse(tools.execute({"patch-ambiguous",
                                                            "apply_patch",
                                                            {{"path", "duplicate.txt"},
                                                             {"operation", "replace"},
                                                             {"old_text", "repeat"},
                                                             {"new_text", "changed"}}}));
    MINT_EXPECT(!ambiguous.at("ok").get<bool>(), "replace rejects an ambiguous old_text");
    MINT_EXPECT(read_text(workspace / "duplicate.txt") == "repeat\nrepeat\n",
                "ambiguous replacement leaves the file unchanged");

    const auto stale = mint::Json::parse(tools.execute({"patch-stale",
                                                        "apply_patch",
                                                        {{"path", "README.md"},
                                                         {"operation", "replace"},
                                                         {"old_text", "not present"},
                                                         {"new_text", "changed"}}}));
    MINT_EXPECT(!stale.at("ok").get<bool>(), "replace detects stale file context");

    const auto invalid_encoding = mint::Json::parse(tools.execute({"patch-invalid-utf8",
                                                                   "apply_patch",
                                                                   {{"path", "invalid.txt"},
                                                                    {"operation", "replace"},
                                                                    {"old_text", "bad"},
                                                                    {"new_text", "good"}}}));
    MINT_EXPECT(!invalid_encoding.at("ok").get<bool>(),
                "apply_patch rejects a non-UTF-8 source before writing");
    MINT_EXPECT(read_text(workspace / "invalid.txt") == invalid_utf8,
                "encoding rejection leaves the original bytes unchanged");

    const auto protected_config =
        mint::Json::parse(tools.execute({"patch-config",
                                         "apply_patch",
                                         {{"path", "config.json"},
                                          {"operation", "replace"},
                                          {"old_text", "must-stay-secret"},
                                          {"new_text", "leaked"}}}));
    MINT_EXPECT(!protected_config.at("ok").get<bool>(),
                "apply_patch rejects the protected config file");
    MINT_EXPECT(read_text(workspace / "config.json").find("must-stay-secret") != std::string::npos,
                "protected config remains unchanged");

    const auto git_config = mint::Json::parse(tools.execute({"patch-git-config",
                                                             "apply_patch",
                                                             {{"path", ".git/config"},
                                                              {"operation", "replace"},
                                                              {"old_text", "must-stay-secret"},
                                                              {"new_text", "leaked"}}}));
    MINT_EXPECT(!git_config.at("ok").get<bool>(),
                "apply_patch rejects ignored repository metadata");
    MINT_EXPECT(read_text(workspace / ".git" / "config").find("must-stay-secret") !=
                    std::string::npos,
                "ignored repository metadata remains unchanged");

    const auto escaped = mint::Json::parse(tools.execute({"patch-escape",
                                                          "apply_patch",
                                                          {{"path", "../outside.txt"},
                                                           {"operation", "replace"},
                                                           {"old_text", "outside"},
                                                           {"new_text", "escaped"}}}));
    MINT_EXPECT(!escaped.at("ok").get<bool>(), "apply_patch rejects path traversal");
    MINT_EXPECT(read_text(temporary.path() / "outside.txt") == "outside\n",
                "path traversal leaves outside files unchanged");

    const auto unsupported = mint::Json::parse(
        tools.execute({"patch-delete",
                       "apply_patch",
                       {{"path", "README.md"}, {"operation", "delete"}, {"new_text", ""}}}));
    MINT_EXPECT(!unsupported.at("ok").get<bool>(), "v0.2 does not allow file deletion");

    std::error_code symlink_error;
    std::filesystem::create_symlink(temporary.path() / "outside.txt", workspace / "write-link",
                                    symlink_error);
    if (!symlink_error) {
        const auto symlink = mint::Json::parse(tools.execute({"patch-symlink",
                                                              "apply_patch",
                                                              {{"path", "write-link"},
                                                               {"operation", "replace"},
                                                               {"old_text", "outside"},
                                                               {"new_text", "escaped"}}}));
        MINT_EXPECT(!symlink.at("ok").get<bool>(), "apply_patch rejects symbolic links");
        MINT_EXPECT(read_text(temporary.path() / "outside.txt") == "outside\n",
                    "symbolic link rejection leaves outside files unchanged");
    }

    const auto journal_state = tools.workspace_change_state();
    mint::ToolRegistry restored_tools(workspace, mint::ToolRegistryOptions{.allow_write = true});
    restored_tools.restore_workspace_change_state(journal_state);
    MINT_EXPECT(restored_tools.workspace_change_snapshot().at("changed_files").size() == 2,
                "change journal restores both stable file entries");

    write_text(workspace / "README.md", "externally changed\n");
    bool rejected_stale_session = false;
    try {
        mint::ToolRegistry stale_tools(workspace, mint::ToolRegistryOptions{.allow_write = true});
        stale_tools.restore_workspace_change_state(journal_state);
    } catch (const std::invalid_argument& error) {
        rejected_stale_session = std::string(error.what()).find("外部修改") != std::string::npos;
    }
    MINT_EXPECT(rejected_stale_session,
                "change journal restore rejects files modified after the checkpoint");
}

TEST(ToolRegistryTest, ApplyChangeSet) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    std::filesystem::create_directories(workspace / "src");
    std::filesystem::create_directories(workspace / ".git");
    write_text(workspace / "src" / "alpha.txt", "alpha\n");
    write_text(workspace / "src" / "delete.txt", "delete me\n");
    write_text(workspace / "src" / "move.txt", "move me\n");
    write_text(workspace / ".git" / "config", "protected\n");

    bool approval_seen = false;
    mint::ToolRegistry tools(
        workspace, mint::ToolRegistryOptions{
                       .allow_write = true,
                       .allowed_write_paths = {"src"},
                       .change_set_approval = [&](const mint::ChangeSetApprovalRequest& request) {
                           approval_seen =
                               request.paths.size() == 5 &&
                               request.unified_diff.find("src/alpha.txt") != std::string::npos &&
                               request.unified_diff.find("src/moved.txt") != std::string::npos;
                           return true;
                       }});

    const auto rejected_extra_field = mint::Json::parse(tools.execute(
        {"changeset-extra-field",
         "apply_changeset",
         {{"changes", mint::Json::array({{{"operation", "create"},
                                          {"path", "src/extra.txt"},
                                          {"new_text", "new\n"},
                                          {"old_text", "not valid for create"}}})}}}));
    MINT_EXPECT(!rejected_extra_field.at("ok").get<bool>() &&
                    !std::filesystem::exists(workspace / "src" / "extra.txt"),
                "changeset rejects operation fields outside the exact operation contract");

    const auto committed = mint::Json::parse(tools.execute(
        {"changeset-commit",
         "apply_changeset",
         {{"changes",
           mint::Json::array(
               {{{"operation", "replace"},
                 {"path", "src/alpha.txt"},
                 {"old_text", "alpha\n"},
                 {"new_text", "beta\n"}},
                {{"operation", "create"}, {"path", "src/new.txt"}, {"new_text", "new\n"}},
                {{"operation", "delete"}, {"path", "src/delete.txt"}, {"old_text", "delete me\n"}},
                {{"operation", "move"},
                 {"path", "src/move.txt"},
                 {"destination", "src/moved.txt"},
                 {"old_text", "move me\n"}}})}}}));
    MINT_EXPECT(committed.at("ok").get<bool>() && committed.at("status") == "committed" &&
                    committed.at("operation_count") == 4,
                "apply_changeset commits four validated operations together");
    MINT_EXPECT(approval_seen, "changeset approval receives a bounded five-file diff preview");
    MINT_EXPECT(read_text(workspace / "src" / "alpha.txt") == "beta\n" &&
                    read_text(workspace / "src" / "new.txt") == "new\n",
                "changeset replace and create write exact content");
    MINT_EXPECT(!std::filesystem::exists(workspace / "src" / "delete.txt") &&
                    !std::filesystem::exists(workspace / "src" / "move.txt") &&
                    read_text(workspace / "src" / "moved.txt") == "move me\n",
                "changeset delete and move produce exact final paths");

    const auto snapshot = tools.workspace_change_snapshot();
    MINT_EXPECT(snapshot.at("changed_files").size() == 5,
                "change journal represents a move as one deletion and one creation");
    MINT_EXPECT(snapshot.at("diff").get<std::string>().find(
                    "--- a/src/delete.txt\n+++ /dev/null") != std::string::npos,
                "change journal emits deleted-file unified diff headers");
    MINT_EXPECT(tools.workspace_change_state().at("schema_version") ==
                    mint::workspace_change_schema_version,
                "workspace change state uses the integrity-aware schema");

    mint::ToolRegistry restored(
        workspace, mint::ToolRegistryOptions{.allow_write = true, .allowed_write_paths = {"src"}});
    restored.restore_workspace_change_state(tools.workspace_change_state());
    MINT_EXPECT(restored.workspace_change_snapshot().at("changed_files").size() == 5,
                "change journal restores created, modified and deleted paths");

    const auto before_failed_validation = read_text(workspace / "src" / "alpha.txt");
    const auto rejected = mint::Json::parse(
        tools.execute({"changeset-prevalidation",
                       "apply_changeset",
                       {{"changes", mint::Json::array({{{"operation", "replace"},
                                                        {"path", "src/alpha.txt"},
                                                        {"old_text", "beta\n"},
                                                        {"new_text", "gamma\n"}},
                                                       {{"operation", "delete"},
                                                        {"path", "src/moved.txt"},
                                                        {"old_text", "stale\n"}}})}}}));
    MINT_EXPECT(!rejected.at("ok").get<bool>() &&
                    read_text(workspace / "src" / "alpha.txt") == before_failed_validation,
                "changeset validates every operation before writing the first file");

    bool denied_seen = false;
    mint::ToolRegistry denied_tools(
        workspace, mint::ToolRegistryOptions{
                       .allow_write = true,
                       .allowed_write_paths = {"src"},
                       .change_set_approval = [&](const mint::ChangeSetApprovalRequest&) {
                           denied_seen = true;
                           return false;
                       }});
    const auto denied = mint::Json::parse(
        denied_tools.execute({"changeset-denied",
                              "apply_changeset",
                              {{"changes", mint::Json::array({{{"operation", "create"},
                                                               {"path", "src/denied.txt"},
                                                               {"new_text", "denied\n"}}})}}}));
    MINT_EXPECT(denied_seen && denied.at("status") == "denied" &&
                    !std::filesystem::exists(workspace / "src" / "denied.txt"),
                "changeset approval denial performs no writes");

    auto cancelled_control = std::make_shared<mint::TaskControl>();
    mint::ToolRegistry cancelled_tools(
        workspace,
        mint::ToolRegistryOptions{.allow_write = true,
                                  .allowed_write_paths = {"src"},
                                  .task_control = cancelled_control,
                                  .change_set_approval = [](const mint::ChangeSetApprovalRequest&) {
                                      return mint::ApprovalDecisionKind::run_cancelled;
                                  }});
    const auto cancelled = mint::Json::parse(cancelled_tools.execute(
        {"changeset-cancelled",
         "apply_changeset",
         {{"changes", mint::Json::array({{{"operation", "create"},
                                          {"path", "src/cancelled.txt"},
                                          {"new_text", "cancelled\n"}}})}}}));
    MINT_EXPECT(cancelled.at("status") == "cancelled" && cancelled.at("cancelled").get<bool>() &&
                    cancelled.at("approval_decision_source") == "run_cancelled" &&
                    cancelled.at("changed_files").empty() &&
                    !std::filesystem::exists(workspace / "src" / "cancelled.txt") &&
                    cancelled_tools.workspace_change_snapshot().at("changed_files").empty() &&
                    cancelled_control->cancellation_requested() &&
                    cancelled.at("error").get<std::string>().find("用户拒绝") == std::string::npos,
                "cancelled changeset approval writes nothing and is not a user denial");

    mint::ToolRegistry unresolved_tools(
        workspace,
        mint::ToolRegistryOptions{.allow_write = true,
                                  .allowed_write_paths = {"src"},
                                  .change_set_approval = [](const mint::ChangeSetApprovalRequest&) {
                                      return mint::ApprovalDecisionKind::invalid_response;
                                  }});
    const auto unresolved = mint::Json::parse(unresolved_tools.execute(
        {"changeset-unresolved",
         "apply_changeset",
         {{"changes", mint::Json::array({{{"operation", "create"},
                                          {"path", "src/unresolved.txt"},
                                          {"new_text", "unresolved\n"}}})}}}));
    MINT_EXPECT(unresolved.at("status") == "failed" &&
                    unresolved.at("approval_decision_source") == "invalid_response" &&
                    unresolved.at("changed_files").empty() &&
                    !std::filesystem::exists(workspace / "src" / "unresolved.txt") &&
                    unresolved_tools.workspace_change_snapshot().at("changed_files").empty(),
                "invalid changeset approval is a protocol failure with zero writes");

    const auto ignored = mint::Json::parse(
        tools.execute({"changeset-ignored",
                       "apply_changeset",
                       {{"changes", mint::Json::array({{{"operation", "replace"},
                                                        {"path", ".git/config"},
                                                        {"old_text", "protected\n"},
                                                        {"new_text", "changed\n"}}})}}}));
    MINT_EXPECT(!ignored.at("ok").get<bool>() &&
                    read_text(workspace / ".git" / "config") == "protected\n",
                "changeset cannot write ignored repository metadata");

#if !defined(_WIN32)
    const auto locked = workspace / "zlocked";
    std::filesystem::create_directories(locked);
    std::filesystem::permissions(
        locked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);
    mint::ToolRegistry rollback_tools(
        workspace,
        mint::ToolRegistryOptions{.allow_write = true, .allowed_write_paths = {"src", "zlocked"}});
    const auto rolled_back = mint::Json::parse(rollback_tools.execute(
        {"changeset-rollback",
         "apply_changeset",
         {{"changes", mint::Json::array({{{"operation", "replace"},
                                          {"path", "src/alpha.txt"},
                                          {"old_text", "beta\n"},
                                          {"new_text", "gamma\n"}},
                                         {{"operation", "create"},
                                          {"path", "zlocked/new.txt"},
                                          {"new_text", "cannot write\n"}}})}}}));
    std::filesystem::permissions(locked, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    MINT_EXPECT(!rolled_back.at("ok").get<bool>() &&
                    read_text(workspace / "src" / "alpha.txt") == "beta\n" &&
                    !std::filesystem::exists(locked / "new.txt"),
                "a mid-commit filesystem failure restores every earlier file");
#endif
}

TEST(ToolRegistryTest, EnforcesWritePathAllowlist) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "keep\n");

    mint::ToolRegistry tools(
        workspace, mint::ToolRegistryOptions{.allow_write = true,
                                             .allowed_write_paths = {"src", "FIX_REPORT.md"}});
    MINT_EXPECT(tools.allowed_write_paths() == std::vector<std::string>({"src", "FIX_REPORT.md"}),
                "write allowlist exposes stable relative policy labels");

    const auto denied = mint::Json::parse(tools.execute({"scope-denied",
                                                         "apply_patch",
                                                         {{"path", "README.md"},
                                                          {"operation", "replace"},
                                                          {"old_text", "keep\n"},
                                                          {"new_text", "changed\n"}}}));
    MINT_EXPECT(!denied.at("ok").get<bool>() && read_text(workspace / "README.md") == "keep\n",
                "write allowlist rejects an otherwise valid edit outside its scope");

    const auto allowed_file = mint::Json::parse(tools.execute(
        {"scope-report",
         "apply_patch",
         {{"path", "FIX_REPORT.md"}, {"operation", "create"}, {"new_text", "verified\n"}}}));
    MINT_EXPECT(allowed_file.at("ok").get<bool>(),
                "write allowlist permits an exact not-yet-created file");

    const auto allowed_directory =
        mint::Json::parse(tools.execute({"scope-source",
                                         "apply_patch",
                                         {{"path", "src/generated.cpp"},
                                          {"operation", "create"},
                                          {"new_text", "int generated = 1;\n"}}}));
    MINT_EXPECT(allowed_directory.at("ok").get<bool>(),
                "write allowlist permits descendants of an authorized existing directory");

    bool rejected_escape = false;
    try {
        mint::ToolRegistry invalid(
            workspace, mint::ToolRegistryOptions{.allow_write = true,
                                                 .allowed_write_paths = {"../outside.txt"}});
    } catch (const std::invalid_argument&) {
        rejected_escape = true;
    }
    MINT_EXPECT(rejected_escape, "write allowlist rejects paths outside the workspace");
}

} // namespace

const std::filesystem::path& mint_test_executable_path() {
    return test_executable;
}

int run_change_transaction_lock_helper(int argc, char** argv);

#undef MINT_EXPECT

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--transaction-lock-helper") {
        return run_change_transaction_lock_helper(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--command-helper") {
        return mint::test::run_command_helper(argc, argv);
    }

    std::error_code executable_error;
    test_executable = std::filesystem::weakly_canonical(argv[0], executable_error);
    if (executable_error || test_executable.empty()) {
        std::cerr << "could not resolve test executable path\n";
        return 1;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
