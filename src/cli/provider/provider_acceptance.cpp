#include "provider_acceptance.hpp"

#include "mint/localization/localization.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace mint::cli::provider_detail {
namespace {

using localization::Message;
using localization::message;
using localization::Placeholder;

constexpr auto tool_name = "mint_acceptance_echo";
constexpr auto challenge = "mint-provider-acceptance-v1";
constexpr auto receipt = "MINT_PROVIDER_ACCEPTANCE_OK";

Json tool_definition() {
    return Json::array({{{"type", "function"},
                         {"function",
                          {{"name", tool_name},
                           {"description", "Return the provider acceptance challenge unchanged."},
                           {"parameters",
                            {{"type", "object"},
                             {"properties", {{"challenge", {{"type", "string"}}}}},
                             {"required", Json::array({"challenge"})},
                             {"additionalProperties", false}}}}}}});
}

Json initial_messages() {
    return Json::array(
        {{{"role", "system"},
          {"content",
           "This is a provider compatibility test. Follow the requested tool protocol exactly."}},
         {{"role", "user"},
          {"content",
           "Call mint_acceptance_echo exactly once with challenge "
           "\"mint-provider-acceptance-v1\". After receiving the tool result, reply with the "
           "receipt from that result and do not call another tool."}}});
}

struct AcceptanceMetrics {
    std::size_t requests = 0;
    std::size_t attempts = 0;
    std::size_t retries = 0;
    std::size_t streamed_requests = 0;
    std::size_t stream_events = 0;
    std::size_t streamed_bytes = 0;
    std::size_t usage_requests = 0;
    std::size_t prompt_tokens = 0;
    std::size_t completion_tokens = 0;
    std::size_t total_tokens = 0;
    std::size_t cached_tokens = 0;
    long long duration_ms = 0;
    std::string reported_provider;
    std::string reported_adapter;
    std::string reported_model;

    void record(const ModelReply& reply, bool expect_streaming) {
        if (reply.metadata.streamed != expect_streaming) {
            throw std::runtime_error(message(Message::cli_provider_test_streaming_mismatch));
        }
        ++requests;
        attempts += reply.metadata.attempts;
        retries += reply.metadata.retries;
        streamed_requests += reply.metadata.streamed ? 1 : 0;
        stream_events += reply.metadata.stream_events;
        streamed_bytes += reply.metadata.streamed_bytes;
        duration_ms += reply.metadata.duration_ms;
        if (!reply.metadata.provider.empty()) {
            reported_provider = reply.metadata.provider;
        }
        if (!reply.metadata.adapter.empty()) {
            reported_adapter = reply.metadata.adapter;
        }
        if (!reply.metadata.model.empty()) {
            reported_model = reply.metadata.model;
        }
        if (reply.usage.available) {
            ++usage_requests;
            prompt_tokens += reply.usage.prompt_tokens;
            completion_tokens += reply.usage.completion_tokens;
            total_tokens += reply.usage.total_tokens;
            cached_tokens += reply.usage.cached_tokens;
        }
    }

    [[nodiscard]] Json report() const {
        return {
            {"requests", requests},
            {"attempts", attempts},
            {"retries", retries},
            {"duration_ms", duration_ms},
            {"streamed_requests", streamed_requests},
            {"stream_events", stream_events},
            {"streamed_bytes", streamed_bytes},
            {"reported_provider",
             reported_provider.empty() ? Json(nullptr) : Json(reported_provider)},
            {"reported_adapter", reported_adapter.empty() ? Json(nullptr) : Json(reported_adapter)},
            {"reported_model", reported_model.empty() ? Json(nullptr) : Json(reported_model)},
            {"usage",
             {{"reported_requests", usage_requests},
              {"prompt_tokens", prompt_tokens},
              {"completion_tokens", completion_tokens},
              {"total_tokens", total_tokens},
              {"cached_tokens", cached_tokens},
              {"cache_hit_rate", model_usage::cache_hit_rate_json(cached_tokens, prompt_tokens)}}},
            {"checks",
             {{"function_call", true},
              {"arguments_round_trip", true},
              {"tool_result_continuation", true}}}};
    }
};

void validate_tool_call(const ModelReply& reply) {
    if (!reply.assistant_message.is_object() || reply.tool_calls.size() != 1) {
        throw std::runtime_error(message(Message::cli_provider_test_tool_call_missing));
    }
    const auto& call = reply.tool_calls.front();
    if (call.id.empty() || call.name != tool_name || !call.arguments.is_object() ||
        call.arguments.value("challenge", "") != challenge) {
        throw std::runtime_error(message(Message::cli_provider_test_tool_call_invalid));
    }
}

void validate_final_reply(const ModelReply& reply) {
    if (!reply.assistant_message.is_object() || !reply.tool_calls.empty()) {
        throw std::runtime_error(message(Message::cli_provider_test_final_reply_invalid));
    }
    if (reply.text.find(receipt) == std::string::npos) {
        throw std::runtime_error(message(Message::cli_provider_test_receipt_missing));
    }
}

} // namespace

Json run_provider_acceptance(ModelClient& client, bool expect_streaming) {
    auto messages = initial_messages();
    const auto tools = tool_definition();
    AcceptanceMetrics metrics;

    auto first = client.complete(messages, tools);
    metrics.record(first, expect_streaming);
    validate_tool_call(first);

    const auto call_id = first.tool_calls.front().id;
    messages.push_back(std::move(first.assistant_message));
    messages.push_back(
        {{"role", "tool"},
         {"tool_call_id", call_id},
         {"content", Json({{"ok", true}, {"challenge", challenge}, {"receipt", receipt}}).dump()}});

    const auto second = client.complete(messages, tools);
    metrics.record(second, expect_streaming);
    validate_final_reply(second);
    return metrics.report();
}

} // namespace mint::cli::provider_detail
