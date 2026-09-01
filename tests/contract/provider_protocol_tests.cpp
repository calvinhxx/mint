#include "mint/infrastructure/config.hpp"
#include "mint/infrastructure/model_provider_client.hpp"

#include "agent_command.hpp"
#include "model/model_protocol.hpp"
#include "provider_command.hpp"
#include "scripted_http_server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

#define MINT_EXPECT(condition, message) EXPECT_TRUE(condition) << (message)

using mint::test::ScriptedHttpServer;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ =
            std::filesystem::temp_directory_path() / ("mint-v1-4-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

void set_environment(std::string_view name, std::string_view value) {
#if defined(_WIN32)
    const auto result = ::_putenv_s(std::string(name).c_str(), std::string(value).c_str());
#else
    const auto result = ::setenv(std::string(name).c_str(), std::string(value).c_str(), 1);
#endif
    if (result != 0) {
        throw std::runtime_error("could not override test environment variable");
    }
}

void unset_environment(std::string_view name) noexcept {
#if defined(_WIN32)
    (void)::_putenv_s(std::string(name).c_str(), "");
#else
    (void)::unsetenv(std::string(name).c_str());
#endif
}

class ScopedEnvironmentOverride final {
  public:
    ScopedEnvironmentOverride(std::string name, std::string value) : name_(std::move(name)) {
        if (const auto* current = std::getenv(name_.c_str())) {
            previous_ = current;
        }
        set_environment(name_, value);
    }

    ~ScopedEnvironmentOverride() {
        if (previous_.has_value()) {
            try {
                set_environment(name_, *previous_);
            } catch (...) {
            }
        } else {
            unset_environment(name_);
        }
    }

    ScopedEnvironmentOverride(const ScopedEnvironmentOverride&) = delete;
    ScopedEnvironmentOverride& operator=(const ScopedEnvironmentOverride&) = delete;

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not create test file " + path.string());
    }
    output << text;
}

std::string sse(const mint::Json& event) {
    return "data: " + event.dump() + "\n\n";
}

mint::test::ScriptedHttpResponse sse_response(std::string body, int status = 200) {
    return {.status = status,
            .content_type = "text/event-stream",
            .body = std::move(body),
            .fragment_bytes = 17};
}

void feed_fragmented(mint::detail::ModelStreamDecoder& decoder, const std::string& stream) {
    constexpr std::size_t fragment_sizes[] = {1, 7, 2, 19, 3, 5, 11};
    std::size_t offset = 0;
    std::size_t fragment = 0;
    while (offset < stream.size()) {
        const auto bytes =
            std::min(fragment_sizes[fragment % std::size(fragment_sizes)], stream.size() - offset);
        decoder.feed(std::string_view(stream).substr(offset, bytes));
        offset += bytes;
        ++fragment;
    }
}

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

TEST(ProviderConfigContractTest, LoadsLegacyEndpointFieldAndSelectsAdapters) {
    TemporaryDirectory temporary;
    const auto legacy_path = temporary.path() / "legacy.json";
    write_text(legacy_path, R"({"api_url":"https://example.test/v1/chat/completions",)"
                            R"("api_key":"secret","model":"test-model"})");
    const auto legacy = mint::load_model_provider_config(legacy_path);
    MINT_EXPECT(legacy.adapter == mint::ModelAdapter::chat_completions && !legacy.stream,
                "legacy endpoint field defaults to non-streaming Chat Completions");

    const auto responses_path = temporary.path() / "responses.json";
    write_text(responses_path,
               R"({"adapter":"responses","api_url":"https://api.openai.com/v1/responses",)"
               R"("api_key":"secret","model":"test-model","stream":true})");
    const auto responses = mint::load_model_provider_config(responses_path);
    MINT_EXPECT(responses.adapter == mint::ModelAdapter::responses && responses.stream,
                "config selects the Responses streaming adapter explicitly");

    const auto invalid_adapter_path = temporary.path() / "invalid-adapter.json";
    write_text(invalid_adapter_path, R"({"adapter":"magic","api_url":"https://example.test",)"
                                     R"("model":"test-model"})");
    bool adapter_rejected = false;
    try {
        (void)mint::load_model_provider_config(invalid_adapter_path);
    } catch (const std::runtime_error& error) {
        adapter_rejected = std::string(error.what()).find("adapter") != std::string::npos;
    }
    MINT_EXPECT(adapter_rejected, "unknown adapters fail with a field-specific error");

    const auto invalid_stream_path = temporary.path() / "invalid-stream.json";
    write_text(invalid_stream_path,
               R"({"api_url":"https://example.test","model":"test-model","stream":"yes"})");
    bool stream_rejected = false;
    try {
        (void)mint::load_model_provider_config(invalid_stream_path);
    } catch (const std::runtime_error& error) {
        stream_rejected = std::string(error.what()).find("stream") != std::string::npos;
    }
    MINT_EXPECT(stream_rejected, "stream requires a JSON boolean");
}

TEST(ProviderConfigContractTest, ValidatesResponseLimitsStrictly) {
    TemporaryDirectory temporary;
    const auto configured_path = temporary.path() / "response-limits.json";
    write_text(configured_path,
               R"({"api_url":"https://example.test","model":"test-model",)"
               R"("response_limits":{"max_http_body_bytes":512,"max_tool_calls":2}})");
    const auto configured = mint::load_model_provider_config(configured_path);
    MINT_EXPECT(configured.response_limits.max_http_body_bytes == 512 &&
                    configured.response_limits.max_tool_calls == 2,
                "response limit overrides are loaded");

    const auto unknown_path = temporary.path() / "unknown-response-limit.json";
    write_text(unknown_path, R"({"api_url":"https://example.test","model":"test-model",)"
                             R"("response_limits":{"unbounded":1}})");
    EXPECT_THROW((void)mint::load_model_provider_config(unknown_path), std::runtime_error);
}

TEST(ProviderProtocolContractTest, BoundsSseBuffersAndToolIndexes) {
    {
        mint::ModelResponseLimits limits;
        limits.max_tool_calls = 4;
        mint::detail::ModelStreamDecoder decoder(mint::ModelAdapter::chat_completions, {}, limits);
        const mint::Json tool_delta = {{"index", 1'000'000'000},
                                       {"function", {{"arguments", "{}"}}}};
        const auto event = sse(
            {{"choices", mint::Json::array(
                             {{{"delta", {{"tool_calls", mint::Json::array({tool_delta})}}}}})}});
        try {
            decoder.feed(event);
            FAIL() << "a tool index above the configured bound must fail";
        } catch (const std::runtime_error& error) {
            MINT_EXPECT(std::string(error.what()).find("index") != std::string::npos,
                        "a huge tool index fails before growing the tool vector");
        }
    }

    {
        mint::ModelResponseLimits limits;
        limits.max_sse_line_bytes = 16;
        mint::detail::ModelStreamDecoder decoder(mint::ModelAdapter::responses, {}, limits);
        try {
            decoder.feed("data: " + std::string(11, 'x'));
            FAIL() << "an unterminated SSE line above the configured bound must fail";
        } catch (const std::runtime_error& error) {
            MINT_EXPECT(std::string(error.what()).find("SSE 行") != std::string::npos,
                        "an unterminated SSE line cannot grow indefinitely");
        }
    }

    {
        mint::ModelResponseLimits limits;
        limits.max_sse_line_bytes = 64;
        limits.max_sse_event_bytes = 8;
        mint::detail::ModelStreamDecoder decoder(mint::ModelAdapter::responses, {}, limits);
        try {
            decoder.feed("data: 123456789\n\n");
            FAIL() << "an SSE event above the configured bound must fail";
        } catch (const std::runtime_error& error) {
            MINT_EXPECT(std::string(error.what()).find("SSE 事件") != std::string::npos,
                        "SSE event accumulation has an independent bound");
        }
    }

    {
        mint::ModelResponseLimits limits;
        limits.max_sse_events = 1;
        mint::detail::ModelStreamDecoder decoder(mint::ModelAdapter::chat_completions, {}, limits);
        decoder.feed("data: [DONE]\n\n");
        EXPECT_THROW(decoder.feed("data: [DONE]\n\n"), std::runtime_error);
    }
}

TEST(ProviderProtocolContractTest, BoundsAccumulatedModelOutput) {
    const auto chat_delta = [](mint::Json delta) {
        return sse({{"choices", mint::Json::array({{{"delta", std::move(delta)}}})}});
    };

    {
        mint::ModelResponseLimits limits;
        limits.max_text_bytes = 5;
        mint::detail::ModelStreamDecoder decoder(mint::ModelAdapter::chat_completions, {}, limits);
        decoder.feed(chat_delta({{"content", "123"}}));
        EXPECT_THROW(decoder.feed(chat_delta({{"content", "456"}})), std::runtime_error);
    }

    {
        mint::ModelResponseLimits limits;
        limits.max_reasoning_bytes = 5;
        mint::detail::ModelStreamDecoder decoder(mint::ModelAdapter::chat_completions, {}, limits);
        decoder.feed(chat_delta({{"reasoning_content", "123"}}));
        EXPECT_THROW(decoder.feed(chat_delta({{"reasoning_content", "456"}})), std::runtime_error);
    }

    {
        mint::ModelResponseLimits limits;
        limits.max_tool_arguments_bytes = 5;
        mint::detail::ModelStreamDecoder decoder(mint::ModelAdapter::chat_completions, {}, limits);
        const auto arguments_delta = [&](std::string value) {
            return chat_delta(
                {{"tool_calls",
                  mint::Json::array(
                      {{{"index", 0}, {"function", {{"arguments", std::move(value)}}}}})}});
        };
        decoder.feed(arguments_delta("123"));
        EXPECT_THROW(decoder.feed(arguments_delta("456")), std::runtime_error);
    }

    {
        const mint::Json response = {
            {"choices", mint::Json::array({{{"message", {{"content", "123456"}}}}})}};
        mint::ModelResponseLimits limits;
        limits.max_text_bytes = 5;
        EXPECT_THROW((void)mint::detail::parse_provider_response(
                         mint::ModelAdapter::chat_completions, response, limits),
                     std::runtime_error);
    }

    const mint::Json first_call = {{"id", "one"},
                                   {"function", {{"name", "read_file"}, {"arguments", "{}"}}}};
    const mint::Json second_call = {{"id", "two"},
                                    {"function", {{"name", "read_file"}, {"arguments", "{}"}}}};
    const mint::Json response = {
        {"choices",
         mint::Json::array({{{"message",
                              {{"role", "assistant"},
                               {"content", nullptr},
                               {"tool_calls", mint::Json::array({first_call, second_call})}}}}})}};
    mint::ModelResponseLimits one_tool;
    one_tool.max_tool_calls = 1;
    EXPECT_THROW((void)mint::detail::parse_provider_response(mint::ModelAdapter::chat_completions,
                                                             response, one_tool),
                 std::runtime_error);
}

TEST(ProviderProtocolContractTest, DecodesChatCompletionsStream) {
    const mint::ModelProviderConfig config{.api_url = "https://example.test/chat",
                                           .model = "chat-test",
                                           .max_completion_tokens = 321,
                                           .adapter = mint::ModelAdapter::chat_completions,
                                           .stream = true};
    const auto messages = mint::Json::array(
        {{{"role", "system"}, {"content", "test"}},
         {{"role", "user"}, {"content", "inspect"}},
         {{"role", "assistant"},
          {"content", "old"},
          {"_mint_provider_state", {{"adapter", "responses"}, {"output", "private"}}}}});
    const auto request = mint::detail::build_provider_request(config, messages, tool_definitions());
    MINT_EXPECT(request.at("stream") && request.at("stream_options").at("include_usage"),
                "Chat streaming requests include final usage reporting");
    MINT_EXPECT(request.at("max_completion_tokens") == 321,
                "Chat requests keep the compatible completion limit field");
    MINT_EXPECT(!request.at("messages").at(2).contains("_mint_provider_state"),
                "provider-private state never leaks into Chat messages");

    std::vector<mint::ModelStreamEvent> deltas;
    mint::detail::ModelStreamDecoder decoder(
        mint::ModelAdapter::chat_completions,
        [&](const mint::ModelStreamEvent& event) { deltas.push_back(event); });
    std::string stream;
    stream += sse(
        {{"id", "chat_1"},
         {"model", "chat-test"},
         {"choices",
          mint::Json::array(
              {{{"delta",
                 {{"role", "assistant"}, {"reasoning_content", "先计划"}, {"content", "先"}}}}})}});
    const mint::Json first_tool_delta = {{"index", 0},
                                         {"id", "call_1"},
                                         {"type", "function"},
                                         {"function", {{"name", "read_"}, {"arguments", "{\"pa"}}}};
    stream +=
        sse({{"choices",
              mint::Json::array({{{"delta",
                                   {{"reasoning_content", "再调用"},
                                    {"content", "检查"},
                                    {"tool_calls", mint::Json::array({first_tool_delta})}}}}})}});
    const mint::Json second_tool_delta = {
        {"index", 0}, {"function", {{"name", "file"}, {"arguments", "th\":\"README.md\"}"}}}};
    stream += sse({{"choices",
                    mint::Json::array(
                        {{{"delta", {{"tool_calls", mint::Json::array({second_tool_delta})}}}}})}});
    stream += sse({{"choices", mint::Json::array()},
                   {"usage",
                    {{"prompt_tokens", 10},
                     {"completion_tokens", 4},
                     {"total_tokens", 14},
                     {"prompt_tokens_details", {{"cached_tokens", 6}}}}}});
    stream += "data: [DONE]\r\n\r\n";
    feed_fragmented(decoder, stream);

    const auto response = decoder.finish();
    const auto reply =
        mint::detail::parse_provider_response(mint::ModelAdapter::chat_completions, response);
    MINT_EXPECT(reply.text == "先检查" && reply.tool_calls.size() == 1,
                "fragmented Chat text and tool deltas form one canonical reply");
    MINT_EXPECT(reply.assistant_message.at("reasoning_content") == "先计划再调用",
                "Chat streaming retains provider reasoning needed by tool continuation");
    MINT_EXPECT(reply.tool_calls.at(0).id == "call_1" &&
                    reply.tool_calls.at(0).name == "read_file" &&
                    reply.tool_calls.at(0).arguments.at("path") == "README.md",
                "fragmented Chat function name and arguments are reassembled by index");
    MINT_EXPECT(reply.usage.available && reply.usage.prompt_tokens == 10 &&
                    reply.usage.completion_tokens == 4 && reply.usage.cached_tokens == 6,
                "Chat streaming keeps usage from the terminal usage chunk");
    MINT_EXPECT(decoder.event_count() == 4 && decoder.streamed_bytes() > 0,
                "stream decoder exposes body-free event and byte metrics");
    MINT_EXPECT(deltas.size() == 4 &&
                    deltas.front().kind == mint::ModelStreamEventKind::text_delta &&
                    deltas.back().kind == mint::ModelStreamEventKind::tool_arguments_delta,
                "Chat streaming emits text and function-argument deltas incrementally");
}

TEST(ProviderProtocolContractTest, RoundTripsResponsesTools) {
    const mint::ModelProviderConfig config{.api_url = "https://example.test/responses",
                                           .model = "responses-test",
                                           .max_completion_tokens = 456,
                                           .adapter = mint::ModelAdapter::responses};
    auto messages = mint::Json::array({{{"role", "system"}, {"content", "test"}},
                                       {{"role", "user"}, {"content", "read the file"}}});
    const auto first_request =
        mint::detail::build_provider_request(config, messages, tool_definitions());
    MINT_EXPECT(first_request.at("store") == false && first_request.at("max_output_tokens") == 456,
                "Responses requests are stateless and use max_output_tokens");
    MINT_EXPECT(first_request.at("include").at(0) == "reasoning.encrypted_content",
                "Responses requests preserve stateless reasoning continuation data");
    MINT_EXPECT(first_request.at("tools").at(0).at("type") == "function" &&
                    first_request.at("tools").at(0).at("name") == "read_file" &&
                    !first_request.at("tools").at(0).contains("function"),
                "Chat-shaped tool definitions are flattened for Responses");

    const mint::Json tool_response = {
        {"id", "resp_1"},
        {"object", "response"},
        {"status", "completed"},
        {"model", "responses-test"},
        {"output", mint::Json::array({{{"id", "rs_1"},
                                       {"type", "reasoning"},
                                       {"summary", mint::Json::array()},
                                       {"encrypted_content", "opaque"}},
                                      {{"id", "fc_1"},
                                       {"type", "function_call"},
                                       {"call_id", "call_1"},
                                       {"name", "read_file"},
                                       {"arguments", "{\"path\":\"README.md\"}"},
                                       {"status", "completed"}}})},
        {"usage",
         {{"input_tokens", 20},
          {"output_tokens", 8},
          {"total_tokens", 28},
          {"input_tokens_details", {{"cached_tokens", 12}}}}}};
    const auto tool_reply =
        mint::detail::parse_provider_response(mint::ModelAdapter::responses, tool_response);
    MINT_EXPECT(tool_reply.tool_calls.size() == 1 && tool_reply.tool_calls.at(0).id == "call_1" &&
                    tool_reply.tool_calls.at(0).arguments.at("path") == "README.md",
                "Responses function_call maps call_id into the canonical tool call");
    MINT_EXPECT(tool_reply.usage.prompt_tokens == 20 && tool_reply.usage.completion_tokens == 8 &&
                    tool_reply.usage.cached_tokens == 12,
                "Responses usage maps input/output/cached token names");

    messages.push_back(tool_reply.assistant_message);
    messages.push_back({{"role", "tool"},
                        {"tool_call_id", "call_1"},
                        {"content", R"({"ok":true,"content":"# Agent"})"}});
    const auto second_request =
        mint::detail::build_provider_request(config, messages, tool_definitions());
    const auto& input = second_request.at("input");
    MINT_EXPECT(input.size() == 5 && input.at(2).at("type") == "reasoning" &&
                    input.at(3).at("type") == "function_call" &&
                    input.at(4).at("type") == "function_call_output",
                "Responses continuation resends exact output items followed by tool output");
    MINT_EXPECT(input.at(4).at("call_id") == "call_1" &&
                    input.at(4).at("output").get<std::string>().find("# Agent") !=
                        std::string::npos,
                "Responses tool output preserves call linkage and result text");

    auto legacy_messages = messages;
    legacy_messages.at(2)["_aiagent_provider_state"] =
        legacy_messages.at(2).at("_mint_provider_state");
    legacy_messages.at(2).erase("_mint_provider_state");
    const auto legacy_request =
        mint::detail::build_provider_request(config, legacy_messages, tool_definitions());
    MINT_EXPECT(legacy_request.at("input") == input,
                "Responses continuation accepts provider state from existing checkpoints");

    const mint::Json final_response = {
        {"id", "resp_2"},
        {"status", "completed"},
        {"model", "responses-test"},
        {"output",
         mint::Json::array({{{"id", "msg_1"},
                             {"type", "message"},
                             {"role", "assistant"},
                             {"status", "completed"},
                             {"content", mint::Json::array({{{"type", "output_text"},
                                                             {"text", "README 已读取。"}}})}}})}};
    const auto final_reply =
        mint::detail::parse_provider_response(mint::ModelAdapter::responses, final_response);
    MINT_EXPECT(final_reply.text == "README 已读取。" && final_reply.tool_calls.empty(),
                "Responses output_text maps to the canonical final answer");

    bool failed_response_rejected = false;
    try {
        (void)mint::detail::parse_provider_response(
            mint::ModelAdapter::responses,
            {{"status", "failed"}, {"error", {{"message", "provider failed"}}}});
    } catch (const std::runtime_error& error) {
        failed_response_rejected =
            std::string(error.what()).find("provider failed") != std::string::npos;
    }
    MINT_EXPECT(failed_response_rejected, "Responses failed status cannot become an Agent reply");
}

TEST(ProviderProtocolContractTest, DecodesResponsesStream) {
    const mint::Json completed_response = {
        {"id", "resp_stream"},
        {"status", "completed"},
        {"model", "responses-test"},
        {"output", mint::Json::array({{{"id", "msg_stream"},
                                       {"type", "message"},
                                       {"role", "assistant"},
                                       {"status", "completed"},
                                       {"content", mint::Json::array({{{"type", "output_text"},
                                                                       {"text", "流式完成"}}})}}})},
        {"usage", {{"input_tokens", 5}, {"output_tokens", 3}, {"total_tokens", 8}}}};

    std::vector<mint::ModelStreamEvent> deltas;
    mint::detail::ModelStreamDecoder decoder(
        mint::ModelAdapter::responses,
        [&](const mint::ModelStreamEvent& event) { deltas.push_back(event); });
    std::string stream;
    stream += "event: response.created\n";
    stream += sse({{"type", "response.created"}, {"response", {{"id", "resp_stream"}}}});
    stream += sse({{"type", "response.output_text.delta"},
                   {"item_id", "msg_stream"},
                   {"output_index", 0},
                   {"content_index", 0},
                   {"delta", "流式"}});
    stream += sse({{"type", "response.output_text.delta"},
                   {"item_id", "msg_stream"},
                   {"output_index", 0},
                   {"content_index", 0},
                   {"delta", "完成"}});
    stream += sse({{"type", "response.function_call_arguments.delta"},
                   {"item_id", "fc_preview"},
                   {"output_index", 1},
                   {"delta", "{\"path\":"}});
    stream += sse({{"type", "response.completed"}, {"response", completed_response}});
    feed_fragmented(decoder, stream);
    const auto reply =
        mint::detail::parse_provider_response(mint::ModelAdapter::responses, decoder.finish());
    MINT_EXPECT(reply.text == "流式完成" && reply.usage.total_tokens == 8,
                "Responses stream uses the authoritative response.completed object");
    MINT_EXPECT(deltas.size() == 3 && deltas.at(0).kind == mint::ModelStreamEventKind::text_delta &&
                    deltas.at(2).kind == mint::ModelStreamEventKind::tool_arguments_delta,
                "Responses stream exposes text and function argument deltas");

    mint::detail::ModelStreamDecoder incomplete(mint::ModelAdapter::responses, {});
    incomplete.feed(sse({{"type", "response.output_text.delta"}, {"delta", "partial"}}));
    bool rejected = false;
    try {
        (void)incomplete.finish();
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("response.completed") != std::string::npos;
    }
    MINT_EXPECT(rejected, "Responses streams must end with response.completed");
}

TEST(ProviderTransportContractTest, StopsOversizedHttpBodiesWithoutLeakingThem) {
    const auto exercise = [](int status) {
        constexpr std::string_view marker = "SENSITIVE_PROVIDER_RESPONSE_BODY";
        const auto body =
            mint::Json({{"error", {{"message", std::string(80, 'x') + std::string(marker)}}}})
                .dump();
        ScriptedHttpServer server({{.status = status, .body = body}});
        mint::ModelResponseLimits limits;
        limits.max_http_body_bytes = 64;
        mint::ModelProviderClient client({.api_url = server.url("/v1/chat/completions"),
                                          .model = "body-limit-test",
                                          .connect_timeout_seconds = 2,
                                          .request_timeout_seconds = 2,
                                          .max_retries = 0,
                                          .response_limits = limits});
        std::string failure;
        try {
            (void)client.complete(mint::Json::array({{{"role", "user"}, {"content", "limit"}}}),
                                  mint::Json::array());
        } catch (const std::runtime_error& error) {
            failure = error.what();
        }
        server.wait();
        MINT_EXPECT(failure.find("HTTP 响应正文超过资源上限") != std::string::npos,
                    "curl surfaces an explicit response-size failure");
        MINT_EXPECT(failure.find(marker) == std::string::npos,
                    "an oversized provider body never appears in the surfaced error");
    };

    exercise(200);
    exercise(413);
}

TEST(ProviderTransportContractTest, StopsOversizedSuccessfulSseBodies) {
    std::string body;
    for (int event = 0; event < 20; ++event) {
        body += "data: {}\n\n";
    }
    auto response = sse_response(std::move(body));
    response.fragment_bytes = 17;
    response.allow_client_disconnect = true;
    ScriptedHttpServer server({std::move(response)});

    mint::ModelResponseLimits limits;
    limits.max_http_body_bytes = 64;
    mint::ModelProviderClient client({.api_url = server.url("/v1/responses"),
                                      .model = "sse-body-limit-test",
                                      .connect_timeout_seconds = 2,
                                      .request_timeout_seconds = 2,
                                      .max_retries = 0,
                                      .adapter = mint::ModelAdapter::responses,
                                      .stream = true,
                                      .response_limits = limits});
    std::string failure;
    try {
        (void)client.complete(mint::Json::array({{{"role", "user"}, {"content", "limit"}}}),
                              mint::Json::array());
    } catch (const std::runtime_error& error) {
        failure = error.what();
    }
    server.wait();
    MINT_EXPECT(failure.find("HTTP 响应正文超过资源上限") != std::string::npos,
                "the HTTP body budget also caps successful SSE streams");
}

TEST(ProviderTransportContractTest, BypassesEnvironmentProxyForPlaintextLoopback) {
    const auto response = [](std::string text) {
        return mint::Json{
            {"id", "chat_proxy_test"},
            {"model", "proxy-test"},
            {"choices",
             mint::Json::array({{{"message", {{"role", "assistant"}, {"content", text}}}}})},
            {"usage", {{"prompt_tokens", 1}, {"completion_tokens", 1}, {"total_tokens", 2}}}};
    };
    ScriptedHttpServer direct({{.body = response("direct").dump()}});
    ScriptedHttpServer proxy({{.body = response("proxy").dump()}});
    auto proxy_url = proxy.url("/");
    proxy_url.pop_back();

    ScopedEnvironmentOverride http_proxy("http_proxy", proxy_url);
    ScopedEnvironmentOverride no_proxy("no_proxy", "");
#if !defined(_WIN32)
    ScopedEnvironmentOverride uppercase_http_proxy("HTTP_PROXY", proxy_url);
    ScopedEnvironmentOverride uppercase_no_proxy("NO_PROXY", "");
#endif

    mint::ModelProviderClient client({.api_url = direct.url("/v1/chat/completions"),
                                      .api_key = "must-not-reach-environment-proxy",
                                      .model = "proxy-test",
                                      .connect_timeout_seconds = 2,
                                      .request_timeout_seconds = 2,
                                      .max_retries = 0});
    const auto reply =
        client.complete(mint::Json::array({{{"role", "user"}, {"content", "private prompt"}}}),
                        mint::Json::array());
    ASSERT_EQ(reply.text, "direct");
    direct.wait();
    MINT_EXPECT(direct.request().find("must-not-reach-environment-proxy") != std::string::npos,
                "loopback credentials reach the direct local service");
}

TEST(ProviderTransportContractTest, StopsUnterminatedSseLines) {
    ScriptedHttpServer server({sse_response("data: " + std::string(80, 'x'))});
    mint::ModelResponseLimits limits;
    limits.max_sse_line_bytes = 64;
    mint::ModelProviderClient client({.api_url = server.url("/v1/responses"),
                                      .model = "sse-line-limit-test",
                                      .connect_timeout_seconds = 2,
                                      .request_timeout_seconds = 2,
                                      .max_retries = 0,
                                      .adapter = mint::ModelAdapter::responses,
                                      .stream = true,
                                      .response_limits = limits});
    std::string failure;
    try {
        (void)client.complete(mint::Json::array({{{"role", "user"}, {"content", "limit"}}}),
                              mint::Json::array());
    } catch (const std::runtime_error& error) {
        failure = error.what();
    }
    server.wait();
    MINT_EXPECT(failure.find("SSE 行") != std::string::npos,
                "curl stops an unterminated SSE line at the configured budget");
}

TEST(ProviderTransportContractTest, StreamsHttpResponses) {
    const mint::Json completed_response = {
        {"id", "resp_http"},
        {"status", "completed"},
        {"model", "responses-http-test"},
        {"output",
         mint::Json::array({{{"id", "msg_http"},
                             {"type", "message"},
                             {"role", "assistant"},
                             {"status", "completed"},
                             {"content", mint::Json::array({{{"type", "output_text"},
                                                             {"text", "HTTP 流式完成"}}})}}})},
        {"usage", {{"input_tokens", 9}, {"output_tokens", 4}, {"total_tokens", 13}}}};
    std::string body;
    body += sse({{"type", "response.created"}, {"response", {{"id", "resp_http"}}}});
    body += sse({{"type", "response.output_text.delta"},
                 {"item_id", "msg_http"},
                 {"output_index", 0},
                 {"content_index", 0},
                 {"delta", "HTTP "}});
    body += sse({{"type", "response.output_text.delta"},
                 {"item_id", "msg_http"},
                 {"output_index", 0},
                 {"content_index", 0},
                 {"delta", "流式完成"}});
    body += sse({{"type", "response.completed"}, {"response", completed_response}});
    ScriptedHttpServer server({sse_response(std::move(body))});

    std::vector<mint::ModelProgress> progress;
    std::string streamed_text;
    mint::ModelProviderClient client(
        {.api_url = server.url("/v1/responses"),
         .model = "responses-http-test",
         .connect_timeout_seconds = 2,
         .request_timeout_seconds = 2,
         .max_retries = 0,
         .progress = [&](const mint::ModelProgress& event) { progress.push_back(event); },
         .adapter = mint::ModelAdapter::responses,
         .stream = true,
         .stream_event =
             [&](const mint::ModelStreamEvent& event) {
                 if (event.kind == mint::ModelStreamEventKind::text_delta) {
                     streamed_text += event.delta;
                 }
             }});
    const auto reply =
        client.complete(mint::Json::array({{{"role", "system"}, {"content", "test"}},
                                           {{"role", "user"}, {"content", "stream"}}}),
                        mint::Json::array());
    server.wait();

    MINT_EXPECT(reply.text == "HTTP 流式完成" && streamed_text == reply.text,
                "libcurl transport delivers Responses deltas before the canonical reply");
    MINT_EXPECT(reply.metadata.adapter == "responses" && reply.metadata.streamed &&
                    reply.metadata.stream_events == 4 && reply.metadata.streamed_bytes > 0 &&
                    reply.metadata.http_status == 200,
                "streaming transport records adapter, event, byte and HTTP metadata");
    MINT_EXPECT(progress.size() == 4 &&
                    progress.at(0).kind == mint::ModelProgressKind::attempt_started &&
                    progress.at(1).kind == mint::ModelProgressKind::stream_started &&
                    progress.at(2).kind == mint::ModelProgressKind::stream_completed &&
                    progress.at(3).kind == mint::ModelProgressKind::request_succeeded,
                "streaming transport reports a stable progress lifecycle");
    MINT_EXPECT(server.request().find("POST /v1/responses HTTP/1.1") != std::string::npos,
                "HTTP transport targets the configured Responses endpoint");
    MINT_EXPECT(server.request().find(R"("stream":true)") != std::string::npos,
                "HTTP transport enables Responses streaming");
    MINT_EXPECT(server.request().find(R"("store":false)") != std::string::npos,
                "HTTP transport disables remote Responses storage");
    MINT_EXPECT(server.request().find(R"("max_output_tokens":1024)") != std::string::npos,
                "HTTP transport sends the Responses output token limit");
}

TEST(ProviderTransportContractTest, LearnsRequestTokenLimitWithoutLooseningExplicitBudget) {
    const mint::Json response = {
        {"id", "chat_limit"},
        {"model", "chat-limit-test"},
        {"choices", mint::Json::array({{{"message", {{"role", "assistant"}, {"content", "ok"}}}}})},
        {"usage", {{"prompt_tokens", 4}, {"completion_tokens", 1}, {"total_tokens", 5}}}};
    const auto response_with_limit = [&] {
        return mint::test::ScriptedHttpResponse{.headers = {{"X-RateLimit-Limit-Tokens", "8000"}},
                                                .body = response.dump()};
    };
    const auto messages =
        mint::Json::array({{{"role", "user"}, {"content", "report the request limit"}}});
    const auto tools = tool_definitions();

    {
        ScriptedHttpServer server({response_with_limit()});
        mint::ModelProviderClient client({.api_url = server.url("/v1/chat/completions"),
                                          .model = "chat-limit-test",
                                          .connect_timeout_seconds = 2,
                                          .request_timeout_seconds = 2,
                                          .max_retries = 0,
                                          .adapter = mint::ModelAdapter::chat_completions});
        const auto reply = client.complete(messages, tools);
        server.wait();

        MINT_EXPECT(reply.metadata.request_token_limit == 8'000,
                    "HTTP metadata retains the provider token-rate ceiling");
        const auto limits = client.request_limits(tools);
        MINT_EXPECT(limits.max_request_tokens == 8'000 &&
                        limits.max_request_tokens_source ==
                            mint::ModelRequestLimitSource::response_header &&
                        limits.response_header_max_request_tokens == 8'000,
                    "subsequent requests use the provider-advertised token ceiling");
    }

    {
        ScriptedHttpServer server({response_with_limit()});
        mint::ModelProviderClient client({.api_url = server.url("/v1/chat/completions"),
                                          .model = "chat-limit-test",
                                          .connect_timeout_seconds = 2,
                                          .request_timeout_seconds = 2,
                                          .max_retries = 0,
                                          .adapter = mint::ModelAdapter::chat_completions,
                                          .max_request_tokens = 6'000});
        const auto reply = client.complete(messages, tools);
        server.wait();

        MINT_EXPECT(reply.metadata.request_token_limit == 8'000,
                    "explicit-budget requests still expose provider limit metadata");
        const auto limits = client.request_limits(tools);
        MINT_EXPECT(limits.max_request_tokens == 6'000 &&
                        limits.max_request_tokens_source == mint::ModelRequestLimitSource::config &&
                        limits.response_header_max_request_tokens == 8'000,
                    "a larger provider ceiling never widens an explicit local budget");
    }
}

TEST(ProviderTransportContractTest, RejectsLearnedTokenLimitBelowReservedBudget) {
    const mint::Json response = {
        {"id", "chat_small_limit"},
        {"model", "chat-small-limit-test"},
        {"choices", mint::Json::array({{{"message", {{"role", "assistant"}, {"content", "ok"}}}}})},
        {"usage", {{"prompt_tokens", 4}, {"completion_tokens", 1}, {"total_tokens", 5}}}};
    ScriptedHttpServer server(
        {{.headers = {{"X-RateLimit-Limit-Tokens", "512"}}, .body = response.dump()}});
    const auto messages =
        mint::Json::array({{{"role", "user"}, {"content", "report the request limit"}}});
    const auto tools = tool_definitions();
    mint::ModelProviderClient client({.api_url = server.url("/v1/chat/completions"),
                                      .model = "chat-small-limit-test",
                                      .connect_timeout_seconds = 2,
                                      .request_timeout_seconds = 2,
                                      .max_retries = 0,
                                      .adapter = mint::ModelAdapter::chat_completions});

    const auto first = client.complete(messages, tools);
    server.wait();

    MINT_EXPECT(first.metadata.request_token_limit == 512,
                "HTTP metadata retains a small provider token ceiling");
    const auto learned_limits = client.request_limits(tools);
    MINT_EXPECT(learned_limits.available_request_tokens() == 0 &&
                    learned_limits.max_request_tokens_source ==
                        mint::ModelRequestLimitSource::response_header &&
                    learned_limits.response_header_max_request_tokens == 512,
                "a learned ceiling below reserved tokens saturates at zero");
    EXPECT_THROW((void)client.complete(messages, tools), std::runtime_error);
}

TEST(ProviderRequestBudgetContractTest, RejectsDenseUtf8BeforeStartingHttp) {
    mint::ModelProviderClient client({.api_url = "http://127.0.0.1:1/v1/chat/completions",
                                      .model = "dense-input-test",
                                      .connect_timeout_seconds = 1,
                                      .request_timeout_seconds = 1,
                                      .max_retries = 0,
                                      .max_completion_tokens = 128,
                                      .adapter = mint::ModelAdapter::chat_completions,
                                      .max_request_tokens = 512,
                                      .request_token_safety_margin = 128});
    std::string dense_text;
    for (int index = 0; index < 160; ++index) {
        dense_text += "中";
    }
    const auto dense_messages =
        mint::Json::array({{{"role", "user"}, {"content", std::move(dense_text)}}});

    try {
        (void)client.complete(dense_messages, mint::Json::array());
        FAIL() << "dense UTF-8 request should fail the local token budget";
    } catch (const std::runtime_error& error) {
        MINT_EXPECT(std::string_view(error.what()).find("Token 预算") != std::string_view::npos,
                    "dense UTF-8 input is rejected locally before HTTP starts");
    }
}

TEST(ProviderTransportContractTest, RetriesOnlyExplicitFailedGenerationBadRequests) {
    const mint::Json failed_generation = {{"error",
                                           {{"message", "Invalid tool call generated"},
                                            {"type", "invalid_request_error"},
                                            {"failed_generation",
                                             {{"reason", "Tool call arguments are not valid JSON"},
                                              {"attempted_arguments", "{'path': 'README.md'}"}}}}}};
    const mint::Json success = {
        {"id", "chat_tool_retry"},
        {"model", "chat-tool-retry-test"},
        {"choices",
         mint::Json::array({{{"message", {{"role", "assistant"}, {"content", "recovered"}}}}})},
        {"usage", {{"prompt_tokens", 9}, {"completion_tokens", 2}, {"total_tokens", 11}}}};
    ScriptedHttpServer server(
        {{.status = 400, .body = failed_generation.dump()}, {.body = success.dump()}});
    mint::ModelProviderClient client({.api_url = server.url("/v1/chat/completions"),
                                      .model = "chat-tool-retry-test",
                                      .connect_timeout_seconds = 2,
                                      .request_timeout_seconds = 2,
                                      .max_retries = 1,
                                      .retry_initial_delay_ms = 1,
                                      .adapter = mint::ModelAdapter::chat_completions,
                                      .provider = mint::ModelProvider::groq});

    const auto reply = client.complete(
        mint::Json::array({{{"role", "user"}, {"content", "read the project README"}}}),
        tool_definitions());
    server.wait();

    MINT_EXPECT(reply.text == "recovered" && reply.metadata.retries == 1,
                "a structured failed_generation 400 retries once and can recover");
    MINT_EXPECT(server.request_count() == 2,
                "failed tool-call generation uses the configured retry budget");
    MINT_EXPECT(server.request(0).find(R"("tool_choice":"auto")") != std::string::npos &&
                    server.request(0).find("failed tool call did not match") == std::string::npos,
                "the first request keeps the configured provider contract");
    MINT_EXPECT(server.request(1).find(R"("tool_choice":"required")") != std::string::npos &&
                    server.request(1).find(R"("temperature":0.0)") != std::string::npos &&
                    server.request(1).find("previous tool call did not match") != std::string::npos,
                "the retry steers Groq toward one schema-valid tool call");
    MINT_EXPECT(server.request(1).find("attempted_arguments") == std::string::npos &&
                    server.request(1).find("{'path': 'README.md'}") == std::string::npos,
                "the provider's failed generation is never replayed into model context");
}

TEST(ProviderTransportContractTest, RelaxesRepeatedGenerationFailureToAutoChoice) {
    const mint::Json failed_generation = {
        {"error",
         {{"message", "Invalid tool call generated"},
          {"type", "invalid_request_error"},
          {"failed_generation", {{"reason", "Tool call arguments are not valid JSON"}}}}}};
    const mint::Json success = {
        {"id", "chat_verified_final_retry"},
        {"model", "chat-verified-final-retry-test"},
        {"choices", mint::Json::array(
                        {{{"message", {{"role", "assistant"}, {"content", "verified summary"}}}}})},
        {"usage", {{"prompt_tokens", 9}, {"completion_tokens", 2}, {"total_tokens", 11}}}};
    ScriptedHttpServer server({{.status = 400, .body = failed_generation.dump()},
                               {.status = 400, .body = failed_generation.dump()},
                               {.body = success.dump()}});
    mint::ModelProviderClient client({.api_url = server.url("/v1/chat/completions"),
                                      .model = "chat-verified-final-retry-test",
                                      .connect_timeout_seconds = 2,
                                      .request_timeout_seconds = 2,
                                      .max_retries = 2,
                                      .retry_initial_delay_ms = 1,
                                      .adapter = mint::ModelAdapter::chat_completions,
                                      .provider = mint::ModelProvider::groq});
    const auto messages =
        mint::Json::array({{{"role", "user"}, {"content", "repair the project"}}});

    const auto reply = client.complete(messages, tool_definitions());
    server.wait();

    MINT_EXPECT(reply.text == "verified summary" && reply.metadata.retries == 2,
                "repeated invalid tool generation can recover with a final answer");
    MINT_EXPECT(server.request_count() == 3,
                "the fallback uses the remaining configured retry attempt");
    MINT_EXPECT(server.request(0).find(R"("tool_choice":"auto")") != std::string::npos,
                "the first request keeps normal tool selection");
    MINT_EXPECT(server.request(1).find(R"("tool_choice":"required")") != std::string::npos,
                "the first correction still preserves a legitimate tool continuation");
    MINT_EXPECT(server.request(2).find(R"("tools":)") != std::string::npos &&
                    server.request(2).find(R"("tool_choice":"auto")") != std::string::npos &&
                    server.request(2).find("previous response still could not be parsed") !=
                        std::string::npos &&
                    server.request(2).find("previous tool call did not match") == std::string::npos,
                "the second correction keeps tools, restores auto choice, and removes the "
                "conflicting required-tool instruction");
}

TEST(ProviderTransportContractTest, DoesNotRetryExcludedFailedGenerationResponses) {
    const mint::Json marker = {{"error",
                                {{"message", "SENSITIVE_GENERATED_ARGUMENTS"},
                                 {"type", "invalid_request_error"},
                                 {"failed_generation", "SENSITIVE_GENERATED_ARGUMENTS"}}}};
    struct Scenario {
        std::string_view name;
        int status;
        mint::ModelProvider provider;
        mint::Json body;
        mint::Json tools;
        long max_retries;
    };
    const std::vector<Scenario> scenarios = {
        {"ordinary 400",
         400,
         mint::ModelProvider::groq,
         {{"error", {{"message", "ordinary invalid request"}, {"type", "invalid_request_error"}}}},
         tool_definitions(),
         1},
        {"non-Groq provider", 400, mint::ModelProvider::openai, marker, tool_definitions(), 1},
        {"request without tools", 400, mint::ModelProvider::groq, marker, mint::Json::array(), 1},
        {"null marker",
         400,
         mint::ModelProvider::groq,
         {{"error", {{"message", "null failed generation"}, {"failed_generation", nullptr}}}},
         tool_definitions(),
         1},
        {"empty marker",
         400,
         mint::ModelProvider::groq,
         {{"error",
           {{"message", "empty failed generation"}, {"failed_generation", mint::Json::object()}}}},
         tool_definitions(),
         1},
        {"contradictory error code",
         400,
         mint::ModelProvider::groq,
         {{"error",
           {{"code", "unsupported_parameter"},
            {"failed_generation", "SENSITIVE_GENERATED_ARGUMENTS"}}}},
         tool_definitions(),
         1},
        {"exhausted retry budget", 400, mint::ModelProvider::groq, marker, tool_definitions(), 0},
        {"non-400 status", 401, mint::ModelProvider::groq, marker, tool_definitions(), 1}};

    for (const auto& scenario : scenarios) {
        SCOPED_TRACE(scenario.name);
        ScriptedHttpServer server({{.status = scenario.status, .body = scenario.body.dump()}});
        std::vector<mint::ModelProgress> progress;
        mint::ModelProviderClient client(
            {.api_url = server.url("/v1/chat/completions"),
             .model = "chat-non-retryable-400-test",
             .connect_timeout_seconds = 1,
             .request_timeout_seconds = 1,
             .max_retries = scenario.max_retries,
             .retry_initial_delay_ms = 1,
             .progress = [&](const mint::ModelProgress& event) { progress.push_back(event); },
             .adapter = mint::ModelAdapter::chat_completions,
             .provider = scenario.provider});

        std::string failure;
        try {
            (void)client.complete(
                mint::Json::array({{{"role", "user"}, {"content", "send an invalid request"}}}),
                scenario.tools);
            FAIL() << "non-retryable response should fail";
        } catch (const std::runtime_error& error) {
            failure = error.what();
        }
        server.wait();

        MINT_EXPECT(server.request_count() == 1,
                    "excluded failed_generation responses fail after one request");
        MINT_EXPECT(std::none_of(progress.begin(), progress.end(),
                                 [](const auto& event) {
                                     return event.kind == mint::ModelProgressKind::retry_scheduled;
                                 }),
                    "excluded responses do not schedule a retry");
        MINT_EXPECT(failure.find("SENSITIVE_GENERATED_ARGUMENTS") == std::string::npos,
                    "failed_generation content is not retained in the surfaced error");
    }
}

TEST(ProviderTransportContractTest, RetriesStreamingHttpFailures) {
    const auto rate_limit_stream =
        sse({{"type", "error"}, {"message", "transient stream rate limit"}});
    const mint::Json completed_response = {
        {"id", "resp_retry"},
        {"status", "completed"},
        {"model", "responses-retry-test"},
        {"output",
         mint::Json::array({{{"id", "msg_retry"},
                             {"type", "message"},
                             {"role", "assistant"},
                             {"status", "completed"},
                             {"content", mint::Json::array({{{"type", "output_text"},
                                                             {"text", "retry completed"}}})}}})}};
    const auto success_stream =
        sse({{"type", "response.completed"}, {"response", completed_response}});
    ScriptedHttpServer server({sse_response(rate_limit_stream, 429), sse_response(success_stream)});

    std::vector<mint::ModelProgress> progress;
    mint::ModelProviderClient client(
        {.api_url = server.url("/v1/responses"),
         .model = "responses-retry-test",
         .connect_timeout_seconds = 2,
         .request_timeout_seconds = 2,
         .max_retries = 1,
         .retry_initial_delay_ms = 1,
         .progress = [&](const mint::ModelProgress& event) { progress.push_back(event); },
         .adapter = mint::ModelAdapter::responses,
         .stream = true});
    const auto reply = client.complete(
        mint::Json::array({{{"role", "user"}, {"content", "retry"}}}), mint::Json::array());
    server.wait();
    MINT_EXPECT(reply.text == "retry completed" && reply.metadata.retries == 1,
                "streaming client retries an SSE-formatted HTTP 429 response");
    MINT_EXPECT(server.request(0).find(R"("stream":true)") != std::string::npos &&
                    server.request(1).find(R"("stream":true)") != std::string::npos,
                "streaming retry sends the same protocol contract on both attempts");
    MINT_EXPECT(progress.size() == 7 &&
                    progress.at(2).kind == mint::ModelProgressKind::retry_scheduled &&
                    progress.at(2).http_status == 429 &&
                    progress.at(5).kind == mint::ModelProgressKind::stream_completed &&
                    progress.at(6).kind == mint::ModelProgressKind::request_succeeded,
                "SSE HTTP errors follow retry progress instead of becoming parser failures");
}

TEST(ProviderCliContractTest, CompletesResponsesStreamingToolLoop) {
    const mint::Json tool_response = {
        {"id", "resp_cli_1"},
        {"status", "completed"},
        {"model", "responses-cli-test"},
        {"output", mint::Json::array({{{"id", "fc_cli"},
                                       {"type", "function_call"},
                                       {"call_id", "call_cli"},
                                       {"name", "read_file"},
                                       {"arguments", "{\"path\":\"README.md\"}"},
                                       {"status", "completed"}}})},
        {"usage", {{"input_tokens", 12}, {"output_tokens", 5}, {"total_tokens", 17}}}};
    std::string first_stream;
    first_stream += sse({{"type", "response.function_call_arguments.delta"},
                         {"item_id", "fc_cli"},
                         {"output_index", 0},
                         {"delta", "{\"path\":\"README.md\"}"}});
    first_stream += sse({{"type", "response.completed"}, {"response", tool_response}});

    const mint::Json final_response = {
        {"id", "resp_cli_2"},
        {"status", "completed"},
        {"model", "responses-cli-test"},
        {"output",
         mint::Json::array(
             {{{"id", "msg_cli"},
               {"type", "message"},
               {"role", "assistant"},
               {"status", "completed"},
               {"content", mint::Json::array({{{"type", "output_text"},
                                               {"text", "README 证明项目是本地 Agent。"}}})}}})},
        {"usage", {{"input_tokens", 18}, {"output_tokens", 7}, {"total_tokens", 25}}}};
    std::string second_stream;
    second_stream += sse({{"type", "response.output_text.delta"},
                          {"item_id", "msg_cli"},
                          {"output_index", 0},
                          {"content_index", 0},
                          {"delta", "README 证明"}});
    second_stream += sse({{"type", "response.output_text.delta"},
                          {"item_id", "msg_cli"},
                          {"output_index", 0},
                          {"content_index", 0},
                          {"delta", "项目是本地 Agent。"}});
    second_stream += sse({{"type", "response.completed"}, {"response", final_response}});
    auto first_response = sse_response(std::move(first_stream));
    first_response.headers = {{"X-RateLimit-Limit-Tokens", "6800"}};
    auto second_response = sse_response(std::move(second_stream));
    second_response.headers = {{"X-RateLimit-Limit-Tokens", "6800"}};
    ScriptedHttpServer server({std::move(first_response), std::move(second_response)});

    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    std::filesystem::create_directories(workspace);
    write_text(workspace / "README.md", "# Local Agent\n");
    const auto config_path = temporary.path() / "config.json";
    write_text(config_path, mint::Json({{"adapter", "responses"},
                                        {"api_url", server.url("/v1/responses")},
                                        {"api_key", ""},
                                        {"model", "responses-cli-test"},
                                        {"connect_timeout_seconds", 2},
                                        {"request_timeout_seconds", 2},
                                        {"max_retries", 0},
                                        {"max_completion_tokens", 128},
                                        {"stream", true}})
                                .dump(2));

    mint::cli::CommandLine command_line;
    command_line.json_output = true;
    command_line.config = config_path;
    command_line.config_specified = true;
    command_line.root = workspace;
    command_line.root_specified = true;
    command_line.question = "读取 README.md 后用一句话说明项目用途";
    std::optional<mint::ManagedTaskPaths> managed_task;
    std::istringstream input;
    std::ostringstream output_stream;
    std::ostringstream error_stream;
    mint::cli::Console console(input, output_stream, error_stream);
    const auto exit_code =
        mint::cli::run_agent_command(std::move(command_line), managed_task, console);
    server.wait();
    const auto output = output_stream.str();
    MINT_EXPECT(exit_code == 0, "streaming Responses CLI exits successfully: " + output);
    MINT_EXPECT(error_stream.str().empty(),
                "streaming Responses CLI keeps stderr empty: " + error_stream.str());

    mint::Json result;
    ASSERT_NO_THROW(result = mint::Json::parse(output))
        << "CLI stdout is not one JSON document: " << output;
    MINT_EXPECT(result.at("status") == "completed" && result.at("turns") == 2 &&
                    result.at("execution").at("tool_calls") == 1,
                "CLI completes a two-turn Responses tool loop");
    MINT_EXPECT(result.at("answer") == "README 证明项目是本地 Agent。",
                "CLI keeps the authoritative streamed final answer");
    MINT_EXPECT(result.at("model").at("provider") == "custom" &&
                    result.at("model").at("adapter") == "responses" &&
                    result.at("model").at("streamed_calls") == 2 &&
                    result.at("model").at("stream_events") == 5 &&
                    result.at("model").at("streamed_bytes").get<std::size_t>() > 0,
                "CLI JSON reports provider, Responses adapter, and aggregate stream metrics");
    MINT_EXPECT(result.at("model").at("max_request_tokens") == 6800 &&
                    result.at("model").at("max_request_tokens_source") == "response_header" &&
                    result.at("model").at("response_header_max_request_tokens") == 6800 &&
                    result.at("model").at("request_token_estimate_bytes_per_token") == 2,
                "CLI JSON retains the Agent's final effective request budget");
    MINT_EXPECT(server.request(0).find(R"("tools":[{"description")") != std::string::npos &&
                    server.request(0).find(R"("name":"read_file")") != std::string::npos,
                "first CLI request advertises flattened Responses tools");
    MINT_EXPECT(server.request(1).find(R"("type":"function_call_output")") != std::string::npos &&
                    server.request(1).find(R"("call_id":"call_cli")") != std::string::npos &&
                    server.request(1).find("# Local Agent") != std::string::npos,
                "second CLI request returns the real tool result with call linkage");
}

TEST(ProviderCliContractTest, RunsSanitizedLiveProviderAcceptance) {
    constexpr auto challenge = "mint-provider-acceptance-v1";
    constexpr auto receipt = "MINT_PROVIDER_ACCEPTANCE_OK";
    constexpr auto api_key = "acceptance-test-secret";

    const mint::Json tool_response = {
        {"id", "resp_acceptance_1"},
        {"status", "completed"},
        {"model", "responses-acceptance-test"},
        {"output", mint::Json::array({{{"id", "fc_acceptance"},
                                       {"type", "function_call"},
                                       {"call_id", "call_acceptance"},
                                       {"name", "mint_acceptance_echo"},
                                       {"arguments", mint::Json({{"challenge", challenge}}).dump()},
                                       {"status", "completed"}}})},
        {"usage", {{"input_tokens", 7}, {"output_tokens", 3}, {"total_tokens", 10}}}};
    std::string first_stream;
    first_stream += sse({{"type", "response.function_call_arguments.delta"},
                         {"item_id", "fc_acceptance"},
                         {"output_index", 0},
                         {"delta", mint::Json({{"challenge", challenge}}).dump()}});
    first_stream += sse({{"type", "response.completed"}, {"response", tool_response}});

    const mint::Json final_response = {
        {"id", "resp_acceptance_2"},
        {"status", "completed"},
        {"model", "responses-acceptance-test"},
        {"output",
         mint::Json::array(
             {{{"id", "msg_acceptance"},
               {"type", "message"},
               {"role", "assistant"},
               {"status", "completed"},
               {"content", mint::Json::array({{{"type", "output_text"}, {"text", receipt}}})}}})},
        {"usage", {{"input_tokens", 12}, {"output_tokens", 2}, {"total_tokens", 14}}}};
    std::string second_stream;
    second_stream += sse({{"type", "response.output_text.delta"},
                          {"item_id", "msg_acceptance"},
                          {"output_index", 0},
                          {"content_index", 0},
                          {"delta", receipt}});
    second_stream += sse({{"type", "response.completed"}, {"response", final_response}});
    auto first_response = sse_response(std::move(first_stream));
    first_response.headers = {{"X-RateLimit-Limit-Tokens", "7000"}};
    auto second_response = sse_response(std::move(second_stream));
    second_response.headers = {{"X-RateLimit-Limit-Tokens", "7000"}};
    ScriptedHttpServer server({std::move(first_response), std::move(second_response)});

    TemporaryDirectory temporary;
    const auto config_path = temporary.path() / "provider.json";
    write_text(config_path, mint::Json({{"adapter", "responses"},
                                        {"api_url", server.url("/v1/responses")},
                                        {"api_key", api_key},
                                        {"model", "responses-acceptance-test"},
                                        {"connect_timeout_seconds", 2},
                                        {"request_timeout_seconds", 2},
                                        {"max_retries", 4},
                                        {"max_completion_tokens", 4096},
                                        {"stream", true}})
                                .dump(2));

    mint::cli::CommandLine command_line;
    command_line.mode = mint::cli::CommandMode::provider;
    command_line.provider_action = mint::cli::ProviderCommandAction::test;
    command_line.config = config_path;
    command_line.config_specified = true;
    command_line.json_output = true;
    std::istringstream input;
    std::ostringstream output_stream;
    std::ostringstream error_stream;
    mint::cli::Console console(input, output_stream, error_stream);
    const auto exit_code = mint::cli::run_provider_command(command_line, console);
    server.wait();
    const auto output = output_stream.str();
    MINT_EXPECT(exit_code == 0, "provider acceptance exits successfully: " + output);
    MINT_EXPECT(error_stream.str().empty(),
                "provider acceptance keeps stderr empty: " + error_stream.str());

    mint::Json result;
    ASSERT_NO_THROW(result = mint::Json::parse(output))
        << "provider acceptance stdout is not one JSON document: " << output;
    MINT_EXPECT(result.at("operation") == "test" && result.at("status") == "passed" &&
                    result.at("provider") == "custom" && result.at("adapter") == "responses",
                "provider acceptance reports the tested protocol profile");
    MINT_EXPECT(result.at("limits").at("max_completion_tokens") == 1024 &&
                    result.at("limits").at("max_attempts_per_request") == 1,
                "provider acceptance caps output and disables retry spend");
    MINT_EXPECT(result.at("limits").at("max_request_tokens") == 7000 &&
                    result.at("limits").at("max_request_tokens_source") == "response_header" &&
                    result.at("limits").at("response_header_max_request_tokens") == 7000 &&
                    result.at("limits").at("request_token_estimate_bytes_per_token") == 2,
                "provider acceptance reports the effective header-learned request budget");
    const auto& acceptance = result.at("acceptance");
    MINT_EXPECT(acceptance.at("requests") == 2 && acceptance.at("attempts") == 2 &&
                    acceptance.at("retries") == 0 && acceptance.at("streamed_requests") == 2 &&
                    acceptance.at("stream_events") == 4,
                "provider acceptance reports the two streamed requests");
    MINT_EXPECT(acceptance.at("usage").at("reported_requests") == 2 &&
                    acceptance.at("usage").at("total_tokens") == 24 &&
                    acceptance.at("checks").at("function_call") &&
                    acceptance.at("checks").at("arguments_round_trip") &&
                    acceptance.at("checks").at("tool_result_continuation"),
                "provider acceptance proves tool calling, continuation, and usage parsing");
    MINT_EXPECT(output.find(api_key) == std::string::npos &&
                    output.find(receipt) == std::string::npos &&
                    output.find(challenge) == std::string::npos,
                "provider acceptance output excludes credentials and raw prompt or model content");
    MINT_EXPECT(server.request(0).find(R"("name":"mint_acceptance_echo")") != std::string::npos &&
                    server.request(0).find(challenge) != std::string::npos &&
                    server.request(0).find(R"("max_output_tokens":1024)") != std::string::npos,
                "first provider acceptance request advertises the fixed low-cost tool contract");
    MINT_EXPECT(server.request(1).find(R"("type":"function_call_output")") != std::string::npos &&
                    server.request(1).find(R"("call_id":"call_acceptance")") != std::string::npos &&
                    server.request(1).find(receipt) != std::string::npos,
                "second provider acceptance request returns the tool result with call linkage");
}

} // namespace

#undef MINT_EXPECT
