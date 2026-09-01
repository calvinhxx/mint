#pragma once

#include <string>
#include <string_view>

namespace mint {

// EN: Makes untrusted text safe to display on a terminal while retaining readable UTF-8, line
//     feeds and tabs. Control bytes and bidirectional formatting characters are rendered as
//     explicit escapes instead of being interpreted.
// ZH-CN: 在保留可读 UTF-8、换行和制表符的同时，让不可信文本可以安全显示在终端中。
//        控制字节和双向排版字符会显示为明确的转义序列，而不会被终端解释。
[[nodiscard]] std::string escape_terminal_text(std::string_view text);

// EN: Single-line variant for labels, paths and other values embedded in CLI status lines.
//     Newlines and tabs are escaped as well.
// ZH-CN: 用于 CLI 状态行中的标签、路径及其他值的单行版本；换行和制表符也会被转义。
[[nodiscard]] std::string escape_terminal_field(std::string_view text);

} // namespace mint
