#pragma once

#include "model_protocol.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace mint::detail::protocol {

inline constexpr std::string_view provider_state_field = "_mint_provider_state";
inline constexpr std::string_view legacy_provider_state_field = "_aiagent_provider_state";

[[noreturn]] void throw_byte_limit(std::string_view resource, std::size_t limit);
[[noreturn]] void throw_count_limit(std::string_view resource, std::size_t limit);
void add_bytes(std::size_t& current, std::size_t added, std::size_t limit,
               std::string_view resource);
void append_bounded(std::string& target, std::string_view value, std::size_t limit,
                    std::string_view resource);
void validate_limits(const ModelResponseLimits& limits);

struct OutputBudget {
    explicit OutputBudget(const ModelResponseLimits& configured_limits);

    void add_text(std::size_t bytes);
    void add_reasoning(std::size_t bytes);
    void add_tool(std::size_t id_bytes, std::size_t name_bytes, std::size_t argument_bytes);

    const ModelResponseLimits& limits;
    std::size_t text_bytes = 0;
    std::size_t reasoning_bytes = 0;
    std::size_t tool_argument_bytes = 0;
    std::size_t tool_metadata_bytes = 0;
    std::size_t tool_calls = 0;
};

[[nodiscard]] const Json* provider_state(const Json& message);
[[nodiscard]] Json parse_arguments(const Json& value);
[[nodiscard]] std::string content_text(const Json& content);
[[nodiscard]] std::string extract_text(const Json& content, OutputBudget& budget);
[[nodiscard]] std::string stream_error_message(const Json& value);
[[nodiscard]] std::string response_status_error(const Json& response);

[[nodiscard]] Json build_chat_request(const ModelProviderConfig& config, const Json& messages,
                                      const Json& tools,
                                      const ModelProviderCapabilities& capabilities);
[[nodiscard]] Json build_responses_request(const ModelProviderConfig& config, const Json& messages,
                                           const Json& tools,
                                           const ModelProviderCapabilities& capabilities);
[[nodiscard]] Json build_anthropic_request(const ModelProviderConfig& config, const Json& messages,
                                           const Json& tools,
                                           const ModelProviderCapabilities& capabilities);

[[nodiscard]] ModelReply parse_chat_response(const Json& response,
                                             const ModelResponseLimits& limits);
[[nodiscard]] ModelReply parse_responses_response(const Json& response,
                                                  const ModelResponseLimits& limits);
[[nodiscard]] ModelReply parse_anthropic_response(const Json& response,
                                                  const ModelResponseLimits& limits);

} // namespace mint::detail::protocol
