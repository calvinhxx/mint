#include "agent_conversation.hpp"

#include "mint/localization/localization.hpp"

#include <stdexcept>
#include <utility>

namespace mint::agent_detail {

using localization::Message;
using localization::message;
using localization::Placeholder;

Conversation::Conversation(Json messages) : messages_(std::move(messages)) {}

Conversation Conversation::start(std::string system_prompt, std::string user_request) {
    return Conversation(Json::array({{{"role", "system"}, {"content", std::move(system_prompt)}},
                                     {{"role", "user"}, {"content", std::move(user_request)}}}));
}

Conversation Conversation::restore(Json messages) {
    if (!messages.is_array() || messages.size() < 2) {
        throw std::invalid_argument(message(Message::agent_conversation_snapshot_invalid));
    }
    return Conversation(std::move(messages));
}

const Json& Conversation::messages() const noexcept {
    return messages_;
}

void Conversation::append_user(std::string content) {
    messages_.push_back({{"role", "user"}, {"content", std::move(content)}});
}

void Conversation::append_assistant(Json message) {
    if (!message.is_object()) {
        throw std::runtime_error(
            localization::message(Message::agent_conversation_assistant_message_invalid));
    }
    messages_.push_back(std::move(message));
}

void Conversation::append_tool_result(const ToolCall& call, std::string result) {
    messages_.push_back(
        {{"role", "tool"}, {"tool_call_id", call.id}, {"content", std::move(result)}});
}

} // namespace mint::agent_detail
