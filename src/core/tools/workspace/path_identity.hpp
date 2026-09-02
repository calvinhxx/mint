#pragma once

#include <filesystem>

namespace mint::tools::detail {

// EN: Returns an absolute, lexically normalized path whose existing components use the spelling
//     reported by the filesystem. Parent links are resolved, but the final directory entry is not
//     followed.
// ZH-CN: 返回词法规范化后的绝对路径；已有组件采用文件系统报告的拼写。父级链接会被解析，
//        但不会跟随最后一个目录项。
[[nodiscard]] std::filesystem::path path_entry_identity(const std::filesystem::path& path);

// EN: Object identity for normal file access. Existing paths are compared by the filesystem; the
//     component-aware fallback keeps non-existing suffixes case-sensitive on case-sensitive
//     volumes.
// ZH-CN: 用于普通文件访问的对象身份判断。已有路径由文件系统比较；按组件处理的回退逻辑
//        会在大小写敏感卷上保持不存在后缀的大小写敏感性。
[[nodiscard]] bool same_path_identity(const std::filesystem::path& left,
                                      const std::filesystem::path& right);

// EN: Directory-entry identity for snapshot filters. Unlike same_path_identity(), a symbolic link
//     is never considered identical to its target.
// ZH-CN: 用于快照过滤的目录项身份判断。与 same_path_identity() 不同，符号链接永远不会被
//        视为与其目标相同。
[[nodiscard]] bool same_path_entry_identity(const std::filesystem::path& left,
                                            const std::filesystem::path& right);

} // namespace mint::tools::detail
