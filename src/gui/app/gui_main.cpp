#include "gui_options.hpp"

#include "application/model_settings_controller.hpp"
#include "application/preferences/preferences_controller.hpp"
#include "application/task_workspace_controller.hpp"
#include "infrastructure/cli/mint_cli_process.hpp"
#include "infrastructure/settings/qsettings_preferences_repository.hpp"
#include "infrastructure/settings/qsettings_task_organization_repository.hpp"
#include "infrastructure/settings/qsettings_workspace_catalog_repository.hpp"
#include "infrastructure/settings/shared_model_settings_runtime.hpp"
#include "support/qt_path.hpp"
#include "ui/components/conversation_transcript.hpp"
#include "ui/shell/mint_window.hpp"
#include "ui/support/gui_resources.hpp"
#include "ui/support/localized_text.hpp"
#include "ui/theme/mint_theme.hpp"

#include "mint/infrastructure/model_config_service.hpp"
#include "mint/infrastructure/project_store.hpp"
#include "mint/localization/localization.hpp"

#include <FluentQt/Diagnostics.h>
#include <FluentQt/FluentQt.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScrollBar>
#include <QSettings>
#include <QSize>
#include <QTimer>

#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    auto parsed_options = mint::desktop::parse_gui_options(argc, argv);
    if (!parsed_options) {
        qWarning("Mint: --root, --state-dir and --config require a nonempty path.");
        return 2;
    }
    auto options = std::move(*parsed_options);
    std::optional<mint::localization::Language> language_override;
    if (!options.language.empty() && mint::localization::set_language(options.language)) {
        language_override = mint::localization::current_language();
    }
    const auto theme_override = mint::desktop::theme_mode_from_code(options.theme);

    fluent::prepareHighDpiApplication();
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("mint"));
    QCoreApplication::setApplicationName(QStringLiteral("Mint"));
    std::filesystem::path effective_state_root;
    try {
        // EN: Resolve invocation-relative paths before subprocesses switch to a workspace
        // directory. ZH-CN: 在子进程切换工作区目录前固定本次启动的相对路径。
        mint::desktop::resolve_gui_invocation_paths(options);
        effective_state_root =
            options.state_root.empty() ? mint::default_mint_state_directory() : options.state_root;
    } catch (const std::exception&) {
        qWarning("Mint: the selected workspace or settings path is unavailable.");
        return 2;
    }
    if (!options.state_root.empty()) {
        try {
            // EN: Establish the private root before QSettings can create public parent directories.
            // ZH-CN: 在 QSettings 创建默认权限的父目录前建立私有状态目录。
            mint::ModelConfigService(options.state_root).prepare_storage();
        } catch (const mint::ModelConfigError&) {
            qWarning("Mint: the settings directory is unavailable or not private.");
            return 2;
        }
        const auto settings_root = options.state_root / "gui-settings";
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           mint::gui::qstring_from_path(settings_root));
    }
    mint::infrastructure::desktop::QSettingsPreferencesRepository preferences_repository;
    mint::infrastructure::desktop::QSettingsTaskOrganizationRepository task_organization_repository;
    mint::infrastructure::desktop::QSettingsWorkspaceCatalogRepository workspace_catalog_repository;
    mint::desktop::PreferencesController preferences(preferences_repository);
    preferences.initialize(language_override, theme_override);
    fluent::initializeResources();
    mint::ui::initialize_gui_resources();
    mint::ui::theme::install(preferences.preferences().theme);
    application.setFont(Typography::fontStyle(Typography::FontRole::Body).toQFont());
    application.setWindowIcon(QIcon(QStringLiteral(":/mint/branding/mint-arctic-fox-tile.svg")));

    if (options.mint_cli.empty()) {
        const auto sibling = QFileInfo(
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("mint")));
        options.mint_cli = mint::gui::path_from_qstring(sibling.absoluteFilePath());
    }

    mint::infrastructure::desktop::MintCliProcess process(std::move(options.mint_cli),
                                                          options.state_root);
    const auto language =
        std::string(mint::localization::language_code(preferences.preferences().language));
    const auto legacy_model_config = preferences_repository.legacy_model_config();
    mint::infrastructure::desktop::SharedModelSettingsRuntime model_runtime(options.state_root,
                                                                            legacy_model_config);
    model_runtime.set_legacy_imported_callback([&preferences_repository, legacy_model_config] {
        return preferences_repository.clear_legacy_model_config(legacy_model_config);
    });
    mint::desktop::TaskWorkspaceController controller(
        process, {.root = std::move(options.root),
                  .config = options.config,
                  .language = language,
                  .demo = options.demo,
                  .task_organization_repository = &task_organization_repository,
                  .workspace_catalog_repository = &workspace_catalog_repository,
                  .state_root = effective_state_root});
    mint::desktop::ModelSettingsController model_settings(model_runtime,
                                                          {.root = controller.state().root,
                                                           .config = std::move(options.config),
                                                           .language = language});
    mint::ui::MintWindow window(controller, model_settings, preferences);
    if (options.window_size.has_value()) {
        window.resize(*options.window_size);
    }
    if (!options.screenshot.empty()) {
        window.setBackdropEffect(fluent::windowing::BackdropEffect::Solid);
        auto capture_palette = window.palette();
        capture_palette.setColor(QPalette::Window, mint::ui::theme::palette().chrome);
        window.setPalette(capture_palette);
        window.setAutoFillBackground(true);
        window.setAttribute(Qt::WA_DontShowOnScreen);
    }
    window.show();
    if (!options.quality_report.empty()) {
        QTimer::singleShot(2100, &window, [&window, report_path = options.quality_report] {
            const auto report = QJsonDocument(fluent::diagnostics::Inspector::report(&window));
            const auto bytes = report.toJson();
            QSaveFile file(mint::gui::qstring_from_path(report_path));
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
                !file.commit()) {
                qWarning("Mint: could not save the GUI quality report");
            }
        });
    }
    if (options.screenshot.empty()) {
        window.raise();
        window.activateWindow();
    } else {
        const auto screenshot = options.screenshot;
        const auto screenshot_panel = options.screenshot_panel;
        QTimer::singleShot(1700, &window, [&window, screenshot_panel] {
            const auto drawers = window.findChildren<fluent::collections::DrawerView*>();
            for (auto* drawer : drawers) {
                drawer->setAnimationEnabled(false);
            }
            if (screenshot_panel == QStringLiteral("evidence")) {
                if (auto* transcript = window.findChild<mint::ui::ConversationTranscript*>(
                        QStringLiteral("MintConversationTranscript"));
                    transcript != nullptr) {
                    transcript->select_first_evidence();
                }
            } else if (screenshot_panel == QStringLiteral("settings")) {
                if (auto* button =
                        window.findChild<QAbstractButton*>(QStringLiteral("MintSettingsButton"));
                    button != nullptr) {
                    button->click();
                }
            } else if (screenshot_panel == QStringLiteral("scroll-away")) {
                if (auto* transcript = window.findChild<mint::ui::ConversationTranscript*>(
                        QStringLiteral("MintConversationTranscript"));
                    transcript != nullptr) {
                    auto* scrollbar = transcript->verticalScrollBar();
                    scrollbar->setValue(scrollbar->maximum() / 2);
                }
            }
        });
        QTimer::singleShot(2200, &window, [&application, &window, screenshot] {
            const auto saved = window.grab().save(mint::gui::qstring_from_path(screenshot));
            application.exit(saved ? 0 : 2);
        });
    }
    return application.exec();
}
