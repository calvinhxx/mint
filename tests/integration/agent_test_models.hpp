#pragma once

#include "mint/ports/model_client.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

namespace mint::test {

class ScriptedModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        EXPECT_TRUE(tools.size() == 3) << ("read-only agent exposes exactly three tools");
        if (calls_++ == 0) {
            const mint::Json arguments = {{"path", "README.md"}};
            const mint::Json raw_call = {
                {"id", "test-read"},
                {"type", "function"},
                {"function", {{"name", "read_file"}, {"arguments", arguments.dump()}}}};
            return {.assistant_message = {{"role", "assistant"},
                                          {"content", nullptr},
                                          {"tool_calls", mint::Json::array({raw_call})}},
                    .text = {},
                    .tool_calls = {{"test-read", "read_file", arguments}}};
        }

        EXPECT_TRUE(messages.back().at("role") == "tool") << ("tool result is appended to context");
        const auto result = mint::Json::parse(messages.back().at("content").get<std::string>());
        EXPECT_TRUE(result.at("ok").get<bool>()) << ("tool result returned to model is successful");
        return {.assistant_message = {{"role", "assistant"}, {"content", "完成"}},
                .text = "完成",
                .tool_calls = {},
                .usage = {.available = true,
                          .prompt_tokens = 100,
                          .completion_tokens = 10,
                          .total_tokens = 110,
                          .cached_tokens = 80}};
    }

  private:
    int calls_ = 0;
};

inline mint::ModelReply model_tool_reply(std::string id, std::string name, mint::Json arguments) {
    mint::Json raw_call = {{"id", id},
                           {"type", "function"},
                           {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
    mint::ModelReply reply;
    reply.assistant_message = {
        {"role", "assistant"}, {"content", nullptr}, {"tool_calls", mint::Json::array({raw_call})}};
    reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
    return reply;
}

inline bool is_verification_ready(const mint::Json& message) {
    return message.value("role", "") == "user" && message.contains("content") &&
           message.at("content").is_string() &&
           message.at("content").get<std::string>().find("[Harness status]") != std::string::npos;
}

inline mint::Json latest_tool_result(const mint::Json& messages) {
    auto offset = std::size_t{0};
    if (is_verification_ready(messages.back())) {
        offset = 1;
    }
    const auto& message = messages.at(messages.size() - 1 - offset);
    EXPECT_TRUE(message.at("role") == "tool") << ("script receives the latest tool result");
    return mint::Json::parse(message.at("content").get<std::string>());
}

} // namespace mint::test
