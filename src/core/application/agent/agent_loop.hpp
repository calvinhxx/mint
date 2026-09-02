#pragma once

#include "mint/application/agent.hpp"

#include <ostream>
#include <string>

namespace mint::agent_detail {

[[nodiscard]] AgentResult run_agent_loop(ModelClient& model, AgentTools& tools,
                                         std::ostream& output, const AgentOptions& options,
                                         const AgentServices& services,
                                         const std::string& requested_task,
                                         std::string system_prompt);

} // namespace mint::agent_detail
