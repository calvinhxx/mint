#include "application/preferences/preferences_controller.hpp"
#include "infrastructure/settings/qsettings_preferences_repository.hpp"
#include "infrastructure/settings/qsettings_task_organization_repository.hpp"
#include "infrastructure/settings/qsettings_workspace_catalog_repository.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>

#include <gtest/gtest.h>

namespace {

QString ini_scope_path(QSettings::Scope scope) {
    QSettings probe(QSettings::IniFormat, scope, QStringLiteral("MintSettingsPathProbe"),
                    QStringLiteral("Path"));
    // EN: Read the configured prefix without requiring the system directory to exist.
    // ZH-CN: 读取已配置的路径前缀，不要求系统目录实际存在。
    return QFileInfo(QFileInfo(probe.fileName()).absolutePath()).absolutePath();
}

class ScopedSettingsDomain final {
  public:
    explicit ScopedSettingsDomain(const QString& directory)
        : format_(QSettings::defaultFormat()), organization_(QCoreApplication::organizationName()),
          domain_(QCoreApplication::organizationDomain()),
          application_(QCoreApplication::applicationName()),
          user_path_(ini_scope_path(QSettings::UserScope)),
          system_path_(ini_scope_path(QSettings::SystemScope)) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory + "/user");
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, directory + "/system");
        QCoreApplication::setOrganizationName(
            QStringLiteral("MintSettingsIsolation-%1")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QCoreApplication::setOrganizationDomain({});
        QCoreApplication::setApplicationName(QStringLiteral("App"));
    }

    ~ScopedSettingsDomain() {
        QSettings::setDefaultFormat(format_);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, user_path_);
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, system_path_);
        QCoreApplication::setOrganizationName(organization_);
        QCoreApplication::setOrganizationDomain(domain_);
        QCoreApplication::setApplicationName(application_);
    }

  private:
    QSettings::Format format_;
    QString organization_;
    QString domain_;
    QString application_;
    QString user_path_;
    QString system_path_;
};

TEST(DesktopPreferences, QSettingsRepositoryPersistsAppearanceAndLanguage) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto settings_path = directory.filePath(QStringLiteral("preferences.ini"));

    {
        mint::infrastructure::desktop::QSettingsPreferencesRepository repository(settings_path);
        ASSERT_TRUE(repository.save({.language = mint::localization::Language::simplified_chinese,
                                     .theme = mint::desktop::ThemeMode::light}));
    }

    mint::infrastructure::desktop::QSettingsPreferencesRepository repository(settings_path);
    const auto preferences = repository.load();
    ASSERT_TRUE(preferences.has_value());
    EXPECT_EQ(preferences->language, mint::localization::Language::simplified_chinese);
    EXPECT_EQ(preferences->theme, mint::desktop::ThemeMode::light);
}

TEST(DesktopPreferences, LegacyModelPointerIsOnlyReadForMigration) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("legacy.ini"));
    {
        QSettings legacy(path, QSettings::IniFormat);
        legacy.setValue(QStringLiteral("model/config"), QStringLiteral("/old/profile.json"));
    }
    mint::infrastructure::desktop::QSettingsPreferencesRepository repository(path);
    EXPECT_EQ(repository.legacy_model_config(), "/old/profile.json");
    ASSERT_TRUE(repository.save({.language = mint::localization::Language::english}));
    EXPECT_EQ(repository.legacy_model_config(), "/old/profile.json");
    ASSERT_TRUE(repository.clear_legacy_model_config("/old/profile.json"));
    EXPECT_TRUE(repository.legacy_model_config().empty());
    EXPECT_TRUE(repository.clear_legacy_model_config("/old/profile.json"));
}

TEST(DesktopPreferences, LegacyPointerCleanupPreservesASelectionChangedByAnotherInstance) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("legacy.ini"));
    QSettings older_gui(path, QSettings::IniFormat);
    older_gui.setValue(QStringLiteral("model/config"), QStringLiteral("/old/profile.json"));
    older_gui.sync();
    mint::infrastructure::desktop::QSettingsPreferencesRepository repository(path);
    const auto selected_at_startup = repository.legacy_model_config();
    older_gui.setValue(QStringLiteral("model/config"), QStringLiteral("/new/profile.json"));
    older_gui.sync();
    EXPECT_FALSE(repository.clear_legacy_model_config(selected_at_startup));
    EXPECT_EQ(repository.legacy_model_config(), "/new/profile.json");
    EXPECT_FALSE(repository.clear_legacy_model_config({}));
    EXPECT_EQ(repository.legacy_model_config(), "/new/profile.json");
    ASSERT_TRUE(repository.clear_legacy_model_config("/new/profile.json"));
    EXPECT_TRUE(repository.legacy_model_config().empty());
}

TEST(DesktopPreferences, ExistingLanguagePreferenceDefaultsToSystemTheme) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto settings_path = directory.filePath(QStringLiteral("preferences.ini"));
    {
        QSettings settings(settings_path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("interface/language"), QStringLiteral("en"));
    }

    mint::infrastructure::desktop::QSettingsPreferencesRepository repository(settings_path);
    const auto preferences = repository.load();
    ASSERT_TRUE(preferences.has_value());
    EXPECT_EQ(preferences->theme, mint::desktop::ThemeMode::system);
}

TEST(DesktopPreferences, QSettingsRepositoryPersistsWorkspaceCatalogOrder) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto settings_path = directory.filePath(QStringLiteral("workspaces.ini"));
    const mint::desktop::WorkspaceCatalogState expected{
        .roots = {"/projects/alpha", "/projects/beta"},
        .task_counts = {{.root = "/projects/alpha", .unarchived = 3, .archived = 1},
                        {.root = "/projects/beta", .archived = 2}},
        .last_active_root = "/projects/alpha"};
    {
        mint::infrastructure::desktop::QSettingsWorkspaceCatalogRepository repository(
            settings_path);
        ASSERT_TRUE(repository.save(expected));
    }

    mint::infrastructure::desktop::QSettingsWorkspaceCatalogRepository repository(settings_path);
    const auto restored = repository.load();
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(*restored, expected);
}

TEST(DesktopPreferences, LegacyWorkspaceCatalogHasNoFabricatedActiveRoot) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto settings_path = directory.filePath(QStringLiteral("legacy-workspaces.ini"));
    {
        QSettings settings(settings_path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("workspaces/v1/roots"),
                          QStringList{QStringLiteral("/projects/alpha"), QStringLiteral(".")});
    }
    mint::infrastructure::desktop::QSettingsWorkspaceCatalogRepository repository(settings_path);
    const auto restored = repository.load();
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->roots.size(), 2U);
    EXPECT_TRUE(restored->last_active_root.empty());
}

TEST(DesktopPreferences, WorkspaceCatalogDoesNotReadAnotherSettingsFile) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    mint::infrastructure::desktop::QSettingsWorkspaceCatalogRepository first(
        directory.filePath(QStringLiteral("first.ini")));
    mint::infrastructure::desktop::QSettingsWorkspaceCatalogRepository second(
        directory.filePath(QStringLiteral("second.ini")));
    ASSERT_TRUE(first.save({.roots = {"/projects/alpha"}, .last_active_root = "/projects/alpha"}));
    const auto isolated = second.load();
    ASSERT_TRUE(isolated.has_value());
    EXPECT_TRUE(isolated->roots.empty());
    EXPECT_TRUE(isolated->last_active_root.empty());
}

TEST(DesktopPreferences, DefaultRepositoriesNeverInheritOrganizationOrSystemSettings) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto original_format = QSettings::defaultFormat();
    const auto original_organization = QCoreApplication::organizationName();
    const auto original_domain = QCoreApplication::organizationDomain();
    const auto original_application = QCoreApplication::applicationName();
    const auto original_user_path = ini_scope_path(QSettings::UserScope);
    const auto original_system_path = ini_scope_path(QSettings::SystemScope);
    struct ForeignScope {
        QSettings::Scope scope;
        bool application;
        const char* name;
    };
    for (const auto foreign_scope :
         {ForeignScope{QSettings::UserScope, false, "user-organization"},
          ForeignScope{QSettings::SystemScope, true, "system-application"},
          ForeignScope{QSettings::SystemScope, false, "system-organization"}}) {
        SCOPED_TRACE(foreign_scope.name);
        {
            ScopedSettingsDomain restore(
                directory.filePath(QString::fromLatin1(foreign_scope.name)));
            QSettings foreign(
                QSettings::IniFormat, foreign_scope.scope, QCoreApplication::organizationName(),
                foreign_scope.application ? QCoreApplication::applicationName() : QString{});
            foreign.setFallbacksEnabled(false);
            foreign.setValue(QStringLiteral("interface/language"), QStringLiteral("zh-CN"));
            foreign.setValue(QStringLiteral("appearance/theme"), QStringLiteral("dark"));
            foreign.setValue(QStringLiteral("model/config"),
                             QStringLiteral("/foreign/profile.json"));
            foreign.sync();
            mint::infrastructure::desktop::QSettingsWorkspaceCatalogRepository foreign_catalog(
                foreign.fileName());
            ASSERT_TRUE(foreign_catalog.save(
                {.roots = {"/foreign/project"},
                 .task_counts = {{.root = "/foreign/project", .unarchived = 2}},
                 .last_active_root = "/foreign/project"}));
            mint::infrastructure::desktop::QSettingsTaskOrganizationRepository foreign_tasks(
                foreign.fileName());
            ASSERT_TRUE(
                foreign_tasks.save("/foreign/project", {.pinned_task_ids = {"foreign-pin"},
                                                        .archived_task_ids = {"foreign-archive"}}));

            QSettings inherited;
            ASSERT_EQ(inherited.value(QStringLiteral("workspaces/v1/roots")).toStringList(),
                      QStringList{QStringLiteral("/foreign/project")});
            ASSERT_EQ(inherited.value(QStringLiteral("model/config")).toString(),
                      QStringLiteral("/foreign/profile.json"));
            mint::infrastructure::desktop::QSettingsWorkspaceCatalogRepository catalog;
            const auto isolated = catalog.load();
            ASSERT_TRUE(isolated.has_value());
            EXPECT_TRUE(isolated->roots.empty());
            EXPECT_TRUE(isolated->task_counts.empty());
            EXPECT_TRUE(isolated->last_active_root.empty());

            mint::infrastructure::desktop::QSettingsPreferencesRepository preferences;
            EXPECT_FALSE(preferences.load().has_value());
            EXPECT_TRUE(preferences.legacy_model_config().empty());
            mint::infrastructure::desktop::QSettingsTaskOrganizationRepository tasks;
            const auto organization = tasks.load("/foreign/project");
            ASSERT_TRUE(organization.has_value());
            EXPECT_TRUE(organization->pinned_task_ids.empty());
            EXPECT_TRUE(organization->archived_task_ids.empty());

            QSettings local;
            local.setFallbacksEnabled(false);
            local.setValue(QStringLiteral("interface/language"), QStringLiteral("en"));
            local.sync();
            const auto own_preferences = preferences.load();
            ASSERT_TRUE(own_preferences.has_value());
            EXPECT_EQ(own_preferences->language, mint::localization::Language::english);
            EXPECT_EQ(own_preferences->theme, mint::desktop::ThemeMode::system);
            ASSERT_TRUE(
                catalog.save({.roots = {"/own/project"}, .last_active_root = "/own/project"}));
            const auto own_catalog = catalog.load();
            ASSERT_TRUE(own_catalog.has_value());
            EXPECT_EQ(own_catalog->roots, (std::vector<std::filesystem::path>{"/own/project"}));
            const auto retained_foreign_catalog = foreign_catalog.load();
            ASSERT_TRUE(retained_foreign_catalog.has_value());
            EXPECT_EQ(retained_foreign_catalog->last_active_root,
                      std::filesystem::path("/foreign/project"));
            EXPECT_EQ(foreign.value(QStringLiteral("model/config")).toString(),
                      QStringLiteral("/foreign/profile.json"));
        }
        EXPECT_EQ(QSettings::defaultFormat(), original_format);
        EXPECT_EQ(QCoreApplication::organizationName(), original_organization);
        EXPECT_EQ(QCoreApplication::organizationDomain(), original_domain);
        EXPECT_EQ(QCoreApplication::applicationName(), original_application);
        EXPECT_EQ(ini_scope_path(QSettings::UserScope), original_user_path);
        EXPECT_EQ(ini_scope_path(QSettings::SystemScope), original_system_path);
    }
}

} // namespace
