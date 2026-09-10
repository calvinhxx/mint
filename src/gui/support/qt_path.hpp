#pragma once

#include <QString>

#include <filesystem>

namespace mint::gui {

[[nodiscard]] QString qstring_from_path(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path path_from_qstring(const QString& path);

} // namespace mint::gui
