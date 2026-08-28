#include "mint/application/agent.hpp"
#include "mint/infrastructure/change_transaction_store.hpp"
#include "mint/infrastructure/event_log.hpp"
#include "mint/infrastructure/session_store.hpp"
#include "mint/runtime/task_control.hpp"
#include "mint/tools/tool_registry.hpp"
#include "mint/version.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

const std::filesystem::path& mint_test_executable_path();

namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("mint-transaction-tests-" + std::to_string(stamp) + "-" +
                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        std::filesystem::create_directories(path_ / "workspace" / "src");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

void write_text(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not write " + path.generic_string());
    }
    output << content;
    if (!output) {
        throw std::runtime_error("could not finish writing " + path.generic_string());
    }
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not read " + path.generic_string());
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

mint::Json changeset_arguments() {
    return {{"changes", mint::Json::array({{{"operation", "replace"},
                                            {"path", "src/alpha.txt"},
                                            {"old_text", "alpha before\n"},
                                            {"new_text", "alpha after\n"}},
                                           {{"operation", "replace"},
                                            {"path", "src/beta.txt"},
                                            {"old_text", "beta before\n"},
                                            {"new_text", "beta after\n"}}})}};
}

mint::ToolCall changeset_call(std::string id) {
    return {std::move(id), "apply_changeset", changeset_arguments()};
}

mint::ToolRegistryOptions transaction_options(const std::filesystem::path& journal) {
    return {
        .allow_write = true, .allowed_write_paths = {"src"}, .change_transaction_path = journal};
}

void write_originals(const std::filesystem::path& workspace) {
    write_text(workspace / "src" / "alpha.txt", "alpha before\n");
    write_text(workspace / "src" / "beta.txt", "beta before\n");
}

void expect_originals(const std::filesystem::path& workspace) {
    EXPECT_EQ(read_text(workspace / "src" / "alpha.txt"), "alpha before\n");
    EXPECT_EQ(read_text(workspace / "src" / "beta.txt"), "beta before\n");
}

void expect_desired(const std::filesystem::path& workspace) {
    EXPECT_EQ(read_text(workspace / "src" / "alpha.txt"), "alpha after\n");
    EXPECT_EQ(read_text(workspace / "src" / "beta.txt"), "beta after\n");
}

mint::Json execute_changeset(mint::ToolRegistry& tools, std::string id) {
    return mint::Json::parse(tools.execute(changeset_call(std::move(id))));
}

int run_lock_helper_process(const std::filesystem::path& workspace,
                            const std::filesystem::path& journal) {
    const auto executable = mint_test_executable_path();
#if defined(_WIN32)
    std::wstring command = L"\"" + executable.wstring() + L"\" --transaction-lock-helper \"" +
                           workspace.wstring() + L"\" \"" + journal.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return 125;
    }
    (void)::CloseHandle(process.hThread);
    if (::WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) {
        (void)::TerminateProcess(process.hProcess, 126);
        (void)::WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exit_code = 126;
    (void)::GetExitCodeProcess(process.hProcess, &exit_code);
    (void)::CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
#else
    const auto child = ::fork();
    if (child < 0) {
        return 125;
    }
    if (child == 0) {
        ::execl(executable.c_str(), executable.c_str(), "--transaction-lock-helper",
                workspace.c_str(), journal.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return 126;
    }
    return WEXITSTATUS(status);
#endif
}

mint::ModelReply changeset_reply(std::string id) {
    const auto arguments = changeset_arguments();
    const mint::Json raw_call = {
        {"id", id},
        {"type", "function"},
        {"function", {{"name", "apply_changeset"}, {"arguments", arguments.dump()}}}};
    mint::ModelReply reply;
    reply.assistant_message = {
        {"role", "assistant"}, {"content", nullptr}, {"tool_calls", mint::Json::array({raw_call})}};
    reply.tool_calls.push_back({std::move(id), "apply_changeset", arguments});
    reply.metadata = {.adapter = "test", .model = "transaction-recovery"};
    return reply;
}

class StopAfterChangesetReplyModel final : public mint::ModelClient {
  public:
    explicit StopAfterChangesetReplyModel(std::shared_ptr<mint::TaskControl> control)
        : control_(std::move(control)) {}

    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        control_->request_cancel();
        return changeset_reply("durable-changeset");
    }

  private:
    std::shared_ptr<mint::TaskControl> control_;
};

class FinishAfterChangesetModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json& messages, const mint::Json&) override {
        EXPECT_EQ(messages.back().value("role", ""), "tool");
        const auto result = mint::Json::parse(messages.back().at("content").get<std::string>());
        EXPECT_TRUE(result.value("ok", false));
        return {.assistant_message = {{"role", "assistant"}, {"content", "recovered"}},
                .text = "recovered",
                .metadata = {.adapter = "test", .model = "transaction-recovery"}};
    }
};

TEST(ChangeTransactionTest, RollsBackCompleteAndPartialWritesAfterCrash) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto journal = temporary.path() / "changeset-transaction.json";
    write_originals(workspace);

    {
        mint::ToolRegistry tools(workspace, transaction_options(journal));
        const auto result = execute_changeset(tools, "complete-write");
        ASSERT_TRUE(result.value("ok", false));
        ASSERT_TRUE(std::filesystem::exists(journal));
        expect_desired(workspace);
    }
    {
        mint::ToolRegistry recovered(workspace, transaction_options(journal));
        EXPECT_EQ(recovered.reconcile_change_transaction(std::nullopt),
                  mint::ChangeTransactionRecovery::rolled_back);
        EXPECT_FALSE(std::filesystem::exists(journal));
        expect_originals(workspace);
    }

    {
        mint::ToolRegistry tools(workspace, transaction_options(journal));
        ASSERT_TRUE(execute_changeset(tools, "partial-write").value("ok", false));
        write_text(workspace / "src" / "alpha.txt", "alpha before\n");
    }
    {
        mint::ToolRegistry recovered(workspace, transaction_options(journal));
        EXPECT_EQ(recovered.reconcile_change_transaction(std::nullopt),
                  mint::ChangeTransactionRecovery::rolled_back);
        EXPECT_FALSE(std::filesystem::exists(journal));
        expect_originals(workspace);
    }
}

TEST(ChangeTransactionTest, KeepsWritesAcknowledgedByCheckpoint) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto journal = temporary.path() / "changeset-transaction.json";
    write_originals(workspace);

    std::string transaction_id;
    mint::Json change_state;
    {
        mint::ToolRegistry tools(workspace, transaction_options(journal));
        ASSERT_TRUE(execute_changeset(tools, "checkpointed-write").value("ok", false));
        ASSERT_TRUE(tools.pending_change_transaction_id().has_value());
        transaction_id = *tools.pending_change_transaction_id();
        change_state = tools.workspace_change_state();
    }

    mint::ToolRegistry recovered(workspace, transaction_options(journal));
    EXPECT_EQ(recovered.reconcile_change_transaction(transaction_id),
              mint::ChangeTransactionRecovery::committed);
    EXPECT_FALSE(std::filesystem::exists(journal));
    expect_desired(workspace);
    EXPECT_NO_THROW(recovered.restore_workspace_change_state(change_state));
    EXPECT_EQ(recovered.workspace_change_snapshot().at("changed_files").size(), 2);
}

TEST(ChangeTransactionTest, RejectsExternalRewriteBeforeChangingAnyFile) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto journal = temporary.path() / "changeset-transaction.json";
    write_originals(workspace);

    {
        mint::ToolRegistry tools(workspace, transaction_options(journal));
        ASSERT_TRUE(execute_changeset(tools, "externally-rewritten").value("ok", false));
        write_text(workspace / "src" / "alpha.txt", "external value\n");
        EXPECT_THROW(tools.finalize_change_transaction(), std::runtime_error);
        EXPECT_TRUE(std::filesystem::exists(journal));
    }

    mint::ToolRegistry recovered(workspace, transaction_options(journal));
    EXPECT_THROW((void)recovered.reconcile_change_transaction(std::nullopt), std::runtime_error);
    EXPECT_EQ(read_text(workspace / "src" / "alpha.txt"), "external value\n");
    EXPECT_EQ(read_text(workspace / "src" / "beta.txt"), "beta after\n");
    EXPECT_TRUE(std::filesystem::exists(journal));

    write_text(workspace / "src" / "alpha.txt", "alpha after\n");
    EXPECT_EQ(recovered.reconcile_change_transaction(std::nullopt),
              mint::ChangeTransactionRecovery::rolled_back);
    expect_originals(workspace);
}

TEST(ChangeTransactionTest, SerializesWritersWithAnExclusiveTaskLock) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto journal = temporary.path() / "changeset-transaction.json";
    write_originals(workspace);

    {
        mint::ToolRegistry first(workspace, transaction_options(journal));
        EXPECT_EQ(run_lock_helper_process(workspace, journal), 0);
    }

    EXPECT_NO_THROW({ mint::ToolRegistry after_release(workspace, transaction_options(journal)); });
    EXPECT_EQ(mint::change_transaction_path_for_session(temporary.path() / "session.json"),
              journal);
    EXPECT_EQ(mint::change_transaction_lock_path(journal).filename(),
              "changeset-transaction.json.lock");
}

TEST(ChangeTransactionTest, RejectsAliasedJournalAndLockFiles) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto journal = temporary.path() / "aliased-transaction.json";
    const auto target = temporary.path() / "must-not-change.txt";
    write_originals(workspace);
    write_text(target, "protected content\n");

    std::error_code error;
    std::filesystem::create_hard_link(target, journal, error);
    if (!error) {
        EXPECT_THROW((void)mint::ToolRegistry(workspace, transaction_options(journal)),
                     std::invalid_argument);
        EXPECT_EQ(read_text(target), "protected content\n");
        ASSERT_TRUE(std::filesystem::remove(journal));
    }

    error.clear();
    std::filesystem::create_symlink(target, journal, error);
    if (!error) {
        EXPECT_THROW((void)mint::ToolRegistry(workspace, transaction_options(journal)),
                     std::invalid_argument);
        EXPECT_EQ(read_text(target), "protected content\n");
        ASSERT_TRUE(std::filesystem::remove(journal));
    }

    const auto lock = mint::change_transaction_lock_path(journal);
    error.clear();
    std::filesystem::create_symlink(target, lock, error);
    if (!error) {
        EXPECT_THROW((void)mint::ToolRegistry(workspace, transaction_options(journal)),
                     std::invalid_argument);
        EXPECT_EQ(read_text(target), "protected content\n");
    }
}

TEST(ChangeTransactionTest, AgentRollsBackAndReplaysAnInflightChangeset) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto session_path = temporary.path() / "session.json";
    const auto journal = mint::change_transaction_path_for_session(session_path);
    const auto event_path = temporary.path() / "events.jsonl";
    write_originals(workspace);
    mint::SessionStore session(session_path);

    auto control = std::make_shared<mint::TaskControl>();
    {
        mint::ToolRegistry tools(workspace, transaction_options(journal));
        StopAfterChangesetReplyModel model(control);
        std::ostringstream output;
        mint::Agent agent(
            model, tools, output,
            mint::AgentOptions{.max_turns = 1, .task_control = control, .session_store = &session});
        const auto stopped = agent.run("change both files");
        ASSERT_EQ(stopped.status, "cancelled");
    }

    auto checkpoint = session.load();
    ASSERT_EQ(checkpoint.at("schema_version"), mint::session_schema_version);
    ASSERT_EQ(checkpoint.at("pending_tool_calls").size(), 1);
    ASSERT_TRUE(checkpoint.at("change_transaction_id").is_null());
    ASSERT_EQ(checkpoint.at("capabilities").at("change_transaction_path"),
              std::filesystem::weakly_canonical(journal).generic_string());

    const auto moved_session_path = temporary.path() / "moved-session.json";
    mint::SessionStore moved_session(moved_session_path);
    moved_session.save(checkpoint);
    bool rejected_moved_session = false;
    try {
        mint::ToolRegistry moved_tools(
            workspace,
            transaction_options(mint::change_transaction_path_for_session(moved_session_path)));
        FinishAfterChangesetModel unused_model;
        std::ostringstream unused_output;
        mint::Agent moved_agent(unused_model, moved_tools, unused_output,
                                mint::AgentOptions{.max_turns = 1,
                                                   .session_store = &moved_session,
                                                   .resume_session = true});
        (void)moved_agent.run("");
    } catch (const std::invalid_argument& error) {
        rejected_moved_session = std::string(error.what()).find("能力授权") != std::string::npos;
    }
    EXPECT_TRUE(rejected_moved_session);
    expect_originals(workspace);

    checkpoint["status"] = "running";
    checkpoint["in_flight_tool_call"] = checkpoint.at("pending_tool_calls").at(0);
    session.save(checkpoint);

    {
        mint::ToolRegistry crashed_process(workspace, transaction_options(journal));
        const auto result = execute_changeset(crashed_process, "durable-changeset");
        ASSERT_TRUE(result.value("ok", false));
        ASSERT_TRUE(std::filesystem::exists(journal));
        expect_desired(workspace);
    }

    mint::EventLog events(event_path);
    mint::ToolRegistry resumed_tools(workspace, transaction_options(journal));
    FinishAfterChangesetModel resumed_model;
    std::ostringstream resumed_output;
    mint::Agent resumed_agent(resumed_model, resumed_tools, resumed_output,
                              mint::AgentOptions{.max_turns = 1,
                                                 .event_log = &events,
                                                 .session_store = &session,
                                                 .resume_session = true});
    const auto resumed = resumed_agent.run("");

    EXPECT_TRUE(resumed.completed);
    EXPECT_EQ(resumed.answer, "recovered");
    EXPECT_EQ(resumed.execution.tool_calls, 1);
    expect_desired(workspace);
    EXPECT_FALSE(std::filesystem::exists(journal));
    const auto final_checkpoint = session.load();
    EXPECT_EQ(final_checkpoint.at("schema_version"), mint::session_schema_version);
    EXPECT_TRUE(final_checkpoint.at("change_transaction_id").is_null());

    std::ifstream input(event_path, std::ios::binary);
    std::string first_event;
    ASSERT_TRUE(static_cast<bool>(std::getline(input, first_event)));
    const auto started = mint::Json::parse(first_event);
    EXPECT_EQ(started.at("type"), "task_started");
    EXPECT_EQ(started.at("data").at("transaction_recovery"), "rolled_back");
}

} // namespace

int run_change_transaction_lock_helper(int argc, char** argv) {
    if (argc != 4) {
        return 64;
    }
    try {
        mint::ToolRegistry unexpected_lock_owner(argv[2], transaction_options(argv[3]));
        return 65;
    } catch (const std::runtime_error& error) {
        return std::string(error.what()).find("另一个 mint 进程") == std::string::npos ? 66 : 0;
    } catch (...) {
        return 67;
    }
}
