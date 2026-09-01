#pragma once

#include <string>
#include <string_view>

namespace mint {

// Makes untrusted text safe to display on a terminal while retaining readable
// UTF-8, line feeds and tabs. Control bytes and bidirectional formatting
// characters are rendered as explicit escapes instead of being interpreted.
[[nodiscard]] std::string escape_terminal_text(std::string_view text);

// Single-line variant for labels, paths and other values embedded in CLI status
// lines. Newlines and tabs are escaped as well.
[[nodiscard]] std::string escape_terminal_field(std::string_view text);

} // namespace mint
