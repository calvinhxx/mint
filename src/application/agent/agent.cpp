#include "mint/application/agent.hpp"

#include "agent_loop.hpp"
#include "mint/localization/localization.hpp"

#include <stdexcept>
#include <utility>

namespace mint {

using localization::Message;
using localization::message;
using localization::Placeholder;

Agent::Agent(ModelClient& model, AgentTools& tools, std::ostream& output, AgentOptions options,
             AgentServices services)
    : model_(model), tools_(tools), output_(output), options_(std::move(options)),
      services_(std::move(services)) {
    if (options_.max_turns == 0) {
        throw std::invalid_argument(message(Message::agent_config_max_turns));
    }
    if (options_.max_context_bytes < runtime_bounds::min_context_bytes ||
        options_.max_context_bytes > runtime_bounds::max_context_bytes) {
        throw std::invalid_argument(message(Message::agent_config_max_context_bytes));
    }
    if (options_.max_total_tokens > runtime_bounds::max_total_tokens) {
        throw std::invalid_argument(message(Message::agent_config_max_total_tokens));
    }
    const auto capabilities = tools_.capabilities();
    if (options_.require_verification_after_write &&
        (!capabilities.write_enabled || !capabilities.commands_enabled)) {
        throw std::invalid_argument(message(Message::agent_config_verification_capabilities));
    }
    if (options_.resume_session && services_.session_repository == nullptr) {
        throw std::invalid_argument(message(Message::agent_config_resume_store));
    }
    if (options_.retry_in_flight_tool && !options_.resume_session) {
        throw std::invalid_argument(message(Message::agent_config_retry_requires_resume));
    }
}

AgentResult Agent::run(const std::string& requested_task) {
    return agent_detail::run_agent_loop(model_, tools_, output_, options_, services_,
                                        requested_task, system_prompt());
}

} // namespace mint
