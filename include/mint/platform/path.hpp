#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace mint::platform {

[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text);
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);
[[nodiscard]] std::string path_to_generic_utf8(const std::filesystem::path& path);

// EN: Compare one lexical component using the host's conventional case rules, without I/O.
// ZH-CN: 按宿主平台的常规大小写规则比较单个路径组件，不访问文件系统。
[[nodiscard]] bool same_path_component(const std::filesystem::path& left,
                                       const std::filesystem::path& right);

} // namespace mint::platform
