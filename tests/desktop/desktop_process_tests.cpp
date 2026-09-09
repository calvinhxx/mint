#include "application/model_settings_controller.hpp"
#include "application/task_workspace_controller.hpp"
#include "infrastructure/cli/mint_cli_command_builder.hpp"
#include "infrastructure/cli/mint_cli_process.hpp"
#include "infrastructure/settings/shared_model_settings_runtime.hpp"

#include "mint/domain/model.hpp"
#include "mint/infrastructure/model_config_service.hpp"

#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace {

using mint::desktop::RunRequest;
using mint::desktop::RuntimeFailure;
using mint::desktop::RuntimeOperation;
using mint::desktop::TaskSummary;
using mint::infrastructure::desktop::MintCliCommandBuilder;
using mint::infrastructure::desktop::MintCliProcess;

std::filesystem::path path_from_qstring(const QString& path) {
#if defined(_WIN32)
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}

class ScopedWorkingDirectory final {
  public:
    ScopedWorkingDirectory() : previous_(std::filesystem::current_path()) {}
    ~ScopedWorkingDirectory() {
        QThreadPool::globalInstance()->waitForDone();
        std::error_code error;
        std::filesystem::current_path(previous_, error);
    }

  private:
    std::filesystem::path previous_;
};

class DesktopProcessOutcome : public testing::TestWithParam<const char*> {};

TEST_P(DesktopProcessOutcome, PreservesStructuredTaskOutcomeForNormalProcessExit) {
    QTemporaryDir workspace;
    ASSERT_TRUE(workspace.isValid());
    MintCliProcess process(std::filesystem::path(MINT_TEST_PROCESS_FIXTURE_PATH));
    std::optional<mint::desktop::TaskResult> result;
    std::string error;
    process.set_callbacks({
        .run_finished = [&](mint::desktop::TaskResult value) { result = std::move(value); },
        .error = [&](RuntimeFailure failure) { error = std::move(failure.detail); },
    });
    process.start_task({.root = path_from_qstring(workspace.path()), .question = GetParam()});
    ASSERT_TRUE(QTest::qWaitFor([&] { return result.has_value() || !error.empty(); }));
    EXPECT_TRUE(error.empty()) << error;
    ASSERT_TRUE(result.has_value());
    const std::string scenario = GetParam();
    EXPECT_EQ(result->status,
              scenario == "partial" || scenario == "trailing_noise" ? "completed" : scenario);
    EXPECT_EQ(result->answer, "Saved task outcome");
    EXPECT_FALSE(process.running());
}

INSTANTIATE_TEST_SUITE_P(TerminalResults, DesktopProcessOutcome,
                         testing::Values("completed", "max_turns", "cancelled", "timed_out",
                                         "budget_exhausted", "failed", "partial",
                                         "trailing_noise"));

TEST(DesktopProcess, MissingResultReportsExitCodeInsteadOfUnknownError) {
    QTemporaryDir workspace;
    ASSERT_TRUE(workspace.isValid());
    MintCliProcess process(std::filesystem::path(MINT_TEST_PROCESS_FIXTURE_PATH));
    std::string error;
    bool result_received = false;
    process.set_callbacks({
        .run_finished = [&](mint::desktop::TaskResult) { result_received = true; },
        .error = [&](RuntimeFailure failure) { error = std::move(failure.detail); },
    });
    process.start_task({.root = path_from_qstring(workspace.path()), .question = "missing_result"});
    ASSERT_TRUE(QTest::qWaitFor([&] { return result_received || !error.empty(); }));
    EXPECT_FALSE(result_received);
    EXPECT_NE(error.find("2"), std::string::npos);
    EXPECT_EQ(error.find("Unknown error"), std::string::npos);
}

TEST(DesktopProcess, RunFailureKeepsTheStructuredError) {
    QTemporaryDir workspace;
    ASSERT_TRUE(workspace.isValid());
    MintCliProcess process(std::filesystem::path(MINT_TEST_PROCESS_FIXTURE_PATH));
    std::string error;
    process.set_callbacks(
        {.error = [&](RuntimeFailure failure) { error = std::move(failure.detail); }});
    process.start_task({.root = path_from_qstring(workspace.path()), .question = "error"});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !error.empty(); }));
    EXPECT_EQ(error, "fixture failure");
}

TEST(DesktopProcess, LongEventHistoryYieldsToTheUiAndDrainsBeforeTheTerminalResult) {
    QTemporaryDir workspace;
    ASSERT_TRUE(workspace.isValid());
    MintCliProcess process(std::filesystem::path(MINT_TEST_PROCESS_FIXTURE_PATH));
    std::size_t received = 0;
    std::size_t batches = 0;
    std::size_t maximum_batch = 0;
    std::size_t last_sequence = 0;
    int heartbeat_ticks = 0;
    int first_batch_tick = 0;
    bool finished = false;
    std::string error;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeat_ticks; });
    heartbeat.start(0);
    process.set_callbacks({
        .events_received =
            [&](std::vector<mint::desktop::RunEvent> events) {
                if (batches++ == 0) {
                    first_batch_tick = heartbeat_ticks;
                }
                maximum_batch = std::max(maximum_batch, events.size());
                for (const auto& event : events) {
                    EXPECT_EQ(event.sequence, ++last_sequence);
                }
                received += events.size();
                EXPECT_TRUE(process.running());
                EXPECT_FALSE(finished);
            },
        .run_finished =
            [&](mint::desktop::TaskResult result) {
                EXPECT_EQ(result.status, "completed");
                EXPECT_EQ(received, 8193U);
                EXPECT_FALSE(process.running());
                finished = true;
            },
        .error = [&](RuntimeFailure failure) { error = std::move(failure.detail); },
    });
    process.start_task({.root = path_from_qstring(workspace.path()), .question = "long_events"});
    ASSERT_TRUE(QTest::qWaitFor([&] { return finished || !error.empty(); }, 15'000));
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_GT(batches, 1U);
    EXPECT_LE(maximum_batch, 128U);
    EXPECT_GT(heartbeat_ticks, first_batch_tick + 1);
    EXPECT_EQ(last_sequence, 8193U);
}

TEST(DesktopProcess, ANewRunCannotReceiveQueuedFramesFromThePreviousRun) {
    QTemporaryDir workspace;
    ASSERT_TRUE(workspace.isValid());
    MintCliProcess process(std::filesystem::path(MINT_TEST_PROCESS_FIXTURE_PATH));
    std::size_t first_events = 0;
    std::size_t second_events = 0;
    int finished = 0;
    std::string error;
    process.set_callbacks({
        .events_received =
            [&](std::vector<mint::desktop::RunEvent> events) {
                if (finished == 0) {
                    first_events += events.size();
                } else {
                    second_events += events.size();
                    for (const auto& event : events) {
                        EXPECT_EQ(event.sequence, 90'001U);
                    }
                }
            },
        .run_finished =
            [&](mint::desktop::TaskResult) {
                if (++finished == 1) {
                    EXPECT_EQ(first_events, 8193U);
                    process.start_task(
                        {.root = path_from_qstring(workspace.path()), .question = "one_event"});
                }
            },
        .error = [&](RuntimeFailure failure) { error = std::move(failure.detail); },
    });
    process.start_task({.root = path_from_qstring(workspace.path()), .question = "long_events"});
    ASSERT_TRUE(QTest::qWaitFor([&] { return finished == 2 || !error.empty(); }, 15'000));
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(first_events, 8193U);
    EXPECT_EQ(second_events, 1U);
}

TEST(DesktopProcess, DestroyingTheRuntimeDiscardsQueuedEventFrames) {
    QTemporaryDir workspace;
    ASSERT_TRUE(workspace.isValid());
    auto* process = new MintCliProcess(std::filesystem::path(MINT_TEST_PROCESS_FIXTURE_PATH));
    QPointer<MintCliProcess> lifetime(process);
    std::size_t events_received = 0;
    bool deletion_queued = false;
    process->set_callbacks({
        .events_received =
            [&](std::vector<mint::desktop::RunEvent> events) {
                events_received += events.size();
                if (!deletion_queued) {
                    deletion_queued = true;
                    QTimer::singleShot(0, process, [process] { delete process; });
                }
            },
    });
    process->start_task({.root = path_from_qstring(workspace.path()), .question = "long_events"});
    ASSERT_TRUE(QTest::qWaitFor([&] { return lifetime.isNull(); }, 15'000));
    const auto count_at_destruction = events_received;
    QTest::qWait(50);
    EXPECT_GT(count_at_destruction, 0U);
    EXPECT_LT(count_at_destruction, 8193U);
    EXPECT_EQ(events_received, count_at_destruction);
}

TEST(DesktopProcess, CancellingABrokenApprovalChannelKeepsExecutionAdmissionUntilExit) {
    QTemporaryDir workspace;
    ASSERT_TRUE(workspace.isValid());
    MintCliProcess process(std::filesystem::path(MINT_TEST_PROCESS_FIXTURE_PATH));
    mint::desktop::TaskWorkspaceController controller(
        process, {.root = path_from_qstring(workspace.path())});
    ASSERT_TRUE(controller.start("closed_approval_input"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().approval.has_value(); }));
    controller.cancel();
    EXPECT_TRUE(process.running());
    EXPECT_TRUE(controller.state().running);
    EXPECT_FALSE(controller.start("missing_result"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !controller.state().running; }));
    ASSERT_TRUE(controller.state().result.has_value());
    EXPECT_EQ(controller.state().result->status, "cancelled");
}

TEST(DesktopProcess, CrashDoesNotPromoteAnEarlierStructuredResultToCompletion) {
    QTemporaryDir workspace;
    ASSERT_TRUE(workspace.isValid());
    MintCliProcess process(std::filesystem::path(MINT_TEST_PROCESS_FIXTURE_PATH));
    bool finished = false;
    std::string error;
    process.set_callbacks({
        .run_finished = [&](mint::desktop::TaskResult) { finished = true; },
        .error = [&](RuntimeFailure failure) { error = std::move(failure.detail); },
    });
    process.start_task(
        {.root = path_from_qstring(workspace.path()), .question = "crash_after_result"});
    ASSERT_TRUE(QTest::qWaitFor([&] { return finished || !error.empty(); }));
    EXPECT_FALSE(finished);
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(process.running());
}

TEST(DesktopProcess, SharedModelRequestsContainNoCredentialOverride) {
    const MintCliCommandBuilder builder;
    const auto arguments = builder.run(
        {.root = "/workspace", .question = "Inspect", .language = "en"}, "/state/cancel");
    EXPECT_EQ(std::find(arguments.begin(), arguments.end(), "--api-key-env"), arguments.end());
    EXPECT_EQ(std::find(arguments.begin(), arguments.end(), "--config"), arguments.end());
}

TEST(DesktopProcess, RelativeStateDirectoryStaysSharedWhenTheWorkspaceChanges) {
    QTemporaryDir launch_directory;
    QTemporaryDir workspace;
    ASSERT_TRUE(launch_directory.isValid());
    ASSERT_TRUE(workspace.isValid());
    ScopedWorkingDirectory restore_directory;
    std::filesystem::current_path(path_from_qstring(launch_directory.path()));
    const MintCliCommandBuilder builder("shared-state");
    mint::infrastructure::desktop::SharedModelSettingsRuntime runtime("shared-state");
    std::filesystem::current_path(path_from_qstring(workspace.path()));

    mint::desktop::ModelSettingsController controller(
        runtime, {.root = path_from_qstring(workspace.path())});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().provider.has_value(); }));
    ASSERT_TRUE(
        controller.save_custom_profile({.expected_revision = controller.state().provider->revision,
                                        .base_url = "https://fixture.example.test/v1",
                                        .model = "same-model-from-another-workspace"}));
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().save_generation == 1; }));

    const auto expected_state = path_from_qstring(launch_directory.path()) / "shared-state";
    EXPECT_EQ(mint::ModelConfigService(expected_state).inspect().config.model,
              "same-model-from-another-workspace");
    EXPECT_FALSE(std::filesystem::exists(path_from_qstring(workspace.path()) / "shared-state"));
    const auto arguments = builder.run(
        {.root = path_from_qstring(workspace.path()), .question = "Inspect", .language = "en"},
        expected_state / "cancel");
    const auto state_argument = std::find(arguments.begin(), arguments.end(), "--state-dir");
    ASSERT_NE(state_argument, arguments.end());
    ASSERT_NE(std::next(state_argument), arguments.end());
    const auto shared_path = QString::fromUtf8(*std::next(state_argument));
    EXPECT_TRUE(std::filesystem::equivalent(path_from_qstring(shared_path), expected_state));

    QProcess cli;
    cli.setWorkingDirectory(workspace.path());
    cli.start(QString::fromUtf8(MINT_TEST_CLI_PATH),
              {QStringLiteral("config"), QStringLiteral("show"), QStringLiteral("--state-dir"),
               shared_path, QStringLiteral("--json")});
    ASSERT_TRUE(cli.waitForStarted(2'000));
    ASSERT_TRUE(cli.waitForFinished(5'000));
    ASSERT_EQ(cli.exitCode(), 0) << cli.readAllStandardError().constData();
    const auto snapshot = QJsonDocument::fromJson(cli.readAllStandardOutput()).object();
    EXPECT_EQ(snapshot.value(QStringLiteral("model")).toString(),
              QStringLiteral("same-model-from-another-workspace"));
}

TEST(DesktopProcess, OfflineDemoCompletesAndAppearsInTaskHistory) {
    QTemporaryDir workspace;
    QTemporaryDir state;
    ASSERT_TRUE(workspace.isValid());
    ASSERT_TRUE(state.isValid());

    QFile readme(workspace.filePath(QStringLiteral("README.md")));
    ASSERT_TRUE(readme.open(QIODevice::WriteOnly));
    ASSERT_GT(readme.write("# Mint Agent\n\nOffline desktop integration fixture.\n"), 0);
    readme.close();

    QProcess initializer;
    initializer.start(QString::fromUtf8(MINT_TEST_CLI_PATH),
                      {QStringLiteral("init"), QStringLiteral("--root"), workspace.path(),
                       QStringLiteral("--state-dir"), state.path(), QStringLiteral("--json")});
    ASSERT_TRUE(initializer.waitForStarted(2'000));
    ASSERT_TRUE(initializer.waitForFinished(5'000));
    ASSERT_EQ(initializer.exitCode(), 0) << initializer.readAllStandardError().constData();

    MintCliProcess process(std::filesystem::path(MINT_TEST_CLI_PATH),
                           path_from_qstring(state.path()));
    QEventLoop run_loop;
    std::string task_id;
    std::string error;
    mint::desktop::TaskResult result;
    bool result_received = false;
    std::vector<std::string> event_types;
    process.set_callbacks({
        .task_ready = [&](std::string id) { task_id = std::move(id); },
        .event_received =
            [&](mint::desktop::RunEvent event) {
                event_types.push_back(std::move(event.raw_type));
            },
        .run_finished =
            [&](mint::desktop::TaskResult value) {
                result = std::move(value);
                result_received = true;
                run_loop.quit();
            },
        .error =
            [&](RuntimeFailure failure) {
                error = std::move(failure.detail);
                run_loop.quit();
            },
    });

    QTimer::singleShot(15'000, &run_loop, &QEventLoop::quit);
    process.start_task({.root = path_from_qstring(workspace.path()),
                        .question = "Inspect this project",
                        .language = "en",
                        .demo = true});
    run_loop.exec();

    EXPECT_FALSE(process.running());
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_FALSE(task_id.empty());
    EXPECT_TRUE(result_received);
    EXPECT_EQ(result.status, "completed");
    EXPECT_GE(event_types.size(), 4U);

    QEventLoop status_loop;
    std::vector<TaskSummary> tasks;
    std::filesystem::path project_directory;
    process.set_callbacks({
        .tasks_loaded =
            [&](std::vector<TaskSummary> loaded, std::filesystem::path directory) {
                tasks = std::move(loaded);
                project_directory = std::move(directory);
                status_loop.quit();
            },
        .error =
            [&](RuntimeFailure failure) {
                error = std::move(failure.detail);
                status_loop.quit();
            },
    });
    QTimer::singleShot(5'000, &status_loop, &QEventLoop::quit);
    process.refresh_tasks(path_from_qstring(workspace.path()), "en");
    status_loop.exec();

    EXPECT_TRUE(error.empty()) << error;
    EXPECT_FALSE(project_directory.empty());
    const auto task = std::find_if(tasks.begin(), tasks.end(), [&](const TaskSummary& summary) {
        return summary.id == task_id;
    });
    ASSERT_NE(task, tasks.end());
    EXPECT_TRUE(task->completed);
    EXPECT_EQ(task->status, "completed");

    QEventLoop load_loop;
    mint::desktop::TaskHistory history;
    bool history_received = false;
    process.set_callbacks({
        .task_loaded =
            [&](std::string loaded_task_id, mint::desktop::TaskHistory value) {
                EXPECT_EQ(loaded_task_id, task_id);
                history = std::move(value);
                history_received = true;
                load_loop.quit();
            },
    });
    QTimer::singleShot(5'000, &load_loop, &QEventLoop::quit);
    process.load_task(task_id);
    load_loop.exec();

    EXPECT_TRUE(history_received);
    EXPECT_GE(history.events.size(), 4U);
    EXPECT_TRUE(history.result.has_value());
}

TEST(DesktopProcess, FailedStatusProcessDoesNotLockFutureRefreshes) {
    QTemporaryDir workspace;
    QTemporaryDir state;
    ASSERT_TRUE(workspace.isValid());
    ASSERT_TRUE(state.isValid());

    MintCliProcess process(path_from_qstring(workspace.filePath(QStringLiteral("missing-mint"))),
                           path_from_qstring(state.path()));
    int error_count = 0;
    RuntimeOperation operation = RuntimeOperation::task_run;
    for (int attempt = 0; attempt < 2; ++attempt) {
        QEventLoop loop;
        process.set_callbacks({.error = [&](RuntimeFailure failure) {
            ++error_count;
            operation = failure.operation;
            loop.quit();
        }});
        QTimer::singleShot(2'000, &loop, &QEventLoop::quit);
        process.refresh_tasks(path_from_qstring(workspace.path()), "en");
        loop.exec();
    }

    EXPECT_EQ(error_count, 2);
    EXPECT_EQ(operation, RuntimeOperation::task_history);
}

TEST(DesktopProcess, StatusFailurePrefersTheMachineReadableError) {
    QTemporaryDir workspace;
    QTemporaryDir state;
    ASSERT_TRUE(workspace.isValid());
    ASSERT_TRUE(state.isValid());

    MintCliProcess process(std::filesystem::path(MINT_TEST_CLI_PATH),
                           path_from_qstring(state.path()));
    QEventLoop loop;
    RuntimeFailure failure;
    process.set_callbacks({.error = [&](RuntimeFailure value) {
        failure = std::move(value);
        loop.quit();
    }});
    QTimer::singleShot(5'000, &loop, &QEventLoop::quit);
    process.refresh_tasks(path_from_qstring(workspace.path()), "en");
    loop.exec();

    EXPECT_EQ(failure.operation, RuntimeOperation::task_history);
    EXPECT_FALSE(failure.detail.empty());
    EXPECT_EQ(failure.detail.find("event=process.failed"), std::string::npos);
}

TEST(DesktopProcess, LatestProjectRefreshWins) {
    QTemporaryDir first_workspace;
    QTemporaryDir second_workspace;
    QTemporaryDir state;
    ASSERT_TRUE(first_workspace.isValid());
    ASSERT_TRUE(second_workspace.isValid());
    ASSERT_TRUE(state.isValid());

    for (const auto& workspace : {first_workspace.path(), second_workspace.path()}) {
        QProcess initializer;
        initializer.start(QString::fromUtf8(MINT_TEST_CLI_PATH),
                          {QStringLiteral("init"), QStringLiteral("--root"), workspace,
                           QStringLiteral("--state-dir"), state.path(), QStringLiteral("--json")});
        ASSERT_TRUE(initializer.waitForStarted(2'000));
        ASSERT_TRUE(initializer.waitForFinished(5'000));
        ASSERT_EQ(initializer.exitCode(), 0) << initializer.readAllStandardError().constData();
    }

    QProcess seed;
    seed.start(QString::fromUtf8(MINT_TEST_CLI_PATH),
               {QStringLiteral("run"), QStringLiteral("--root"), second_workspace.path(),
                QStringLiteral("--state-dir"), state.path(), QStringLiteral("--demo"),
                QStringLiteral("--json"), QStringLiteral("Create the second project task")});
    ASSERT_TRUE(seed.waitForStarted(2'000));
    ASSERT_TRUE(seed.waitForFinished(10'000));
    ASSERT_EQ(seed.exitCode(), 0) << seed.readAllStandardError().constData();

    MintCliProcess process(std::filesystem::path(MINT_TEST_CLI_PATH),
                           path_from_qstring(state.path()));
    QEventLoop loop;
    std::vector<TaskSummary> tasks;
    RuntimeFailure failure;
    process.set_callbacks({
        .tasks_loaded =
            [&](std::vector<TaskSummary> loaded, std::filesystem::path) {
                tasks = std::move(loaded);
                loop.quit();
            },
        .error =
            [&](RuntimeFailure value) {
                failure = std::move(value);
                loop.quit();
            },
    });
    process.refresh_tasks(path_from_qstring(first_workspace.path()), "en");
    process.refresh_tasks(path_from_qstring(second_workspace.path()), "en");
    QTimer::singleShot(5'000, &loop, &QEventLoop::quit);
    loop.exec();

    EXPECT_TRUE(failure.detail.empty()) << failure.detail;
    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_EQ(tasks.front().question, "Create the second project task");
}

TEST(DesktopProcess, ResumeTargetsTheSelectedManagedTask) {
    QTemporaryDir workspace;
    QTemporaryDir state;
    ASSERT_TRUE(workspace.isValid());
    ASSERT_TRUE(state.isValid());

    QProcess initializer;
    initializer.start(QString::fromUtf8(MINT_TEST_CLI_PATH),
                      {QStringLiteral("init"), QStringLiteral("--root"), workspace.path(),
                       QStringLiteral("--state-dir"), state.path(), QStringLiteral("--json")});
    ASSERT_TRUE(initializer.waitForStarted(2'000));
    ASSERT_TRUE(initializer.waitForFinished(5'000));
    ASSERT_EQ(initializer.exitCode(), 0) << initializer.readAllStandardError().constData();

    const auto missing_config = workspace.filePath(QStringLiteral("missing-config.json"));
    QProcess seed;
    seed.start(QString::fromUtf8(MINT_TEST_CLI_PATH),
               {QStringLiteral("run"), QStringLiteral("--root"), workspace.path(),
                QStringLiteral("--state-dir"), state.path(), QStringLiteral("--config"),
                missing_config, QStringLiteral("--json"), QStringLiteral("Resume this task")});
    ASSERT_TRUE(seed.waitForStarted(2'000));
    ASSERT_TRUE(seed.waitForFinished(5'000));
    ASSERT_NE(seed.exitCode(), 0);
    const auto seeded = mint::Json::parse(seed.readAllStandardOutput().trimmed().toStdString());
    const auto task_id = seeded.value("task_id", std::string{});
    ASSERT_FALSE(task_id.empty());
    const auto task_directory = QString::fromStdString(seeded.value("task_directory", ""));
    QFile metadata(QDir(task_directory).filePath(QStringLiteral("task.json")));
    ASSERT_TRUE(metadata.open(QIODevice::ReadOnly));
    const auto task_metadata = mint::Json::parse(metadata.readAll().toStdString());
    metadata.close();
    QFile session(QDir(task_directory).filePath(QStringLiteral("session.json")));
    ASSERT_TRUE(session.open(QIODevice::WriteOnly));
    const auto checkpoint = mint::Json{
        {"schema_version", 5},     {"workspace_root", task_metadata.value("workspace_root", "")},
        {"status", "timed_out"},   {"verification_status", "not_run"},
        {"turns", std::size_t{1}},
    };
    ASSERT_GT(session.write(QByteArray::fromStdString(checkpoint.dump())), 0);
    session.close();
    ASSERT_TRUE(session.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    MintCliProcess process(std::filesystem::path(MINT_TEST_CLI_PATH),
                           path_from_qstring(state.path()));
    QEventLoop loop;
    std::string ready_task_id;
    RuntimeFailure failure;
    process.set_callbacks({
        .task_ready = [&](std::string value) { ready_task_id = std::move(value); },
        .error =
            [&](RuntimeFailure value) {
                failure = std::move(value);
                loop.quit();
            },
    });
    QTimer::singleShot(5'000, &loop, &QEventLoop::quit);
    process.resume_task({.root = path_from_qstring(workspace.path()),
                         .config = path_from_qstring(missing_config),
                         .task_id = task_id,
                         .language = "en"});
    loop.exec();

    EXPECT_FALSE(process.running());
    EXPECT_EQ(ready_task_id, task_id) << failure.detail;
    EXPECT_EQ(failure.operation, RuntimeOperation::task_run);
    EXPECT_FALSE(failure.detail.empty());
}

} // namespace
