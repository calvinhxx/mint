#include "qt_path.hpp"

#include "mint/platform/path.hpp"

#include <QByteArray>

#include <string_view>

namespace mint::gui {

QString qstring_from_path(const std::filesystem::path& path) {
    const auto utf8 = platform::path_to_utf8(path);
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

std::filesystem::path path_from_qstring(const QString& path) {
    const auto utf8 = path.toUtf8();
    return platform::path_from_utf8(
        std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
}

} // namespace mint::gui
