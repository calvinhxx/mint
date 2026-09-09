#pragma once

#include "application/preferences/gui_preferences.hpp"

#include <FluentQt/Windowing.h>

namespace mint::desktop {
class ModelSettingsController;
class PreferencesController;
class TaskWorkspaceController;
} // namespace mint::desktop

namespace mint::ui {

class MintWindow final : public fluent::windowing::Window {
  public:
    MintWindow(desktop::TaskWorkspaceController& controller,
               desktop::ModelSettingsController& model_settings,
               desktop::PreferencesController& preferences, QWidget* parent = nullptr);
    ~MintWindow() override;

  protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void apply_theme();
    void retranslate();
    void sync_title_bar_surface();
    void schedule_chrome_exclusion_refresh();

    desktop::TaskWorkspaceController& controller_;
    desktop::ModelSettingsController& model_settings_;
    desktop::PreferencesController& preferences_;
    desktop::GuiPreferences applied_preferences_;
    class MintTitleBar* title_bar_ = nullptr;
    class TitleBarSurface* title_bar_surface_ = nullptr;
    class TaskWorkspacePage* page_ = nullptr;
    bool chrome_refresh_pending_ = false;
};

} // namespace mint::ui
