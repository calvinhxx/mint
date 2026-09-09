#include "application/model_settings_controller.hpp"
#include "infrastructure/settings/shared_model_settings_runtime.hpp"
#include "mint/infrastructure/model_config_service.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string_view>

namespace {

using mint::desktop::ModelSettingsController;
using mint::desktop::ProviderDescriptor;
using mint::desktop::SavedCredentialState;
using mint::infrastructure::desktop::SharedModelSettingsRuntime;

constexpr auto gui_key = "gui-cli-integration-first-fixture-key";
constexpr auto cli_key = "gui-cli-integration-second-fixture-key";
constexpr auto override_key = "gui-cli-integration-override-fixture-key";
constexpr std::array<std::string_view, 3> fixture_keys{gui_key, cli_key, override_key};

std::filesystem::path path_from_qstring(const QString& value) {
#if defined(_WIN32)
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toUtf8().constData());
#endif
}

enum class OfflineCommand { show_config, inspect_provider, set_config };

struct CliResult {
    bool started = false;
    bool finished = false;
    bool normal_exit = false;
    bool valid_json = false;
    bool output_redacted = true;
    int exit_code = -1;
    QJsonObject object;
};

class SharedConfigCliIntegration : public ::testing::Test {
  protected:
    void SetUp() override {
        if (QCoreApplication::instance() == nullptr) {
            application_ = std::make_unique<QCoreApplication>(argc_, argv_);
        }
        ASSERT_TRUE(directory_.isValid());
        ASSERT_TRUE(QDir().mkpath(workspace()));
        ASSERT_TRUE(QFileInfo(QString::fromUtf8(MINT_TEST_CLI_PATH)).isExecutable());
    }

    void TearDown() override {
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        application_.reset();
    }

    [[nodiscard]] QString workspace() const {
        return directory_.filePath(QStringLiteral("workspace"));
    }

    [[nodiscard]] std::filesystem::path state_root() const {
        return path_from_qstring(directory_.filePath(QStringLiteral("state")));
    }

    [[nodiscard]] CliResult invoke_cli(OfflineCommand command,
                                       const QJsonObject& update = {}) const {
        QStringList arguments;
        switch (command) {
        case OfflineCommand::show_config:
            arguments = {QStringLiteral("config"), QStringLiteral("show")};
            break;
        case OfflineCommand::inspect_provider:
            arguments = {QStringLiteral("provider")};
            break;
        case OfflineCommand::set_config:
            arguments = {QStringLiteral("config"), QStringLiteral("set"),
                         QStringLiteral("--stdin-json")};
            break;
        }
        arguments.append({QStringLiteral("--state-dir"),
                          directory_.filePath(QStringLiteral("state")), QStringLiteral("--lang"),
                          QStringLiteral("en"), QStringLiteral("--json")});
        QProcess process;
        // EN: Fixed inspect/set commands, loopback fixtures and an empty child environment
        // isolate this cross-process test from real providers and user credentials.
        // ZH-CN: 固定离线命令、回环地址和空子进程环境，隔离真实供应商及用户凭据。
        process.setProcessEnvironment(QProcessEnvironment{});
        process.setWorkingDirectory(workspace());
        process.start(QString::fromUtf8(MINT_TEST_CLI_PATH), arguments);
        CliResult result;
        result.started = process.waitForStarted(2'000);
        if (!result.started) {
            return result;
        }
        if (command == OfflineCommand::set_config) {
            process.write(QJsonDocument(update).toJson(QJsonDocument::Compact));
        }
        process.closeWriteChannel();
        result.finished = process.waitForFinished(5'000);
        if (!result.finished) {
            process.kill();
            process.waitForFinished(1'000);
        }
        result.normal_exit = process.exitStatus() == QProcess::NormalExit;
        result.exit_code = process.exitCode();
        const auto output = process.readAllStandardOutput();
        const auto errors = process.readAllStandardError();
        for (const auto key : fixture_keys) {
            const auto bytes = QByteArray(key.data(), static_cast<qsizetype>(key.size()));
            result.output_redacted &= !output.contains(bytes) && !errors.contains(bytes);
        }
        QJsonParseError parse_error;
        const auto document = QJsonDocument::fromJson(output, &parse_error);
        result.valid_json = parse_error.error == QJsonParseError::NoError && document.isObject();
        result.object = document.object();
        return result;
    }

    void expect_snapshot(const CliResult& result, const ProviderDescriptor& gui,
                         bool provider_report = false) const {
        ASSERT_TRUE(result.started);
        ASSERT_TRUE(result.finished);
        ASSERT_TRUE(result.normal_exit);
        ASSERT_EQ(result.exit_code, 0);
        ASSERT_TRUE(result.output_redacted);
        ASSERT_TRUE(result.valid_json);
        const auto& object = result.object;
        // EN: Boolean comparisons keep credential values out of failure output, including
        // accidental leaks into an otherwise public response field.
        // ZH-CN: 使用布尔比较，避免断言失败输出密钥，包括误入公开响应字段的值。
        EXPECT_TRUE(object.value(QStringLiteral("config")).toString().toStdString() ==
                    gui.config.generic_string());
        EXPECT_TRUE(object.value(QStringLiteral("revision")).toString().toStdString() ==
                    gui.revision);
        EXPECT_TRUE(object.value(QStringLiteral("model")).toString().toStdString() == gui.model);
        EXPECT_TRUE(object.value(QStringLiteral("endpoint")).toString().toStdString() ==
                    gui.endpoint);
        EXPECT_TRUE(object.value(QStringLiteral("adapter")).toString().toStdString() ==
                    gui.adapter);
        if (provider_report) {
            EXPECT_TRUE(object.value(QStringLiteral("operation")) == QStringLiteral("inspect"));
            EXPECT_TRUE(object.value(QStringLiteral("authentication")) ==
                        QStringLiteral("local_store"));
        } else {
            EXPECT_TRUE(object.value(QStringLiteral("configured")).toBool());
            EXPECT_FALSE(object.value(QStringLiteral("override")).toBool());
            EXPECT_TRUE(object.value(QStringLiteral("saved_api_key")).toBool());
            EXPECT_TRUE(object.value(QStringLiteral("credential_available")).toBool());
        }
        EXPECT_FALSE(object.contains(QStringLiteral("api_key")));
    }

    QTemporaryDir directory_;

  private:
    int argc_ = 1;
    char executable_[28] = "mint-shared-config-test";
    char* argv_[2] = {executable_, nullptr};
    std::unique_ptr<QCoreApplication> application_;
};

TEST_F(SharedConfigCliIntegration, GuiSaveAndCliUpdateUseOneConfigurationAndCredentialSelection) {
    SharedModelSettingsRuntime runtime(state_root());
    ModelSettingsController controller(runtime, {.root = path_from_qstring(workspace())});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().provider.has_value(); }));
    ASSERT_TRUE(
        controller.save_custom_profile({.expected_revision = controller.state().provider->revision,
                                        .base_url = "http://127.0.0.1:48101/v1",
                                        .model = "gui-fixture-model",
                                        .api_key = gui_key}));
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().save_generation == 1; }));
    const auto gui_snapshot = *controller.state().provider;
    EXPECT_EQ(gui_snapshot.config, state_root() / "model.json");
    EXPECT_FALSE(gui_snapshot.revision.empty());
    EXPECT_EQ(controller.state().saved_credential, SavedCredentialState::ready);
    EXPECT_TRUE(gui_snapshot.credential_available);
    expect_snapshot(invoke_cli(OfflineCommand::show_config), gui_snapshot);
    expect_snapshot(invoke_cli(OfflineCommand::inspect_provider), gui_snapshot, true);

    mint::ModelConfigService service(state_root());
    EXPECT_TRUE(service.load_runtime().api_key == gui_key);
    const QJsonObject update{
        {QStringLiteral("expected_revision"), QString::fromStdString(gui_snapshot.revision)},
        {QStringLiteral("adapter"), QStringLiteral("chat_completions")},
        {QStringLiteral("base_url"), QStringLiteral("http://127.0.0.1:48102/v1")},
        {QStringLiteral("model"), QStringLiteral("cli-fixture-model")},
        {QStringLiteral("api_key"), QString::fromUtf8(cli_key)}};
    const auto cli_update = invoke_cli(OfflineCommand::set_config, update);
    ASSERT_TRUE(cli_update.started && cli_update.finished && cli_update.normal_exit);
    ASSERT_EQ(cli_update.exit_code, 0);
    ASSERT_TRUE(cli_update.output_redacted && cli_update.valid_json);
    const auto updated_revision =
        cli_update.object.value(QStringLiteral("revision")).toString().toStdString();
    ASSERT_FALSE(updated_revision.empty());
    EXPECT_NE(updated_revision, gui_snapshot.revision);
    controller.refresh();
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return !controller.state().loading && controller.state().provider &&
               controller.state().provider->revision == updated_revision;
    }));
    const auto& refreshed = *controller.state().provider;
    EXPECT_EQ(refreshed.model, "cli-fixture-model");
    EXPECT_EQ(refreshed.endpoint, "http://127.0.0.1:48102/v1/chat/completions");
    EXPECT_EQ(refreshed.config, gui_snapshot.config);
    EXPECT_EQ(controller.state().saved_credential, SavedCredentialState::ready);
    expect_snapshot(cli_update, refreshed);
    expect_snapshot(invoke_cli(OfflineCommand::show_config), refreshed);
    expect_snapshot(invoke_cli(OfflineCommand::inspect_provider), refreshed, true);
    const auto selected = service.load_runtime();
    EXPECT_TRUE(selected.api_key == cli_key);
    EXPECT_TRUE(selected.api_key != gui_key);
    EXPECT_EQ(selected.model, refreshed.model);
    EXPECT_EQ(selected.api_url, refreshed.endpoint);
}

TEST_F(SharedConfigCliIntegration,
       SavingAnExplicitGuiOverrideMakesNewCliProcessesUseSharedDefault) {
    const auto override_path = QDir(workspace()).filePath(QStringLiteral("override.json"));
    const QJsonObject legacy{
        {QStringLiteral("provider"), QStringLiteral("custom")},
        {QStringLiteral("adapter"), QStringLiteral("responses")},
        {QStringLiteral("endpoint"), QStringLiteral("http://127.0.0.1:48103/v1/responses")},
        {QStringLiteral("model"), QStringLiteral("override-fixture-model")},
        {QStringLiteral("api_key"), QString::fromUtf8(override_key)}};
    const auto original = QJsonDocument(legacy).toJson(QJsonDocument::Compact);
    QFile file(override_path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(original), original.size());
    file.close();

    SharedModelSettingsRuntime runtime(state_root());
    ModelSettingsController controller(
        runtime, {.root = path_from_qstring(workspace()), .config = "override.json"});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().provider.has_value(); }));
    ASSERT_TRUE(controller.state().provider->is_override);
    EXPECT_EQ(controller.state().provider->model, "override-fixture-model");
    EXPECT_FALSE(controller.state().selected_config.empty());
    ASSERT_TRUE(
        controller.save_custom_profile({.expected_revision = controller.state().provider->revision,
                                        .adapter = mint::desktop::CustomModelAdapter::responses,
                                        .base_url = "http://127.0.0.1:48104/v1",
                                        .model = "shared-fixture-model",
                                        .api_key = gui_key}));
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().save_generation == 1; }));
    const auto saved_revision = controller.state().provider->revision;
    EXPECT_TRUE(controller.state().selected_config.empty());
    EXPECT_FALSE(controller.state().provider->is_override);
    controller.refresh();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !controller.state().loading; }));
    const auto& shared = *controller.state().provider;
    EXPECT_TRUE(controller.state().selected_config.empty());
    EXPECT_FALSE(shared.is_override);
    EXPECT_EQ(shared.revision, saved_revision);
    EXPECT_EQ(shared.config, state_root() / "model.json");
    EXPECT_EQ(shared.model, "shared-fixture-model");
    EXPECT_EQ(shared.endpoint, "http://127.0.0.1:48104/v1/responses");
    expect_snapshot(invoke_cli(OfflineCommand::show_config), shared);
    expect_snapshot(invoke_cli(OfflineCommand::inspect_provider), shared, true);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_TRUE(file.readAll() == original);
    mint::ModelConfigService service(state_root());
    const auto selected = service.load_runtime();
    EXPECT_TRUE(selected.api_key == gui_key);
    EXPECT_TRUE(selected.api_key != override_key);
    EXPECT_EQ(selected.model, shared.model);
    EXPECT_EQ(selected.api_url, shared.endpoint);
}

} // namespace
