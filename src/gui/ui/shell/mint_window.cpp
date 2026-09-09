#include "mint_window.hpp"

#include "application/model_settings_controller.hpp"
#include "application/preferences/preferences_controller.hpp"
#include "application/task_workspace_controller.hpp"
#include "ui/components/mint_title_bar.hpp"
#include "ui/components/title_bar_surface.hpp"
#include "ui/pages/task_workspace_page.hpp"
#include "ui/support/localized_text.hpp"
#include "ui/theme/layout_metrics.hpp"
#include "ui/theme/mint_theme.hpp"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QIcon>
#include <QResizeEvent>
#include <QStyleHints>
#include <QTimer>

#include <utility>

namespace mint::ui {

MintWindow::MintWindow(desktop::TaskWorkspaceController& controller,
                       desktop::ModelSettingsController& model_settings,
                       desktop::PreferencesController& preferences, QWidget* parent)
    : fluent::windowing::Window(parent), controller_(controller), model_settings_(model_settings),
      preferences_(preferences), applied_preferences_(preferences.preferences()) {
    setObjectName(QStringLiteral("MintWindow"));
    setWindowIcon(QIcon(QStringLiteral(":/mint/branding/mint-arctic-fox-tile.svg")));
    setMinimumSize(720, 580);
    resize(1280, 800);
    setCustomWindowChromeEnabled(true);
    setBackdropEffect(fluent::windowing::BackdropEffect::Mica);
    titleBar()->setTitleBarHeight(metrics::title_bar_height);
    title_bar_surface_ = new TitleBarSurface(titleBar());
    title_bar_surface_->lower();
    title_bar_ = new MintTitleBar(titleBar());
    titleBar()->setContentWidget(title_bar_);
    page_ = new TaskWorkspacePage(controller_, model_settings_, preferences_, this);
    setContentWidget(page_);
    connect(title_bar_, &MintTitleBar::settings_requested, page_,
            &TaskWorkspacePage::open_settings);
    connect(title_bar_, &MintTitleBar::resume_requested, this,
            [this] { (void)controller_.resume(); });
    connect(page_, &TaskWorkspacePage::task_header_changed, this,
            [this](QString title, bool can_resume) {
                title_bar_->set_task(std::move(title), can_resume);
                schedule_chrome_exclusion_refresh();
            });
    connect(page_, &TaskWorkspacePage::navigation_surface_width_changed, this,
            [this](int) { sync_title_bar_surface(); });
    title_bar_->set_task(QString::fromStdString(controller_.state().title),
                         controller_.state().can_resume());
    QTimer::singleShot(0, this, [this] { sync_title_bar_surface(); });
    titleBar()->refreshChromeExclusions();
    preferences_.set_listener([this](const desktop::GuiPreferences& preferences) {
        const auto language_changed = preferences.language != applied_preferences_.language;
        const auto theme_changed = preferences.theme != applied_preferences_.theme;
        applied_preferences_ = preferences;
        if (theme_changed) {
            apply_theme();
        }
        if (language_changed) {
            controller_.set_language(
                std::string(localization::language_code(preferences.language)));
            model_settings_.set_language(
                std::string(localization::language_code(preferences.language)));
            retranslate();
        }
    });
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme) {
                if (preferences_.preferences().theme == desktop::ThemeMode::system) {
                    apply_theme();
                }
            });
    retranslate();
}

MintWindow::~MintWindow() {
    preferences_.set_listener({});
}

void MintWindow::closeEvent(QCloseEvent* event) {
    controller_.cancel();
    fluent::windowing::Window::closeEvent(event);
}

void MintWindow::resizeEvent(QResizeEvent* event) {
    fluent::windowing::Window::resizeEvent(event);
    QTimer::singleShot(0, this, [this] { sync_title_bar_surface(); });
}

void MintWindow::sync_title_bar_surface() {
    if (page_ == nullptr || title_bar_surface_ == nullptr) {
        return;
    }
    const auto boundary =
        titleBar()
            ->mapFromGlobal(page_->mapToGlobal(QPoint(page_->navigation_surface_width(), 0)))
            .x();
    const auto start = qBound(0, boundary, titleBar()->width());
    title_bar_surface_->setGeometry(start, 0, titleBar()->width() - start, titleBar()->height());
    title_bar_surface_->lower();
    title_bar_->set_navigation_boundary(start);
    schedule_chrome_exclusion_refresh();
}

void MintWindow::schedule_chrome_exclusion_refresh() {
    titleBar()->refreshChromeExclusions();
    if (chrome_refresh_pending_) {
        return;
    }
    chrome_refresh_pending_ = true;
    QTimer::singleShot(0, this, [this] {
        chrome_refresh_pending_ = false;
        titleBar()->refreshChromeExclusions();
    });
}

void MintWindow::apply_theme() {
    theme::apply(preferences_.preferences().theme);
    page_->refresh_theme();
    title_bar_surface_->update();
    title_bar_->update();
    update();
}

void MintWindow::retranslate() {
    setWindowTitle(text(localization::Message::gui_app_name));
    setCaptionButtonToolTips(text(localization::Message::gui_window_minimize),
                             text(localization::Message::gui_window_maximize),
                             text(localization::Message::gui_window_close),
                             text(localization::Message::gui_window_restore));
    setCaptionButtonAccessibleNames(text(localization::Message::gui_window_minimize),
                                    text(localization::Message::gui_window_maximize),
                                    text(localization::Message::gui_window_close),
                                    text(localization::Message::gui_window_restore));
    page_->retranslate();
    title_bar_->retranslate();
    schedule_chrome_exclusion_refresh();
}

} // namespace mint::ui
