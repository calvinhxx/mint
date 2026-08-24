#pragma once

#include <ostream>

#include "aiagent/infrastructure/project_store.hpp"

#include "command_line.hpp"

namespace aiagent::cli {

int handle_init_command(const CommandLine& command_line, ProjectStore& store,
                        std::ostream& output);
int handle_status_command(const CommandLine& command_line, ProjectStore& store,
                          std::ostream& output);

} // namespace aiagent::cli
