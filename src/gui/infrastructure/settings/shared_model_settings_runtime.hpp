#pragma once

#include "application/model_settings_runtime.hpp"

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

namespace mint::infrastructure::desktop {

// EN: The sole Qt adapter delegates persistence to ModelConfigService; workers never capture this.
// ZH-CN: 唯一 Qt 适配器将持久化交给 ModelConfigService，工作线程不会捕获自身对象。
class SharedModelSettingsRuntime final : public QObject,
                                         public mint::desktop::ModelSettingsRuntime {
  public:
    explicit SharedModelSettingsRuntime(std::filesystem::path state_directory = {},
                                        std::filesystem::path legacy_config = {},
                                        QObject* parent = nullptr);
    void set_model_settings_callbacks(mint::desktop::ModelSettingsCallbacks callbacks) override;
    void inspect_model(mint::desktop::ModelInspectionRequest request) override;
    void update_model(mint::desktop::ModelUpdateRequest request) override;
    void resolve_legacy_conflict(mint::desktop::LegacyModelResolution request) override;
    void set_legacy_imported_callback(std::function<bool()> callback);

  private:
    void start_pending();
    void watch_directory(const std::filesystem::path& config_path);
    std::filesystem::path state_directory_;
    std::filesystem::path legacy_config_;
    mint::desktop::ModelSettingsCallbacks callbacks_;
    std::function<bool()> legacy_imported_callback_;
    std::optional<mint::desktop::ModelInspectionRequest> pending_inspection_;
    std::optional<mint::desktop::ModelUpdateRequest> pending_update_;
    std::optional<mint::desktop::LegacyModelResolution> pending_resolution_;
    QFileSystemWatcher watcher_;
    QString registered_directory_;
    QTimer debounce_;
    QTimer watch_retry_;
    std::size_t generation_ = 0;
    bool busy_ = false;
};

} // namespace mint::infrastructure::desktop
