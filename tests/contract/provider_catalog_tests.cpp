#include "mint/infrastructure/model_provider_client.hpp"

#include "model/model_protocol.hpp"
#include "scripted_http_server.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

mint::Json tool_definitions() {
    return mint::Json::array({{{"type", "function"},
                               {"function",
                                {{"name", "read_file"},
                                 {"description", "Read one file"},
                                 {"parameters",
                                  {{"type", "object"},
                                   {"properties", {{"path", {{"type", "string"}}}}},
                                   {"required", mint::Json::array({"path"})},
                                   {"additionalProperties", false}}}}}}});
}

std::string sse(const mint::Json& event) {
    return "data: " + event.dump() + "\n\n";
}

void feed_fragmented(mint::detail::ModelStreamDecoder& decoder, std::string_view stream) {
    constexpr std::size_t fragment_sizes[] = {1, 11, 3, 17, 5};
    std::size_t offset = 0;
    std::size_t fragment = 0;
    while (offset < stream.size()) {
        const auto bytes =
            std::min(fragment_sizes[fragment % std::size(fragment_sizes)], stream.size() - offset);
        decoder.feed(stream.substr(offset, bytes));
        offset += bytes;
        ++fragment;
    }
}

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

mint::ModelProviderConfig anthropic_config(bool stream = false) {
    return {.api_url = "https://example.test/v1/messages",
            .model = "claude-test",
            .max_completion_tokens = 321,
            .adapter = mint::ModelAdapter::anthropic_messages,
            .stream = stream};
}

TEST(MainstreamProviderChatProtocolTest, NormalizesDeepSeekPromptCacheUsage) {
    const mint::Json response = {
        {"choices", mint::Json::array({{{"message", {{"role", "assistant"}, {"content", "ok"}}}}})},
        {"usage",
         {{"prompt_tokens", 100},
          {"completion_tokens", 8},
          {"total_tokens", 108},
          {"prompt_cache_hit_tokens", 75},
          {"prompt_cache_miss_tokens", 25}}}};

    const auto reply =
        mint::detail::parse_provider_response(mint::ModelAdapter::chat_completions, response);

    EXPECT_TRUE(reply.usage.available);
    EXPECT_EQ(reply.usage.prompt_tokens, 100);
    EXPECT_EQ(reply.usage.cached_tokens, 75);
    EXPECT_DOUBLE_EQ(*mint::model_usage::cache_hit_rate(reply.usage), 0.75);
}

TEST(MainstreamProviderChatProtocolTest, ReconstructsPromptTotalFromDeepSeekCacheBreakdown) {
    const mint::Json response = {
        {"choices", mint::Json::array({{{"message", {{"role", "assistant"}, {"content", "ok"}}}}})},
        {"usage",
         {{"completion_tokens", 8},
          {"prompt_cache_hit_tokens", 75},
          {"prompt_cache_miss_tokens", 25}}}};

    const auto reply =
        mint::detail::parse_provider_response(mint::ModelAdapter::chat_completions, response);

    EXPECT_TRUE(reply.usage.available);
    EXPECT_EQ(reply.usage.prompt_tokens, 100);
    EXPECT_EQ(reply.usage.cached_tokens, 75);
    EXPECT_EQ(reply.usage.total_tokens, 108);
}

TEST(MainstreamProviderAnthropicProtocolTest, MapsCanonicalRequestToMessagesApi) {
    const auto messages =
        mint::Json::array({{{"role", "system"}, {"content", "Keep changes small."}},
                           {{"role", "user"}, {"content", "Read README.md"}}});

    const auto request =
        mint::detail::build_provider_request(anthropic_config(), messages, tool_definitions());

    EXPECT_EQ(request.at("model"), "claude-test");
    EXPECT_EQ(request.at("max_tokens"), 321);
    EXPECT_EQ(request.at("system"), "Keep changes small.");
    ASSERT_EQ(request.at("messages").size(), 1);
    EXPECT_EQ(request.at("messages").at(0).at("role"), "user");
    EXPECT_EQ(request.at("messages").at(0).at("content").at(0),
              mint::Json({{"type", "text"}, {"text", "Read README.md"}}));
    ASSERT_EQ(request.at("tools").size(), 1);
    const auto& tool = request.at("tools").at(0);
    EXPECT_EQ(tool.at("name"), "read_file");
    EXPECT_EQ(tool.at("description"), "Read one file");
    EXPECT_EQ(tool.at("input_schema").at("required").at(0), "path");
    EXPECT_EQ(request.at("tool_choice"), mint::Json({{"type", "auto"}}));
    EXPECT_FALSE(request.contains("stream"));
}

TEST(MainstreamProviderAnthropicProtocolTest, RoundTripsToolUseAndToolResult) {
    const mint::Json response = {
        {"id", "msg_1"},
        {"type", "message"},
        {"role", "assistant"},
        {"model", "claude-test"},
        {"content", mint::Json::array({{{"type", "text"}, {"text", "I will inspect it."}},
                                       {{"type", "tool_use"},
                                        {"id", "toolu_1"},
                                        {"name", "read_file"},
                                        {"input", {{"path", "README.md"}}}}})},
        {"usage",
         {{"input_tokens", 11},
          {"cache_creation_input_tokens", 3},
          {"cache_read_input_tokens", 5},
          {"output_tokens", 7}}}};

    const auto reply =
        mint::detail::parse_provider_response(mint::ModelAdapter::anthropic_messages, response);
    EXPECT_EQ(reply.text, "I will inspect it.");
    ASSERT_EQ(reply.tool_calls.size(), 1);
    EXPECT_EQ(reply.tool_calls.at(0).id, "toolu_1");
    EXPECT_EQ(reply.tool_calls.at(0).name, "read_file");
    EXPECT_EQ(reply.tool_calls.at(0).arguments.at("path"), "README.md");
    EXPECT_EQ(reply.assistant_message.at("tool_calls").at(0).at("id"), "toolu_1");
    EXPECT_TRUE(reply.usage.available);
    EXPECT_EQ(reply.usage.prompt_tokens, 19);
    EXPECT_EQ(reply.usage.completion_tokens, 7);
    EXPECT_EQ(reply.usage.total_tokens, 26);
    EXPECT_EQ(reply.usage.cached_tokens, 5);

    auto messages =
        mint::Json::array({{{"role", "system"}, {"content", "Be concise."}},
                           {{"role", "user"}, {"content", "Read README.md"}},
                           reply.assistant_message,
                           {{"role", "tool"}, {"tool_call_id", "toolu_1"}, {"content", "# mint"}}});
    const auto continuation =
        mint::detail::build_provider_request(anthropic_config(), messages, tool_definitions());
    const auto& provider_messages = continuation.at("messages");
    ASSERT_EQ(provider_messages.size(), 3);
    ASSERT_TRUE(provider_messages.at(1).at("content").is_array());
    EXPECT_EQ(provider_messages.at(1).at("content").at(0),
              mint::Json({{"type", "text"}, {"text", "I will inspect it."}}));
    EXPECT_EQ(provider_messages.at(1).at("content").at(1).at("type"), "tool_use");
    EXPECT_EQ(provider_messages.at(1).at("content").at(1).at("id"), "toolu_1");
    EXPECT_EQ(provider_messages.at(1).at("content").at(1).at("input").at("path"), "README.md");
    EXPECT_EQ(provider_messages.at(2).at("role"), "user");
    EXPECT_EQ(provider_messages.at(2).at("content").at(0).at("type"), "tool_result");
    EXPECT_EQ(provider_messages.at(2).at("content").at(0).at("tool_use_id"), "toolu_1");
    EXPECT_EQ(provider_messages.at(2).at("content").at(0).at("content"), "# mint");
}

TEST(MainstreamProviderAnthropicProtocolTest, DecodesTextToolAndUsageStream) {
    std::vector<mint::ModelStreamEvent> deltas;
    mint::detail::ModelStreamDecoder decoder(
        mint::ModelAdapter::anthropic_messages,
        [&](const mint::ModelStreamEvent& event) { deltas.push_back(event); });

    std::string stream;
    stream += sse({{"type", "message_start"},
                   {"message",
                    {{"id", "msg_stream"},
                     {"type", "message"},
                     {"role", "assistant"},
                     {"model", "claude-test"},
                     {"content", mint::Json::array()},
                     {"usage", {{"input_tokens", 10}, {"output_tokens", 0}}}}}});
    stream += sse({{"type", "content_block_start"},
                   {"index", 0},
                   {"content_block", {{"type", "text"}, {"text", ""}}}});
    stream += sse({{"type", "content_block_delta"},
                   {"index", 0},
                   {"delta", {{"type", "text_delta"}, {"text", "查"}}}});
    stream += sse({{"type", "content_block_delta"},
                   {"index", 0},
                   {"delta", {{"type", "text_delta"}, {"text", "看"}}}});
    stream += sse({{"type", "content_block_stop"}, {"index", 0}});
    stream += sse({{"type", "content_block_start"},
                   {"index", 1},
                   {"content_block",
                    {{"type", "tool_use"},
                     {"id", "toolu_stream"},
                     {"name", "read_file"},
                     {"input", mint::Json::object()}}}});
    stream += sse({{"type", "content_block_delta"},
                   {"index", 1},
                   {"delta", {{"type", "input_json_delta"}, {"partial_json", "{\"path\":"}}}});
    stream += sse({{"type", "content_block_delta"},
                   {"index", 1},
                   {"delta", {{"type", "input_json_delta"}, {"partial_json", "\"README.md\"}"}}}});
    stream += sse({{"type", "content_block_stop"}, {"index", 1}});
    stream += sse({{"type", "message_delta"},
                   {"delta", {{"stop_reason", "tool_use"}}},
                   {"usage", {{"output_tokens", 4}}}});
    stream += sse({{"type", "message_stop"}});
    feed_fragmented(decoder, stream);

    const auto reply = mint::detail::parse_provider_response(mint::ModelAdapter::anthropic_messages,
                                                             decoder.finish());
    EXPECT_EQ(reply.text, "查看");
    ASSERT_EQ(reply.tool_calls.size(), 1);
    EXPECT_EQ(reply.tool_calls.at(0).id, "toolu_stream");
    EXPECT_EQ(reply.tool_calls.at(0).name, "read_file");
    EXPECT_EQ(reply.tool_calls.at(0).arguments.at("path"), "README.md");
    EXPECT_EQ(reply.usage.prompt_tokens, 10);
    EXPECT_EQ(reply.usage.completion_tokens, 4);
    EXPECT_EQ(reply.usage.total_tokens, 14);
    ASSERT_EQ(deltas.size(), 4);
    EXPECT_EQ(deltas.at(0).kind, mint::ModelStreamEventKind::text_delta);
    EXPECT_EQ(deltas.at(0).delta, "查");
    EXPECT_EQ(deltas.at(2).kind, mint::ModelStreamEventKind::tool_arguments_delta);
    EXPECT_EQ(deltas.at(2).output_index, 1);
    EXPECT_EQ(deltas.at(2).item_id, "toolu_stream");
    EXPECT_EQ(deltas.at(2).name, "read_file");
}

TEST(MainstreamProviderAnthropicProtocolTest, PreservesSignedThinkingForToolContinuation) {
    mint::detail::ModelStreamDecoder decoder(mint::ModelAdapter::anthropic_messages, {});
    std::string stream;
    stream += sse({{"type", "message_start"},
                   {"message",
                    {{"id", "msg_thinking"},
                     {"model", "claude-test"},
                     {"content", mint::Json::array()},
                     {"usage", {{"input_tokens", 8}, {"output_tokens", 0}}}}}});
    stream += sse({{"type", "content_block_start"},
                   {"index", 0},
                   {"content_block", {{"type", "thinking"}, {"thinking", ""}}}});
    stream += sse({{"type", "content_block_delta"},
                   {"index", 0},
                   {"delta", {{"type", "thinking_delta"}, {"thinking", "inspect first"}}}});
    stream += sse({{"type", "content_block_delta"},
                   {"index", 0},
                   {"delta", {{"type", "signature_delta"}, {"signature", "signed-state"}}}});
    stream += sse({{"type", "content_block_stop"}, {"index", 0}});
    stream += sse({{"type", "content_block_start"},
                   {"index", 1},
                   {"content_block",
                    {{"type", "tool_use"},
                     {"id", "toolu_thinking"},
                     {"name", "read_file"},
                     {"input", mint::Json::object()}}}});
    stream += sse(
        {{"type", "content_block_delta"},
         {"index", 1},
         {"delta", {{"type", "input_json_delta"}, {"partial_json", "{\"path\":\"README.md\"}"}}}});
    stream += sse({{"type", "content_block_stop"}, {"index", 1}});
    stream += sse({{"type", "message_delta"},
                   {"delta", {{"stop_reason", "tool_use"}}},
                   {"usage", {{"output_tokens", 5}}}});
    stream += sse({{"type", "message_stop"}});
    feed_fragmented(decoder, stream);

    const auto reply = mint::detail::parse_provider_response(mint::ModelAdapter::anthropic_messages,
                                                             decoder.finish());
    ASSERT_EQ(reply.tool_calls.size(), 1);
    const auto& raw_content = reply.assistant_message.at("_mint_provider_state").at("content");
    ASSERT_EQ(raw_content.size(), 2);
    EXPECT_EQ(raw_content.at(0).at("type"), "thinking");
    EXPECT_EQ(raw_content.at(0).at("thinking"), "inspect first");
    EXPECT_EQ(raw_content.at(0).at("signature"), "signed-state");

    const auto messages = mint::Json::array(
        {{{"role", "user"}, {"content", "Read README.md"}},
         reply.assistant_message,
         {{"role", "tool"}, {"tool_call_id", "toolu_thinking"}, {"content", "# mint"}}});
    const auto continuation =
        mint::detail::build_provider_request(anthropic_config(), messages, tool_definitions());
    ASSERT_EQ(continuation.at("messages").size(), 3);
    EXPECT_EQ(continuation.at("messages").at(1).at("content"), raw_content);
    EXPECT_EQ(continuation.at("messages").at(2).at("content").at(0).at("type"), "tool_result");
}

TEST(MainstreamProviderAnthropicTransportTest, UsesNativeAuthenticationHeaders) {
    const mint::Json response = {
        {"id", "msg_http"},
        {"type", "message"},
        {"role", "assistant"},
        {"model", "claude-test"},
        {"content", mint::Json::array({{{"type", "text"}, {"text", "ok"}}})},
        {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}};
    mint::test::ScriptedHttpServer server({{.body = response.dump()}});
    mint::ModelProviderClient client({.api_url = server.url("/v1/messages"),
                                      .api_key = "anthropic-test-key",
                                      .model = "claude-test",
                                      .connect_timeout_seconds = 2,
                                      .request_timeout_seconds = 2,
                                      .max_retries = 0,
                                      .adapter = mint::ModelAdapter::anthropic_messages});

    const auto reply = client.complete(
        mint::Json::array({{{"role", "user"}, {"content", "hello"}}}), mint::Json::array());
    EXPECT_EQ(reply.text, "ok");
    server.wait();

    const auto raw_request = server.request();
    const auto normalized = lowercase(raw_request);
    EXPECT_NE(normalized.find("x-api-key: anthropic-test-key"), std::string::npos);
    EXPECT_NE(normalized.find("anthropic-version: 2023-06-01"), std::string::npos);
    EXPECT_EQ(normalized.find("authorization:"), std::string::npos);
}

TEST(MainstreamProviderChatProtocolTest, PreservesGeminiThoughtSignaturesWithoutToolIndexes) {
    mint::detail::ModelStreamDecoder decoder(mint::ModelAdapter::chat_completions, {});
    const mint::Json first = {
        {"choices",
         mint::Json::array(
             {{{"delta",
                {{"tool_calls",
                  mint::Json::array(
                      {{{"id", "call_1"},
                        {"type", "function"},
                        {"function", {{"name", "read_file"}, {"arguments", ""}}},
                        {"extra_content", {{"google", {{"thought_signature", "signature-1"}}}}}},
                       {{"id", "call_2"},
                        {"type", "function"},
                        {"function", {{"name", "read_file"}, {"arguments", ""}}},
                        {"extra_content",
                         {{"google", {{"thought_signature", "signature-2"}}}}}}})}}}}})}};
    const mint::Json second = {
        {"choices",
         mint::Json::array(
             {{{"delta",
                {{"tool_calls",
                  mint::Json::array(
                      {{{"function", {{"arguments", R"({"path":"README.md"})"}}}},
                       {{"function", {{"arguments", R"({"path":"CHANGELOG.md"})"}}}}})}}}}})}};
    feed_fragmented(decoder, sse(first) + sse(second) + "data: [DONE]\n\n");

    const auto reply = mint::detail::parse_provider_response(mint::ModelAdapter::chat_completions,
                                                             decoder.finish());
    ASSERT_EQ(reply.tool_calls.size(), 2);
    EXPECT_EQ(reply.tool_calls.at(0).arguments.at("path"), "README.md");
    EXPECT_EQ(reply.tool_calls.at(1).arguments.at("path"), "CHANGELOG.md");
    const auto& calls = reply.assistant_message.at("tool_calls");
    EXPECT_EQ(calls.at(0).at("extra_content").at("google").at("thought_signature"), "signature-1");
    EXPECT_EQ(calls.at(1).at("extra_content").at("google").at("thought_signature"), "signature-2");

    auto messages = mint::Json::array(
        {{{"role", "user"}, {"content", "Read two files"}}, reply.assistant_message});
    messages.push_back({{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "# mint"}});
    messages.push_back({{"role", "tool"}, {"tool_call_id", "call_2"}, {"content", "# Changes"}});
    mint::ModelProviderConfig config{.api_url =
                                         "https://generativelanguage.googleapis.com/v1beta/openai/"
                                         "chat/completions",
                                     .model = "gemini-test",
                                     .provider = mint::ModelProvider::google};
    const auto continuation =
        mint::detail::build_provider_request(config, messages, tool_definitions());
    const auto& replayed = continuation.at("messages").at(1).at("tool_calls");
    EXPECT_EQ(replayed.at(0).at("extra_content"), calls.at(0).at("extra_content"));
    EXPECT_EQ(replayed.at(1).at("extra_content"), calls.at(1).at("extra_content"));
}

} // namespace
