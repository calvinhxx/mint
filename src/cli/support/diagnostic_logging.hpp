#pragma once

#include "mint/infrastructure/diagnostic_log.hpp"

namespace mint::cli {

class Console;
struct CommandLine;

[[nodiscard]] diagnostics::LogStatus configure_diagnostic_logging(const CommandLine& command_line,
                                                                  Console& console);

} // namespace mint::cli
