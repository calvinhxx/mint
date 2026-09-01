#pragma once

#include "mint/infrastructure/project_store.hpp"

#include "command_line.hpp"
#include "support/console.hpp"

#include <optional>

namespace mint::cli {

int run_agent_command(CommandLine command_line, std::optional<ManagedTaskPaths>& managed_task,
                      Console& console);

} // namespace mint::cli
