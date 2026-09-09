#include "qsettings_task_organization_repository.hpp"

#include <QCryptographicHash>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace mint::infrastructure::desktop {
namespace {

QString storage_key(std::string_view project_scope, const QString& value) {
    const QByteArray scope(project_scope.data(), static_cast<qsizetype>(project_scope.size()));
    const auto fingerprint = QCryptographicHash::hash(scope, QCryptographicHash::Sha256).toHex();
    return QStringLiteral("task-organization/v1/%1/%2")
        .arg(QString::fromLatin1(fingerprint), value);
}

std::unordered_set<std::string> task_ids(const QStringList& values) {
    std::unordered_set<std::string> ids;
    ids.reserve(static_cast<std::size_t>(values.size()));
    for (const auto& value : values) {
        ids.insert(value.toStdString());
    }
    return ids;
}

QStringList sorted_task_ids(const std::unordered_set<std::string>& ids) {
    QStringList values;
    values.reserve(static_cast<qsizetype>(ids.size()));
    for (const auto& id : ids) {
        values.push_back(QString::fromStdString(id));
    }
    std::sort(values.begin(), values.end());
    return values;
}

} // namespace

QSettingsTaskOrganizationRepository::QSettingsTaskOrganizationRepository()
    : settings_(std::make_unique<QSettings>()) {
    // EN: Pinned and archived tasks must not leak across settings domains.
    // ZH-CN: 置顶与归档任务不得跨状态域读取。
    settings_->setFallbacksEnabled(false);
}

QSettingsTaskOrganizationRepository::QSettingsTaskOrganizationRepository(const QString& file_name)
    : settings_(std::make_unique<QSettings>(file_name, QSettings::IniFormat)) {
    settings_->setFallbacksEnabled(false);
}

QSettingsTaskOrganizationRepository::~QSettingsTaskOrganizationRepository() = default;

std::optional<mint::desktop::TaskOrganization>
QSettingsTaskOrganizationRepository::load(std::string_view project_scope) const {
    if (settings_->status() != QSettings::NoError) {
        return std::nullopt;
    }
    return mint::desktop::TaskOrganization{
        .pinned_task_ids = task_ids(
            settings_->value(storage_key(project_scope, QStringLiteral("pinned"))).toStringList()),
        .archived_task_ids =
            task_ids(settings_->value(storage_key(project_scope, QStringLiteral("archived")))
                         .toStringList()),
    };
}

bool QSettingsTaskOrganizationRepository::save(
    std::string_view project_scope, const mint::desktop::TaskOrganization& organization) {
    settings_->setValue(storage_key(project_scope, QStringLiteral("pinned")),
                        sorted_task_ids(organization.pinned_task_ids));
    settings_->setValue(storage_key(project_scope, QStringLiteral("archived")),
                        sorted_task_ids(organization.archived_task_ids));
    settings_->sync();
    return settings_->status() == QSettings::NoError;
}

} // namespace mint::infrastructure::desktop
