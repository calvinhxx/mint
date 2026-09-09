#pragma once

#include "application/task_organization.hpp"

#include <memory>

class QSettings;
class QString;

namespace mint::infrastructure::desktop {

class QSettingsTaskOrganizationRepository final : public mint::desktop::TaskOrganizationRepository {
  public:
    QSettingsTaskOrganizationRepository();
    explicit QSettingsTaskOrganizationRepository(const QString& file_name);
    ~QSettingsTaskOrganizationRepository() override;

    [[nodiscard]] std::optional<mint::desktop::TaskOrganization>
    load(std::string_view project_scope) const override;
    [[nodiscard]] bool save(std::string_view project_scope,
                            const mint::desktop::TaskOrganization& organization) override;

  private:
    std::unique_ptr<QSettings> settings_;
};

} // namespace mint::infrastructure::desktop
