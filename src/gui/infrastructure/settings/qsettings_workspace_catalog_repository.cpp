#include "qsettings_workspace_catalog_repository.hpp"

#include <QSettings>
#include <QStringList>

namespace mint::infrastructure::desktop {
namespace {

const auto workspace_roots_key = QStringLiteral("workspaces/v1/roots");
const auto workspace_task_counts_key = QStringLiteral("workspaces/v1/task-counts");
const auto last_active_root_key = QStringLiteral("workspaces/v1/last-active-root");

QString qstring_from_path(const std::filesystem::path& path) {
#if defined(_WIN32)
    return QString::fromStdWString(path.wstring());
#else
    const auto value = path.native();
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
#endif
}

std::filesystem::path path_from_qstring(const QString& path) {
#if defined(_WIN32)
    return std::filesystem::path(path.toStdWString());
#else
    const auto value = path.toUtf8();
    return std::filesystem::path(value.constData());
#endif
}

} // namespace

QSettingsWorkspaceCatalogRepository::QSettingsWorkspaceCatalogRepository()
    : settings_(std::make_unique<QSettings>()) {
    // EN: A selected state directory must not inherit organization or system workspaces.
    // ZH-CN: 选定的状态目录不得继承组织或系统域的工作区。
    settings_->setFallbacksEnabled(false);
}

QSettingsWorkspaceCatalogRepository::QSettingsWorkspaceCatalogRepository(const QString& file_name)
    : settings_(std::make_unique<QSettings>(file_name, QSettings::IniFormat)) {
    settings_->setFallbacksEnabled(false);
}

QSettingsWorkspaceCatalogRepository::~QSettingsWorkspaceCatalogRepository() = default;

std::optional<mint::desktop::WorkspaceCatalogState>
QSettingsWorkspaceCatalogRepository::load() const {
    if (settings_->status() != QSettings::NoError) {
        return std::nullopt;
    }
    mint::desktop::WorkspaceCatalogState catalog;
    catalog.last_active_root = path_from_qstring(settings_->value(last_active_root_key).toString());
    const auto roots = settings_->value(workspace_roots_key).toStringList();
    catalog.roots.reserve(static_cast<std::size_t>(roots.size()));
    for (const auto& root : roots) {
        catalog.roots.push_back(path_from_qstring(root));
    }
    const auto stored_counts = settings_->value(workspace_task_counts_key).toList();
    catalog.task_counts.reserve(static_cast<std::size_t>(stored_counts.size()));
    for (const auto& stored_count : stored_counts) {
        const auto fields = stored_count.toMap();
        bool unarchived_ok = false;
        bool archived_ok = false;
        const auto unarchived =
            fields.value(QStringLiteral("unarchived")).toULongLong(&unarchived_ok);
        const auto archived = fields.value(QStringLiteral("archived")).toULongLong(&archived_ok);
        const auto root = path_from_qstring(fields.value(QStringLiteral("root")).toString());
        if (!root.empty() && unarchived_ok && archived_ok) {
            catalog.task_counts.push_back({.root = root,
                                           .unarchived = static_cast<std::size_t>(unarchived),
                                           .archived = static_cast<std::size_t>(archived)});
        }
    }
    return catalog;
}

bool QSettingsWorkspaceCatalogRepository::save(
    const mint::desktop::WorkspaceCatalogState& catalog) {
    QStringList roots;
    roots.reserve(static_cast<qsizetype>(catalog.roots.size()));
    for (const auto& root : catalog.roots) {
        roots.push_back(qstring_from_path(root));
    }
    QVariantList stored_counts;
    stored_counts.reserve(static_cast<qsizetype>(catalog.task_counts.size()));
    for (const auto& counts : catalog.task_counts) {
        QVariantMap fields;
        fields.insert(QStringLiteral("root"), qstring_from_path(counts.root));
        fields.insert(QStringLiteral("unarchived"),
                      QVariant::fromValue<qulonglong>(counts.unarchived));
        fields.insert(QStringLiteral("archived"), QVariant::fromValue<qulonglong>(counts.archived));
        stored_counts.push_back(fields);
    }
    settings_->setValue(workspace_roots_key, roots);
    settings_->setValue(workspace_task_counts_key, stored_counts);
    settings_->setValue(last_active_root_key, qstring_from_path(catalog.last_active_root));
    settings_->sync();
    return settings_->status() == QSettings::NoError;
}

} // namespace mint::infrastructure::desktop
