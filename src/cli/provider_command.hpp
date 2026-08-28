#pragma once

#include "command_line.hpp"
#include "console.hpp"

namespace mint::cli {

int run_provider_command(const CommandLine& command_line, Console& console);

} // namespace mint::cli
