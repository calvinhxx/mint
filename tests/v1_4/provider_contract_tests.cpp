#include "mint/infrastructure/chat_completions_client.hpp"
#include "mint/infrastructure/config.hpp"
#include "mint/infrastructure/model_provider_client.hpp"

#include "model_protocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#if !defined(_WIN32)
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

#define MINT_EXPECT(condition, message) EXPECT_TRUE(condition) << (message)

std::string mint_executable;

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

#if !defined(_WIN32)
class ScriptedHttpServer final {
  public:
    explicit ScriptedHttpServer(std::vector<std::string> response_bodies,
                                std::vector<int> response_statuses = {})
        : response_bodies_(std::move(response_bodies)),
          response_statuses_(std::move(response_statuses)) {
        if (response_bodies_.empty()) {
            throw std::invalid_argument("stream test needs at least one response");
        }
        if (response_statuses_.empty()) {
            response_statuses_.assign(response_bodies_.size(), 200);
        }
        if (response_statuses_.size() != response_bodies_.size()) {
            throw std::invalid_argument("stream test response statuses do not match bodies");
        }
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) {
            throw std::runtime_error("stream test could not create socket");
        }
        int reuse = 1;
        (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, static_cast<int>(response_bodies_.size())) != 0) {
            ::close(listener_);
            listener_ = -1;
            throw std::runtime_error("stream test could not bind loopback server");
        }
        socklen_t length = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(listener_);
            listener_ = -1;
            throw std::runtime_error("stream test could not inspect loopback port");
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~ScriptedHttpServer() {
        if (listener_ >= 0) {
            ::shutdown(listener_, SHUT_RDWR);
            ::close(listener_);
            listener_ = -1;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ScriptedHttpServer(const ScriptedHttpServer&) = delete;
    ScriptedHttpServer& operator=(const ScriptedHttpServer&) = delete;

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1/responses";
    }

    void wait() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] const std::string& request(std::size_t index = 0) const {
        return requests_.at(index);
    }

  private:
    static std::size_t content_length(const std::string& request) {
        constexpr std::string_view field = "Content-Length:";
        const auto position = request.find(field);
        if (position == std::string::npos) {
            return 0;
        }
        const auto begin = request.find_first_not_of(" \t", position + field.size());
        const auto end = request.find("\r\n", begin);
        return static_cast<std::size_t>(std::stoull(request.substr(begin, end - begin)));
    }

    static void send_all(int descriptor, std::string_view value) {
        std::size_t offset = 0;
        while (offset < value.size()) {
            const auto sent = ::send(descriptor, value.data() + offset, value.size() - offset, 0);
            if (sent <= 0) {
                return;
            }
            offset += static_cast<std::size_t>(sent);
        }
    }

    void serve() {
        for (std::size_t response_index = 0; response_index < response_bodies_.size();
             ++response_index) {
            const auto& response_body = response_bodies_.at(response_index);
            pollfd ready{listener_, POLLIN, 0};
            if (::poll(&ready, 1, 5000) <= 0) {
                return;
            }
            const auto connection = ::accept(listener_, nullptr, nullptr);
            if (connection < 0) {
                return;
            }
            std::string request;
            std::array<char, 4096> buffer{};
            while (request.find("\r\n\r\n") == std::string::npos) {
                const auto received = ::recv(connection, buffer.data(), buffer.size(), 0);
                if (received <= 0) {
                    break;
                }
                request.append(buffer.data(), static_cast<std::size_t>(received));
            }
            const auto header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const auto expected = header_end + 4 + content_length(request);
                while (request.size() < expected) {
                    const auto received = ::recv(connection, buffer.data(), buffer.size(), 0);
                    if (received <= 0) {
                        break;
                    }
                    request.append(buffer.data(), static_cast<std::size_t>(received));
                }
            }
            requests_.push_back(std::move(request));
            const auto status = response_statuses_.at(response_index);
            const auto reason = status == 200 ? "OK" : "Too Many Requests";
            const auto headers = std::string("HTTP/1.1 ") + std::to_string(status) + " " + reason +
                                 "\r\nContent-Type: text/event-stream\r\nContent-Length: " +
                                 std::to_string(response_body.size()) +
                                 "\r\nConnection: close\r\n\r\n";
            send_all(connection, headers);
            constexpr std::size_t network_fragment = 17;
            for (std::size_t offset = 0; offset < response_body.size();
                 offset += network_fragment) {
                send_all(connection,
                         std::string_view(response_body).substr(offset, network_fragment));
            }
            ::shutdown(connection, SHUT_RDWR);
            ::close(connection);
        }
        ::close(listener_);
        listener_ = -1;
    }

    int listener_ = -1;
    unsigned short port_ = 0;
    std::vector<std::string> response_bodies_;
    std::vector<int> response_statuses_;
    std::vector<std::string> requests_;
    std::thread thread_;
};

std::pair<int, std::string> run_process(const std::vector<std::string>& arguments) {
    if (arguments.empty()) {
        throw std::invalid_argument("process arguments cannot be empty");
    }
    int output_pipe[2]{};
    if (::pipe(output_pipe) != 0) {
        throw std::runtime_error("could not create CLI output pipe");
    }
    const auto child = ::fork();
    if (child < 0) {
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        throw std::runtime_error("could not fork CLI acceptance process");
    }
    if (child == 0) {
        (void)::dup2(output_pipe[1], STDOUT_FILENO);
        (void)::dup2(output_pipe[1], STDERR_FILENO);
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        (void)::setenv("NO_PROXY", "127.0.0.1", 1);
        (void)::setenv("no_proxy", "127.0.0.1", 1);
        std::vector<char*> raw_arguments;
        raw_arguments.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            raw_arguments.push_back(const_cast<char*>(argument.c_str()));
        }
        raw_arguments.push_back(nullptr);
        ::execv(raw_arguments.front(), raw_arguments.data());
        ::_exit(127);
    }

    ::close(output_pipe[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const auto received = ::read(output_pipe[0], buffer.data(), buffer.size());
        if (received <= 0) {
            break;
        }
        output.append(buffer.data(), static_cast<std::size_t>(received));
    }
    ::close(output_pipe[0]);
    int status = 0;
    (void)::waitpid(child, &status, 0);
    const auto exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return {exit_code, std::move(output)};
}
#endif

TEST(ProviderConfigContractTest, PreservesV13Compatibility) {
    const mint::ChatCompletionsConfig positional{"https://example.test/v1/chat/completions",
                                                 "secret",
                                                 "positional-model",
                                                 7,
                                                 42,
                                                 4,
                                                 123,
                                                 777,
                                                 {},
                                                 {},
                                                 mint::ModelAdapter::chat_completions,
                                                 false,
                                                 {}};
    MINT_EXPECT(positional.api_url == "https://example.test/v1/chat/completions" &&
                    positional.max_completion_tokens == 777 &&
                    positional.adapter == mint::ModelAdapter::chat_completions,
                "v1.3 positional aggregate field order remains compatible");

    TemporaryDirectory temporary;
    const auto legacy_path = temporary.path() / "legacy.json";
    write_text(legacy_path, R"({"api_url":"https://example.test/v1/chat/completions",)"
                            R"("api_key":"secret","model":"test-model"})");
    const auto legacy = mint::load_model_provider_config(legacy_path);
    MINT_EXPECT(legacy.adapter == mint::ModelAdapter::chat_completions && !legacy.stream,
                "v1.3 config defaults to non-streaming Chat Completions");
    const auto compatibility = mint::load_chat_completions_config(legacy_path);
    MINT_EXPECT(compatibility.adapter == legacy.adapter && compatibility.model == legacy.model,
                "legacy loader remains source and behavior compatible");

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
    stream += sse({{"id", "chat_1"},
                   {"model", "chat-test"},
                   {"choices",
                    mint::Json::array({{{"delta", {{"role", "assistant"}, {"content", "先"}}}}})}});
    const mint::Json first_tool_delta = {{"index", 0},
                                         {"id", "call_1"},
                                         {"type", "function"},
                                         {"function", {{"name", "read_"}, {"arguments", "{\"pa"}}}};
    stream +=
        sse({{"choices",
              mint::Json::array({{{"delta",
                                   {{"content", "检查"},
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

TEST(ProviderTransportContractTest, StreamsHttpResponses) {
#if defined(_WIN32)
    GTEST_SKIP() << "the local HTTP test server is not implemented on Windows";
#else
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
    ScriptedHttpServer server({std::move(body)});

    std::vector<mint::ModelProgress> progress;
    std::string streamed_text;
    mint::ModelProviderClient client(
        {.api_url = server.url(),
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
#endif
}

TEST(ProviderTransportContractTest, RetriesStreamingHttpFailures) {
#if defined(_WIN32)
    GTEST_SKIP() << "the local HTTP test server is not implemented on Windows";
#else
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
    ScriptedHttpServer server({rate_limit_stream, success_stream}, {429, 200});

    std::vector<mint::ModelProgress> progress;
    mint::ModelProviderClient client(
        {.api_url = server.url(),
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
#endif
}

TEST(ProviderCliContractTest, CompletesResponsesStreamingToolLoop) {
#if defined(_WIN32)
    GTEST_SKIP() << "the local HTTP test server is not implemented on Windows";
#else
    if (mint_executable.empty()) {
        GTEST_SKIP() << "mint executable was not supplied";
    }
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
    ScriptedHttpServer server({std::move(first_stream), std::move(second_stream)});

    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    std::filesystem::create_directories(workspace);
    write_text(workspace / "README.md", "# Local Agent\n");
    const auto config_path = temporary.path() / "config.json";
    write_text(config_path, mint::Json({{"adapter", "responses"},
                                        {"api_url", server.url()},
                                        {"api_key", ""},
                                        {"model", "responses-cli-test"},
                                        {"connect_timeout_seconds", 2},
                                        {"request_timeout_seconds", 2},
                                        {"max_retries", 0},
                                        {"max_completion_tokens", 128},
                                        {"stream", true}})
                                .dump(2));

    const auto [exit_code, output] =
        run_process({mint_executable, "--json", "--config", config_path.generic_string(), "--root",
                     workspace.generic_string(), "读取 README.md 后用一句话说明项目用途"});
    server.wait();
    MINT_EXPECT(exit_code == 0, "streaming Responses CLI exits successfully: " + output);

    mint::Json result;
    ASSERT_NO_THROW(result = mint::Json::parse(output))
        << "CLI stdout is not one JSON document: " << output;
    MINT_EXPECT(result.at("status") == "completed" && result.at("turns") == 2 &&
                    result.at("execution").at("tool_calls") == 1,
                "CLI completes a two-turn Responses tool loop");
    MINT_EXPECT(result.at("answer") == "README 证明项目是本地 Agent。",
                "CLI keeps the authoritative streamed final answer");
    MINT_EXPECT(result.at("model").at("adapter") == "responses" &&
                    result.at("model").at("streamed_calls") == 2 &&
                    result.at("model").at("stream_events") == 5 &&
                    result.at("model").at("streamed_bytes").get<std::size_t>() > 0,
                "CLI JSON reports Responses adapter and aggregate stream metrics");
    MINT_EXPECT(server.request(0).find(R"("tools":[{"description")") != std::string::npos &&
                    server.request(0).find(R"("name":"read_file")") != std::string::npos,
                "first CLI request advertises flattened Responses tools");
    MINT_EXPECT(server.request(1).find(R"("type":"function_call_output")") != std::string::npos &&
                    server.request(1).find(R"("call_id":"call_cli")") != std::string::npos &&
                    server.request(1).find("# Local Agent") != std::string::npos,
                "second CLI request returns the real tool result with call linkage");
#endif
}

} // namespace

#undef MINT_EXPECT

int main(int argc, char** argv) {
    constexpr std::string_view executable_option = "--mint-executable=";
    int write_index = 1;
    for (int read_index = 1; read_index < argc; ++read_index) {
        const std::string_view argument = argv[read_index];
        if (argument.starts_with(executable_option)) {
            mint_executable = argument.substr(executable_option.size());
        } else {
            argv[write_index++] = argv[read_index];
        }
    }
    argc = write_index;
    argv[argc] = nullptr;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
