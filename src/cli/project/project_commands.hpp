#pragma once

#include "mint/infrastructure/project_store.hpp"

#include "command_line.hpp"
#include "support/console.hpp"

namespace mint::cli {

int handle_init_command(const CommandLine& command_line, ProjectStore& store, Console& console);
int handle_status_command(const CommandLine& command_line, ProjectStore& store, Console& console);

} // namespace mint::cli
