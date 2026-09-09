#pragma once

#include "application/workspaces/workspace_catalog.hpp"

#include <memory>

class QSettings;
class QString;

namespace mint::infrastructure::desktop {

class QSettingsWorkspaceCatalogRepository final : public mint::desktop::WorkspaceCatalogRepository {
  public:
    QSettingsWorkspaceCatalogRepository();
    explicit QSettingsWorkspaceCatalogRepository(const QString& file_name);
    ~QSettingsWorkspaceCatalogRepository() override;

    [[nodiscard]] std::optional<mint::desktop::WorkspaceCatalogState> load() const override;
    [[nodiscard]] bool save(const mint::desktop::WorkspaceCatalogState& catalog) override;

  private:
    std::unique_ptr<QSettings> settings_;
};

} // namespace mint::infrastructure::desktop
