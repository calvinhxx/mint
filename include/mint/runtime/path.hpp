#pragma once

#include <filesystem>

namespace mint {

/**
 * EN: Returns true when candidate is root or one of its descendants. Both paths must be absolute.
 * The comparison is lexical and does not follow symlinks; callers handling untrusted filesystem
 * entries must canonicalize first or use the identity-aware tools path helpers.
 *
 * ZH-CN: 当 candidate 是 root 本身或其后代时返回 true。两个路径都必须是绝对路径。
 * 比较只按词法进行且不会跟随符号链接；处理不可信文件系统条目时，调用方必须先规范化
 * 路径，或者使用能识别文件身份的 tools 路径辅助函数。
 */
[[nodiscard]] bool is_path_within(const std::filesystem::path& root,
                                  const std::filesystem::path& candidate);

} // namespace mint
