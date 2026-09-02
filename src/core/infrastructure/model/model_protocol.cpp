#include "model_protocol.hpp"
#include "mint/localization/localization.hpp"
#include "model_protocol_internal.hpp"

#include <stdexcept>

namespace mint::detail {

using localization::Message;
using localization::message;
using localization::Placeholder;

Json build_provider_request(const ModelProviderConfig& config, const Json& messages,
                            const Json& tools) {
    const auto profile = resolve_model_provider_profile(config);
    if (!tools.empty() && !profile.capabilities.function_tools) {
        throw std::invalid_argument(message(Message::model_protocol_function_tools_unsupported));
    }

    switch (profile.adapter) {
    case ModelAdapter::chat_completions:
        return protocol::build_chat_request(config, messages, tools, profile.capabilities);
    case ModelAdapter::responses:
        return protocol::build_responses_request(config, messages, tools, profile.capabilities);
    case ModelAdapter::anthropic_messages:
        return protocol::build_anthropic_request(config, messages, tools, profile.capabilities);
    }
    throw std::invalid_argument(message(Message::model_protocol_adapter_unknown));
}

ModelReply parse_provider_response(ModelAdapter adapter, const Json& response,
                                   const ModelResponseLimits& limits) {
    protocol::validate_limits(limits);
    switch (adapter) {
    case ModelAdapter::chat_completions:
        return protocol::parse_chat_response(response, limits);
    case ModelAdapter::responses:
        return protocol::parse_responses_response(response, limits);
    case ModelAdapter::anthropic_messages:
        return protocol::parse_anthropic_response(response, limits);
    }
    throw std::invalid_argument(message(Message::model_protocol_adapter_unknown));
}

} // namespace mint::detail
