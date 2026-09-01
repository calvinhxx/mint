#include "mint/application/agent.hpp"

#include "agent_loop.hpp"

#include <stdexcept>
#include <utility>

namespace mint {

Agent::Agent(ModelClient& model, AgentTools& tools, std::ostream& output, AgentOptions options,
             AgentServices services)
    : model_(model), tools_(tools), output_(output), options_(std::move(options)),
      services_(std::move(services)) {
    if (options_.max_turns == 0) {
        throw std::invalid_argument("max_turns 必须大于 0");
    }
    if (options_.max_context_bytes < runtime_bounds::min_context_bytes ||
        options_.max_context_bytes > runtime_bounds::max_context_bytes) {
        throw std::invalid_argument("max_context_bytes 必须在 16 KiB 到 8 MiB 之间");
    }
    const auto capabilities = tools_.capabilities();
    if (options_.require_verification_after_write &&
        (!capabilities.write_enabled || !capabilities.commands_enabled)) {
        throw std::invalid_argument(
            "require_verification_after_write 需要同时启用写工具和至少一个命令程序");
    }
    if (options_.resume_session && services_.session_repository == nullptr) {
        throw std::invalid_argument("resume_session 需要会话存储");
    }
    if (options_.retry_in_flight_tool && !options_.resume_session) {
        throw std::invalid_argument("retry_in_flight_tool 只允许用于恢复会话");
    }
}

AgentResult Agent::run(const std::string& requested_task) {
    return agent_detail::run_agent_loop(model_, tools_, output_, options_, services_,
                                        requested_task, system_prompt());
}

} // namespace mint
