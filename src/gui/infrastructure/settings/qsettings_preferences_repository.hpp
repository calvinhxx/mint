#pragma once

#include "application/preferences/preferences_repository.hpp"

#include <memory>

class QSettings;
class QString;

namespace mint::infrastructure::desktop {

class QSettingsPreferencesRepository final : public mint::desktop::PreferencesRepository {
  public:
    QSettingsPreferencesRepository();
    explicit QSettingsPreferencesRepository(const QString& file_name);
    ~QSettingsPreferencesRepository() override;

    [[nodiscard]] std::optional<mint::desktop::GuiPreferences> load() const override;
    [[nodiscard]] bool save(const mint::desktop::GuiPreferences& preferences) override;
    [[nodiscard]] std::filesystem::path legacy_model_config() const;
    [[nodiscard]] bool clear_legacy_model_config(const std::filesystem::path& expected_config);

  private:
    std::unique_ptr<QSettings> settings_;
};

} // namespace mint::infrastructure::desktop
