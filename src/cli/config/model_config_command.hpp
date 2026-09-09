#pragma once

#include "command_line.hpp"
#include "support/console.hpp"

namespace mint::cli {

int run_model_config_command(const CommandLine& command_line, Console& console);

} // namespace mint::cli
