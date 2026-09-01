#include "mint/infrastructure/config.hpp"
#include "mint/infrastructure/model_provider_client.hpp"

#include "scripted_http_server.hpp"
#include "test_workspace.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using mint::test::ScriptedHttpServer;
using mint::test::TemporaryDirectory;
using mint::test::write_text;

TEST(ModelConfigTest, LoadsAndValidatesJson) {
    TemporaryDirectory temporary;
    const auto valid_path = temporary.path() / "valid.json";
    write_text(valid_path, R"({"api_url":"https://example.test/chat/completions",)"
                           R"("api_key":"secret","model":"example-model",)"
                           R"("connect_timeout_seconds":7,"request_timeout_seconds":42,)"
                           R"("max_retries":4,"retry_initial_delay_ms":123,)"
                           R"("max_completion_tokens":777})");

    const auto config = mint::load_model_provider_config(valid_path);
    EXPECT_EQ(config.api_url, "https://example.test/chat/completions");
    EXPECT_EQ(config.api_key, "secret");
    EXPECT_EQ(config.model, "example-model");
    EXPECT_EQ(config.connect_timeout_seconds, 7);
    EXPECT_EQ(config.request_timeout_seconds, 42);
    EXPECT_EQ(config.max_retries, 4);
    EXPECT_EQ(config.retry_initial_delay_ms, 123);
    EXPECT_EQ(config.max_completion_tokens, 777U);
    EXPECT_EQ(config.adapter, mint::ModelAdapter::chat_completions);
    EXPECT_FALSE(config.stream);

    const auto invalid_path = temporary.path() / "invalid.json";
    write_text(invalid_path, R"({"api_url":"https://example.test","api_key":"x"})");
    try {
        (void)mint::load_model_provider_config(invalid_path);
        FAIL() << "missing model must be rejected";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("model"), std::string::npos);
    }

    EXPECT_THROW(
        (void)mint::ModelProviderClient({.api_url = "https://example.test/chat/completions",
                                         .model = "example-model",
                                         .connect_timeout_seconds = 0,
                                         .request_timeout_seconds = 1}),
        std::invalid_argument);
}

TEST(ModelClientTest, RetriesWithServerDirectedBackoff) {
    const std::string transient_error = R"({"error":{"message":"transient test failure"}})";
    const std::string success =
        R"({"choices":[{"message":{"role":"assistant","content":"retry passed"}}],)"
        R"("usage":{"prompt_tokens":120,"completion_tokens":8,"total_tokens":128,)"
        R"("prompt_tokens_details":{"cached_tokens":96}}})";
    ScriptedHttpServer server(
        {{.status = 429,
          .headers = {{"Retry-After", "0.02"}, {"X-RateLimit-Reset-Tokens", "20ms"}},
          .body = transient_error},
         {.status = 503, .body = transient_error},
         {.body = success}});
    std::vector<mint::ModelProgress> progress;
    mint::ModelProviderClient client(
        {.api_url = server.url("/v1/chat/completions"),
         .model = "retry-test-model",
         .connect_timeout_seconds = 2,
         .request_timeout_seconds = 2,
         .max_retries = 2,
         .retry_initial_delay_ms = 1,
         .max_completion_tokens = 321,
         .progress = [&](const mint::ModelProgress& event) { progress.push_back(event); }});
    const auto started = std::chrono::steady_clock::now();
    const auto reply =
        client.complete(mint::Json::array({{{"role", "system"}, {"content", "test"}},
                                           {{"role", "user"}, {"content", "test retry"}}}),
                        mint::Json::array());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    server.wait();

    EXPECT_EQ(reply.text, "retry passed");
    ASSERT_EQ(server.request_count(), 3U);
    EXPECT_GE(elapsed.count(), 50);
    for (std::size_t index = 0; index < server.request_count(); ++index) {
        EXPECT_NE(server.request(index).find(R"("max_completion_tokens":321)"), std::string::npos);
    }
    EXPECT_TRUE(reply.usage.available);
    EXPECT_EQ(reply.usage.prompt_tokens, 120U);
    EXPECT_EQ(reply.usage.completion_tokens, 8U);
    EXPECT_EQ(reply.usage.cached_tokens, 96U);
    EXPECT_EQ(reply.metadata.adapter, "chat_completions");
    EXPECT_EQ(reply.metadata.model, "retry-test-model");
    EXPECT_EQ(reply.metadata.attempts, 3U);
    EXPECT_EQ(reply.metadata.retries, 2U);
    EXPECT_EQ(reply.metadata.http_status, 200L);
    EXPECT_GE(reply.metadata.duration_ms, 50U);

    ASSERT_EQ(progress.size(), 6U);
    EXPECT_EQ(progress.at(0).kind, mint::ModelProgressKind::attempt_started);
    EXPECT_EQ(progress.at(1).kind, mint::ModelProgressKind::retry_scheduled);
    EXPECT_EQ(progress.at(1).http_status, 429L);
    EXPECT_GE(progress.at(1).delay_ms, 50U);
    EXPECT_EQ(progress.at(2).kind, mint::ModelProgressKind::attempt_started);
    EXPECT_EQ(progress.at(3).kind, mint::ModelProgressKind::retry_scheduled);
    EXPECT_EQ(progress.at(3).http_status, 503L);
    EXPECT_EQ(progress.at(4).kind, mint::ModelProgressKind::attempt_started);
    EXPECT_EQ(progress.at(5).kind, mint::ModelProgressKind::request_succeeded);
    EXPECT_EQ(progress.at(5).http_status, 200L);
    EXPECT_EQ(progress.at(5).attempt, 3U);
    EXPECT_EQ(progress.at(5).max_attempts, 3U);

    const auto progress_json = mint::model_progress_to_json(progress.at(1));
    EXPECT_EQ(progress_json.at("kind"), "retry_scheduled");
    EXPECT_EQ(progress_json.at("attempt"), 1);
    EXPECT_EQ(progress_json.at("http_status"), 429);
}

} // namespace
