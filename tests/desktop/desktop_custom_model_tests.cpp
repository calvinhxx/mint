#include "application/model_settings_controller.hpp"
#include "infrastructure/settings/shared_model_settings_runtime.hpp"
#include "mint/infrastructure/model_config_service.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

#include <gtest/gtest.h>

#include <filesystem>

namespace {

using mint::desktop::ModelSettingsController;
using mint::desktop::ModelSettingsProblem;
using mint::desktop::SavedCredentialState;
using mint::infrastructure::desktop::SharedModelSettingsRuntime;

void ensure_application() {
    static int argc = 1;
    static char executable[] = "mint-model-settings-test";
    static char* argv[] = {executable, nullptr};
    static QCoreApplication application(argc, argv);
}

std::filesystem::path state_path(const QTemporaryDir& directory) {
    return std::filesystem::path(directory.path().toStdString());
}

bool write_fixture(const QString& path, const QByteArray& contents) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(contents) == contents.size();
}

QByteArray read_fixture(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

class SharedModelSettings : public ::testing::Test {
  protected:
    void SetUp() override {
        ensure_application();
    }
    void TearDown() override {
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
    }
};

TEST_F(SharedModelSettings, EmptyStorePublishesAnEditableUnconfiguredSnapshot) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    SharedModelSettingsRuntime runtime(state_path(directory) / "new-state");
    ModelSettingsController controller(runtime, {.root = "/workspace"});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().provider.has_value(); }));
    EXPECT_FALSE(controller.state().provider->configured);
    EXPECT_TRUE(controller.state().provider->model.empty());
    EXPECT_TRUE(controller.state().provider->endpoint.empty());
    EXPECT_TRUE(controller.state().provider->revision.empty());
    EXPECT_EQ(controller.state().saved_credential, SavedCredentialState::missing);
    EXPECT_FALSE(controller.state().loading);
}

TEST_F(SharedModelSettings, AsyncSaveAndRemovalUseTheSameNativeService) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    SharedModelSettingsRuntime runtime(state_path(directory));
    ModelSettingsController controller(runtime, {.root = "/workspace"});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().provider.has_value(); }));
    ASSERT_TRUE(
        controller.save_custom_profile({.expected_revision = controller.state().provider->revision,
                                        .base_url = "https://fixture.example.test/v1",
                                        .model = "fixture-model",
                                        .api_key = "fixture-secret"}));
    EXPECT_TRUE(controller.state().saving);
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().save_generation == 1; }));
    mint::ModelConfigService service(state_path(directory));
    const auto saved = service.inspect();
    EXPECT_EQ(saved.path, state_path(directory) / "model.json");
    EXPECT_EQ(saved.config.model, "fixture-model");
    EXPECT_TRUE(saved.config.api_key.empty());
    EXPECT_TRUE(saved.saved_api_key);
    EXPECT_EQ(controller.state().saved_credential, SavedCredentialState::ready);
    EXPECT_FALSE(read_fixture(directory.filePath("model.json")).contains("fixture-secret"));
    ASSERT_TRUE(controller.remove_api_key(controller.state().provider->revision));
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().save_generation == 2; }));
    EXPECT_FALSE(service.inspect().saved_api_key);
    EXPECT_EQ(controller.state().saved_credential, SavedCredentialState::missing);
    EXPECT_FALSE(std::filesystem::exists(state_path(directory) / "model-profiles"));
}

TEST_F(SharedModelSettings, LegacyImportIsOneTimeAndPreservesTheOriginal) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto legacy_path = directory.filePath("legacy.json");
    const auto source = QByteArrayLiteral(
        R"({"provider":"custom","adapter":"chat_completions","endpoint":"https://fixture.example.test/v1/chat/completions","model":"legacy-model","api_key":"fixture-legacy-secret"})");
    ASSERT_TRUE(write_fixture(legacy_path, source));
    bool migrated = false;
    SharedModelSettingsRuntime runtime(state_path(directory) / "state",
                                       std::filesystem::path(legacy_path.toStdString()));
    runtime.set_legacy_imported_callback([&] {
        migrated = true;
        return true;
    });
    ModelSettingsController controller(runtime, {.root = "/workspace"});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().provider.has_value(); }));
    EXPECT_TRUE(migrated);
    EXPECT_EQ(controller.state().provider->model, "legacy-model");
    EXPECT_EQ(controller.state().saved_credential, SavedCredentialState::ready);
    EXPECT_EQ(read_fixture(legacy_path), source);
    ASSERT_TRUE(write_fixture(legacy_path, QByteArrayLiteral("{}")));
    controller.refresh();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !controller.state().loading; }));
    EXPECT_EQ(controller.state().provider->model, "legacy-model");
}

TEST_F(SharedModelSettings, FailedMigrationDoesNotClearTheLegacyPointer) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    bool migrated = false;
    SharedModelSettingsRuntime runtime(state_path(directory) / "state",
                                       state_path(directory) / "missing.json");
    runtime.set_legacy_imported_callback([&] {
        migrated = true;
        return true;
    });
    ModelSettingsController controller(runtime, {.root = "/workspace"});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !controller.state().loading; }));
    EXPECT_FALSE(migrated);
    EXPECT_EQ(controller.state().problem, ModelSettingsProblem::inspection_failed);
}

TEST_F(SharedModelSettings, WorkspaceFilesNeverSupplyOrReplaceTheSharedConfiguration) {
    QTemporaryDir directory;
    QTemporaryDir first_workspace;
    QTemporaryDir second_workspace;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(first_workspace.isValid());
    ASSERT_TRUE(second_workspace.isValid());
    const auto legacy = QByteArrayLiteral(
        R"({"endpoint":"https://legacy.example.test/v1/chat/completions","model":"workspace-model","api_key":"workspace-secret"})");
    ASSERT_TRUE(write_fixture(first_workspace.filePath("config.json"), legacy));
    ASSERT_TRUE(write_fixture(second_workspace.filePath("config.json"), legacy));
    const auto state = state_path(directory) / "state";
    mint::ModelConfigService service(state);
    SharedModelSettingsRuntime runtime(state);
    ModelSettingsController controller(runtime, {.root = state_path(first_workspace)});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().provider.has_value(); }));
    EXPECT_FALSE(controller.state().provider->configured);
    EXPECT_FALSE(std::filesystem::exists(service.config_path()));
    EXPECT_FALSE(std::filesystem::exists(service.credentials_path()));

    const auto saved =
        service.update({.base_url = "https://shared.example.test/v1", .model = "shared-model"});
    controller.set_root(state_path(second_workspace));
    controller.refresh();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !controller.state().loading; }));
    EXPECT_EQ(controller.state().provider->model, "shared-model");
    EXPECT_EQ(controller.state().provider->revision, saved.revision);
    EXPECT_EQ(read_fixture(first_workspace.filePath("config.json")), legacy);
    EXPECT_EQ(read_fixture(second_workspace.filePath("config.json")), legacy);

    ASSERT_TRUE(std::filesystem::remove(service.config_path()));
    controller.refresh();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !controller.state().loading; }));
    EXPECT_FALSE(controller.state().provider->configured);
    EXPECT_FALSE(std::filesystem::exists(service.config_path()));
    EXPECT_FALSE(std::filesystem::exists(service.credentials_path()));
}

TEST_F(SharedModelSettings, MissingLegacySourceRequiresAnExplicitChoiceToKeepSharedSettings) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto state = state_path(directory) / "state";
    mint::ModelConfigService service(state);
    const auto saved = service.update({.base_url = "https://shared.example.test/v1",
                                       .model = "shared-model",
                                       .api_key = "shared-fixture-secret"});
    int retired = 0;
    SharedModelSettingsRuntime runtime(state, state_path(directory) / "missing-legacy.json");
    runtime.set_legacy_imported_callback([&] {
        ++retired;
        return true;
    });
    ModelSettingsController controller(runtime, {.root = "/workspace"});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().provider.has_value(); }));
    EXPECT_EQ(retired, 0);
    ASSERT_TRUE(controller.state().legacy_conflict.has_value());
    EXPECT_FALSE(controller.state().legacy_conflict->legacy.has_value());
    EXPECT_FALSE(controller.state().can_start_task());
    EXPECT_FALSE(
        controller.resolve_legacy_conflict(mint::desktop::LegacyModelChoice::import_legacy));
    EXPECT_EQ(controller.state().provider->revision, saved.revision);
    EXPECT_EQ(controller.state().provider->model, "shared-model");
    EXPECT_EQ(service.load_runtime().api_key, "shared-fixture-secret");
    controller.refresh();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !controller.state().loading; }));
    EXPECT_EQ(retired, 0);
    ASSERT_TRUE(controller.resolve_legacy_conflict(mint::desktop::LegacyModelChoice::keep_shared));
    EXPECT_FALSE(controller.state().can_start_task());
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().save_generation == 1; }));
    EXPECT_EQ(retired, 1);
    EXPECT_FALSE(controller.state().legacy_conflict.has_value());
    EXPECT_TRUE(controller.state().can_start_task());
    EXPECT_EQ(service.inspect().revision, saved.revision);
    EXPECT_EQ(controller.state().problem, ModelSettingsProblem::none);
}

TEST_F(SharedModelSettings, SelectedLegacyProfileReplacesSharedOnlyAfterVersionedImport) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto state = state_path(directory) / "state";
    mint::ModelConfigService service(state);
    const auto saved = service.update({.base_url = "https://shared.example.test/v1",
                                       .model = "old-shared-model",
                                       .api_key = "shared-fixture-key"});
    const auto source = QByteArrayLiteral(
        R"({"provider":"custom","endpoint":"https://previous.example.test/v1/chat/completions","model":"selected-model","api_key":"selected-fixture-key"})");
    ASSERT_TRUE(write_fixture(directory.filePath("selected.json"), source));
    int retired = 0;
    SharedModelSettingsRuntime runtime(state, state_path(directory) / "selected.json");
    runtime.set_legacy_imported_callback([&] {
        ++retired;
        return true;
    });
    ModelSettingsController controller(runtime, {.root = "/workspace"});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().legacy_conflict.has_value(); }));
    ASSERT_TRUE(controller.state().legacy_conflict->legacy.has_value());
    EXPECT_EQ(controller.state().legacy_conflict->legacy->model, "selected-model");
    EXPECT_EQ(service.inspect().revision, saved.revision);
    EXPECT_EQ(retired, 0);
    EXPECT_FALSE(controller.save_custom_profile({.base_url = "https://other.example.test/v1",
                                                 .model = "unselected",
                                                 .api_key = "draft-key"}));
    ASSERT_TRUE(
        controller.resolve_legacy_conflict(mint::desktop::LegacyModelChoice::import_legacy));
    EXPECT_TRUE(controller.state().saving);
    EXPECT_FALSE(controller.state().can_start_task());
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().save_generation == 1; }));
    EXPECT_EQ(service.load_runtime().model, "selected-model");
    EXPECT_EQ(service.load_runtime().api_key, "selected-fixture-key");
    EXPECT_EQ(read_fixture(directory.filePath("selected.json")), source);
    EXPECT_EQ(retired, 1);
    EXPECT_TRUE(controller.state().can_start_task());
}

TEST_F(SharedModelSettings, ConcurrentSharedChangeKeepsMigrationPendingUntilReloadAndChoice) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto state = state_path(directory) / "state";
    mint::ModelConfigService service(state);
    const auto initial =
        service.update({.base_url = "https://shared.example.test/v1", .model = "initial"});
    ASSERT_TRUE(write_fixture(
        directory.filePath("legacy.json"),
        QByteArrayLiteral(
            R"({"endpoint":"https://legacy.example.test/v1/chat/completions","model":"legacy"})")));
    int retired = 0;
    SharedModelSettingsRuntime runtime(state, state_path(directory) / "legacy.json");
    runtime.set_legacy_imported_callback([&] {
        ++retired;
        return true;
    });
    ModelSettingsController controller(runtime, {.root = "/workspace"});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().legacy_conflict.has_value(); }));
    const auto changed = service.update({.expected_revision = initial.revision,
                                         .base_url = "https://shared.example.test/v1",
                                         .model = "external"});
    ASSERT_TRUE(
        controller.resolve_legacy_conflict(mint::desktop::LegacyModelChoice::import_legacy));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !controller.state().saving; }));
    EXPECT_EQ(service.inspect().revision, changed.revision);
    EXPECT_EQ(retired, 0);
    EXPECT_FALSE(controller.state().can_start_task());
    controller.refresh();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !controller.state().loading; }));
    ASSERT_TRUE(controller.state().legacy_conflict.has_value());
    EXPECT_EQ(controller.state().legacy_conflict->shared.model, "external");
    ASSERT_TRUE(controller.resolve_legacy_conflict(mint::desktop::LegacyModelChoice::keep_shared));
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().save_generation == 1; }));
    EXPECT_EQ(service.inspect().revision, changed.revision);
    EXPECT_EQ(retired, 1);
}

TEST_F(SharedModelSettings, FailedLegacyPointerCleanupDoesNotUnblockExecution) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto state = state_path(directory) / "state";
    mint::ModelConfigService service(state);
    (void)service.update({.base_url = "https://shared.example.test/v1", .model = "shared"});
    bool allow_retirement = false;
    SharedModelSettingsRuntime runtime(state, state_path(directory) / "missing.json");
    runtime.set_legacy_imported_callback([&] { return allow_retirement; });
    ModelSettingsController controller(runtime, {.root = "/workspace"});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().legacy_conflict.has_value(); }));
    ASSERT_TRUE(controller.resolve_legacy_conflict(mint::desktop::LegacyModelChoice::keep_shared));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !controller.state().saving; }));
    EXPECT_FALSE(controller.state().can_start_task());
    EXPECT_EQ(controller.state().save_generation, 0U);
    controller.refresh();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !controller.state().loading; }));
    ASSERT_TRUE(controller.state().legacy_conflict.has_value());
    allow_retirement = true;
    ASSERT_TRUE(controller.resolve_legacy_conflict(mint::desktop::LegacyModelChoice::keep_shared));
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().save_generation == 1; }));
    EXPECT_TRUE(controller.state().can_start_task());
}

TEST_F(SharedModelSettings, ExternalUpdatesRefreshThroughTheDirectoryWatchAfterAtomicRename) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    mint::ModelConfigService service(state_path(directory));
    auto snapshot =
        service.update({.base_url = "https://fixture.example.test/v1", .model = "before"});
    SharedModelSettingsRuntime runtime(state_path(directory));
    ModelSettingsController controller(runtime, {.root = "/workspace"});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().provider.has_value(); }));
    snapshot = service.update({.expected_revision = snapshot.revision,
                               .base_url = "https://fixture.example.test/v1",
                               .model = "after"});
    ASSERT_TRUE(
        QTest::qWaitFor([&] { return controller.state().provider->model == "after"; }, 5000));
    snapshot = service.update({.expected_revision = snapshot.revision,
                               .base_url = "https://fixture.example.test/v1",
                               .model = "after-second-rename"});
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return controller.state().provider->model == "after-second-rename"; }, 5000));
    EXPECT_EQ(controller.state().provider->revision, snapshot.revision);
}

TEST_F(SharedModelSettings, NativeDirectoryNotificationsRefreshRepeatedAtomicRenames) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    mint::ModelConfigService service(state_path(directory));
    auto snapshot =
        service.update({.base_url = "https://fixture.example.test/v1", .model = "before"});
    QFileSystemWatcher native_probe;
    if (!native_probe.addPath(QFileInfo(directory.path()).canonicalFilePath())) {
        GTEST_SKIP()
            << "Native directory notifications are unavailable in this execution environment.";
    }
    QSignalSpy native_changes(&native_probe, &QFileSystemWatcher::directoryChanged);
    SharedModelSettingsRuntime runtime(state_path(directory));
    ModelSettingsController controller(runtime, {.root = "/workspace"});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().provider.has_value(); }));
    for (const auto* model : {"first-rename", "second-rename"}) {
        const auto previous_notifications = native_changes.size();
        snapshot = service.update({.expected_revision = snapshot.revision,
                                   .base_url = "https://fixture.example.test/v1",
                                   .model = model});
        ASSERT_TRUE(
            QTest::qWaitFor([&] { return native_changes.size() > previous_notifications; }, 5000));
        ASSERT_TRUE(
            QTest::qWaitFor([&] { return controller.state().provider->model == model; }, 5000));
        EXPECT_EQ(controller.state().provider->revision, snapshot.revision);
    }
}

TEST_F(SharedModelSettings, SaveBetweenInitialReadAndWatchInstallationIsRechecked) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    mint::ModelConfigService service(state_path(directory));
    const auto first =
        service.update({.base_url = "https://fixture.example.test/v1", .model = "first"});
    SharedModelSettingsRuntime runtime(state_path(directory));
    ModelSettingsController controller(runtime, {.root = "/workspace"});
    controller.activate();
    // EN: Finish the worker without delivering its Qt completion, then save before the watch
    // exists. ZH-CN: 完成工作线程但不分发 Qt 回调，在监听安装前写入新配置。
    QThreadPool::globalInstance()->waitForDone();
    ASSERT_FALSE(controller.state().provider.has_value());
    (void)service.update({.expected_revision = first.revision,
                          .base_url = "https://fixture.example.test/v1",
                          .model = "between"});
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            return controller.state().provider && controller.state().provider->model == "between";
        },
        5000));
}

TEST_F(SharedModelSettings, InitialInspectionErrorRecoversAfterAnExternalAtomicRepair) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    mint::ModelConfigService service(state_path(directory));
    (void)service.update({.base_url = "https://fixture.example.test/v1", .model = "repaired"});
    const auto path = directory.filePath("model.json");
    const auto valid_document = read_fixture(path);
    ASSERT_TRUE(write_fixture(path, QByteArrayLiteral("{invalid")));
    SharedModelSettingsRuntime runtime(state_path(directory));
    ModelSettingsController controller(runtime, {.root = "/workspace"});
    int failed_inspections = 0;
    controller.set_listener([&] {
        if (controller.state().problem == ModelSettingsProblem::inspection_failed) {
            ++failed_inspections;
        }
    });
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return failed_inspections >= 2; }, 5000));
    QSaveFile repaired(path);
    ASSERT_TRUE(repaired.open(QIODevice::WriteOnly));
    ASSERT_TRUE(repaired.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    ASSERT_EQ(repaired.write(valid_document), valid_document.size());
    ASSERT_TRUE(repaired.commit());
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            return controller.state().provider && controller.state().provider->model == "repaired";
        },
        5000));
    EXPECT_EQ(controller.state().problem, ModelSettingsProblem::none);
}

TEST_F(SharedModelSettings, StaleRevisionRejectsSavingWithoutOverwritingExternalChanges) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    mint::ModelConfigService service(state_path(directory));
    const auto initial =
        service.update({.base_url = "https://fixture.example.test/v1", .model = "original"});
    SharedModelSettingsRuntime runtime(state_path(directory));
    ModelSettingsController controller(runtime, {.root = "/workspace"});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().provider.has_value(); }));
    (void)service.update({.expected_revision = initial.revision,
                          .base_url = "https://fixture.example.test/v1",
                          .model = "external"});
    ASSERT_TRUE(controller.save_custom_profile({.expected_revision = initial.revision,
                                                .base_url = "https://fixture.example.test/v1",
                                                .model = "my-draft"}));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !controller.state().saving; }));
    EXPECT_EQ(controller.state().problem, ModelSettingsProblem::conflict);
    EXPECT_EQ(controller.state().save_generation, 0U);
    EXPECT_EQ(service.inspect().config.model, "external");
}

TEST_F(SharedModelSettings, LatestInspectionWinsAndCallbackTeardownIsSafe) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(write_fixture(
        directory.filePath("first.json"),
        QByteArrayLiteral(
            R"({"provider":"custom","adapter":"responses","endpoint":"https://fixture.example.test/v1/responses","model":"first"})")));
    ASSERT_TRUE(write_fixture(
        directory.filePath("last.json"),
        QByteArrayLiteral(
            R"({"provider":"custom","adapter":"responses","endpoint":"https://fixture.example.test/v1/responses","model":"last"})")));
    SharedModelSettingsRuntime runtime(state_path(directory) / "state");
    std::vector<std::string> received;
    runtime.set_model_settings_callbacks(
        {.inspected = [&](auto provider) { received.push_back(provider.model); }});
    runtime.inspect_model({.root = state_path(directory), .config = "first.json"});
    runtime.inspect_model({.root = state_path(directory), .config = "last.json"});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !received.empty(); }));
    EXPECT_EQ(received, std::vector<std::string>{"last"});
    runtime.inspect_model({.root = state_path(directory), .config = "first.json"});
    runtime.set_model_settings_callbacks({});
    QThreadPool::globalInstance()->waitForDone();
    QCoreApplication::processEvents();
    EXPECT_EQ(received.size(), 1U);
}

TEST_F(SharedModelSettings, EditingAnExplicitOverrideSavesSharedWithoutChangingTheOverride) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto source = QByteArrayLiteral(
        R"({"provider":"custom","adapter":"responses","endpoint":"https://fixture.example.test/v1/responses","model":"override-model"})");
    ASSERT_TRUE(write_fixture(directory.filePath("override.json"), source));
    SharedModelSettingsRuntime runtime(state_path(directory) / "state",
                                       state_path(directory) / "missing-legacy.json");
    ModelSettingsController controller(runtime,
                                       {.root = state_path(directory), .config = "override.json"});
    controller.activate();
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().provider.has_value(); }));
    EXPECT_TRUE(controller.state().provider->is_override);
    ASSERT_TRUE(
        controller.save_custom_profile({.expected_revision = controller.state().provider->revision,
                                        .adapter = mint::desktop::CustomModelAdapter::responses,
                                        .base_url = "https://fixture.example.test/v1",
                                        .model = "shared-model"}));
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.state().save_generation == 1; }));
    EXPECT_TRUE(controller.state().selected_config.empty());
    EXPECT_FALSE(controller.state().provider->is_override);
    EXPECT_EQ(read_fixture(directory.filePath("override.json")), source);
    mint::ModelConfigService service(state_path(directory) / "state");
    EXPECT_EQ(service.inspect().config.model, "shared-model");
}

} // namespace
