#include "model_request.hpp"

#include "mint/runtime/task_control.hpp"

#include "mint/infrastructure/diagnostic_log.hpp"
#include "mint/localization/localization.hpp"
#include "model_http_transport.hpp"
#include "model_protocol.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace mint::model_detail {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

constexpr std::size_t max_error_body_bytes = 800;
constexpr long retry_poll_interval_ms = 25;
constexpr long server_retry_safety_margin_ms = 50;
constexpr std::string_view failed_tool_generation_retry_prompt =
    "The previous tool call did not match the function schema. Return exactly one valid "
    "function call using a supplied tool. Encode arguments as a JSON object with double-quoted "
    "keys and strings; do not emit prose or a manual tool-call wrapper.";
constexpr std::string_view repeated_failed_generation_retry_prompt =
    "The previous response still could not be parsed. If no tool is needed, return a concise "
    "final answer. Otherwise return exactly one valid function call using a supplied tool with "
    "JSON object arguments; do not emit a manual tool-call wrapper.";

class ProgressReporter {
  public:
    explicit ProgressReporter(const ModelProviderConfig& config)
        : config_(config), started_at_(std::chrono::steady_clock::now()) {}

    [[nodiscard]] long long elapsed_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - started_at_)
            .count();
    }

    void emit(ModelProgress progress) const {
        if (!config_.progress) {
            return;
        }
        progress.max_attempts = static_cast<std::size_t>(config_.max_retries + 1);
        progress.elapsed_ms = elapsed_ms();
        config_.progress(progress);
    }

  private:
    const ModelProviderConfig& config_;
    std::chrono::steady_clock::time_point started_at_;
};

bool is_transient_http_error(long status) noexcept {
    return status == 408 || status == 429 || (status >= 500 && status <= 599);
}

bool has_failed_generation(const Json& error) {
    if (!error.is_object()) {
        return false;
    }
    const auto failed_generation = error.find("failed_generation");
    if (failed_generation == error.end()) {
        return false;
    }
    const auto populated = (failed_generation->is_string() &&
                            !failed_generation->get_ref<const std::string&>().empty()) ||
                           (failed_generation->is_object() && !failed_generation->empty());
    if (!populated) {
        return false;
    }
    const auto code = error.find("code");
    return code == error.end() || code->is_null() ||
           (code->is_string() && code->get_ref<const std::string&>() == "tool_use_failed");
}

bool has_failed_generation(const std::string& body) {
    try {
        const auto parsed = Json::parse(body);
        if (!parsed.is_object()) {
            return false;
        }
        const auto error = parsed.find("error");
        if (error == parsed.end() || !error->is_object()) {
            return false;
        }
        return has_failed_generation(*error);
    } catch (const Json::exception&) {
        return false;
    }
}

bool is_failed_tool_generation(const HttpAttempt& attempt, ModelProvider provider,
                               std::size_t tool_count) {
    return attempt.http_status == 400 && provider == ModelProvider::groq && tool_count > 0 &&
           has_failed_generation(attempt.body);
}

bool is_retryable_http_error(const HttpAttempt& attempt, ModelProvider provider,
                             std::size_t tool_count) {
    return is_transient_http_error(attempt.http_status) ||
           is_failed_tool_generation(attempt, provider, tool_count);
}

void correct_failed_tool_generation_request(std::string& request_body, ModelAdapter adapter) {
    if (adapter != ModelAdapter::chat_completions) {
        return;
    }
    auto request = Json::parse(request_body);
    if (!request.is_object() || !request.contains("messages") ||
        !request.at("messages").is_array()) {
        throw std::logic_error(message(Message::model_request_tool_retry_messages_missing));
    }
    request["temperature"] = 0.0;
    request["tool_choice"] = "required";
    request["messages"].push_back(
        {{"role", "user"}, {"content", failed_tool_generation_retry_prompt}});
    request_body = request.dump();
}

void relax_failed_tool_generation_request(std::string& request_body, ModelAdapter adapter) {
    if (adapter != ModelAdapter::chat_completions) {
        return;
    }
    auto request = Json::parse(request_body);
    if (!request.is_object() || !request.contains("messages") ||
        !request.at("messages").is_array()) {
        throw std::logic_error(message(Message::model_request_tool_fallback_messages_missing));
    }
    request["temperature"] = 0.0;
    request["tool_choice"] = "auto";
    auto& messages = request["messages"];
    if (!messages.empty()) {
        const auto& last = messages.back();
        if (last.is_object() && last.value("role", "") == "user" && last.contains("content") &&
            last.at("content").is_string() &&
            last.at("content").get_ref<const std::string&>() ==
                failed_tool_generation_retry_prompt) {
            messages.erase(messages.end() - 1);
        }
    }
    request["messages"].push_back(
        {{"role", "user"}, {"content", repeated_failed_generation_retry_prompt}});
    request_body = request.dump();
}

std::string_view failed_request_outcome_name(HttpTransportOutcome outcome) noexcept {
    switch (outcome) {
    case HttpTransportOutcome::success:
        return "http_error";
    case HttpTransportOutcome::cancelled:
        return "cancelled";
    case HttpTransportOutcome::retryable_failure:
        return "retryable_transport_error";
    case HttpTransportOutcome::failure:
        return "transport_error";
    }
    return "unknown";
}

std::string response_error(const std::string& body, long status) {
    try {
        const auto parsed = Json::parse(body);
        if (parsed.contains("error")) {
            const auto& error = parsed.at("error");
            if (error.is_object() && error.contains("failed_generation")) {
                return message(Message::model_request_invalid_tool_call,
                               {arg(Placeholder::status, status)});
            }
            if (error.is_object() && error.contains("message") && error.at("message").is_string()) {
                return message(Message::model_request_http_error_detail,
                               {arg(Placeholder::status, status),
                                arg(Placeholder::detail, error.at("message").get<std::string>())});
            }
            if (error.is_string()) {
                return message(Message::model_request_http_error_detail,
                               {arg(Placeholder::status, status),
                                arg(Placeholder::detail, error.get<std::string>())});
            }
        }
    } catch (const Json::exception&) {
        // EN: A short raw response often contains a useful proxy error.
        // ZH-CN: 较短的原始响应通常包含有用的代理错误信息。
    }

    const auto shortened = body.substr(0, max_error_body_bytes);
    return shortened.empty()
               ? message(Message::model_request_http_error, {arg(Placeholder::status, status)})
               : message(Message::model_request_http_error_detail,
                         {arg(Placeholder::status, status), arg(Placeholder::detail, shortened)});
}

[[noreturn]] void throw_stopped_request(const ModelProviderConfig& config) {
    throw std::runtime_error(message(config.task_control->cancellation_requested()
                                         ? Message::model_request_cancelled
                                         : Message::model_request_time_budget_exceeded));
}

void wait_before_retry(const ModelProviderConfig& config, long delay_ms) {
    auto remaining = delay_ms;
    while (remaining > 0) {
        if (config.task_control != nullptr && config.task_control->should_stop()) {
            throw_stopped_request(config);
        }
        const auto slice = std::min(remaining, retry_poll_interval_ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
}

long retry_delay(const HttpAttempt& attempt, long client_delay_ms) {
    if (attempt.http_status != 429) {
        return client_delay_ms;
    }
    const auto server_delay_ms = std::max(attempt.retry_after_ms, attempt.token_reset_ms);
    return server_delay_ms < 0
               ? client_delay_ms
               : std::max(client_delay_ms, server_delay_ms + server_retry_safety_margin_ms);
}

ModelReply parse_successful_attempt(const ModelProviderConfig& config, HttpAttempt& attempt,
                                    const ProgressReporter& progress, std::size_t attempt_number) {
    const auto profile = resolve_model_provider_profile(config);
    Json response;
    std::size_t stream_events = 0;
    std::size_t streamed_bytes = 0;
    if (config.stream) {
        response = attempt.stream->finish();
        stream_events = attempt.stream->event_count();
        streamed_bytes = attempt.stream->streamed_bytes();
    } else {
        response = Json::parse(attempt.body);
    }

    auto reply = detail::parse_provider_response(profile.adapter, response, config.response_limits);
    reply.metadata.adapter = model_adapter_name(profile.adapter);
    reply.metadata.provider = model_provider_name(profile.provider);
    if (reply.metadata.model.empty()) {
        reply.metadata.model = config.model;
    }
    reply.metadata.attempts = attempt_number;
    reply.metadata.retries = attempt_number - 1;
    reply.metadata.http_status = attempt.http_status;
    reply.metadata.duration_ms = progress.elapsed_ms();
    reply.metadata.streamed = config.stream;
    reply.metadata.stream_events = stream_events;
    reply.metadata.streamed_bytes = streamed_bytes;
    reply.metadata.request_token_limit = attempt.token_limit;

    if (config.stream) {
        progress.emit({.kind = ModelProgressKind::stream_completed,
                       .attempt = attempt_number,
                       .http_status = attempt.http_status,
                       .stream_events = stream_events,
                       .streamed_bytes = streamed_bytes});
    }
    progress.emit({.kind = ModelProgressKind::request_succeeded,
                   .attempt = attempt_number,
                   .http_status = attempt.http_status});
    diagnostics::emit(diagnostics::Level::info, "model.request.completed",
                      {{"attempt", attempt_number},
                       {"http_status", attempt.http_status},
                       {"duration_ms", reply.metadata.duration_ms},
                       {"streamed", reply.metadata.streamed},
                       {"request_token_limit", reply.metadata.request_token_limit},
                       {"usage_available", reply.usage.available},
                       {"input_tokens", reply.usage.prompt_tokens},
                       {"cached_tokens", reply.usage.cached_tokens},
                       {"cache_hit_rate", model_usage::cache_hit_rate_json(reply.usage)},
                       {"output_tokens", reply.usage.completion_tokens},
                       {"total_tokens", reply.usage.total_tokens}});
    return reply;
}

} // namespace

ModelReply complete_provider_request(const ModelProviderConfig& config, std::string request_body,
                                     std::size_t message_count, std::size_t tool_count) {
    const ProgressReporter progress(config);
    const auto profile = resolve_model_provider_profile(config);
    diagnostics::emit(diagnostics::Level::debug, "model.request.started",
                      {{"provider", model_provider_name(profile.provider)},
                       {"adapter", model_adapter_name(profile.adapter)},
                       {"model", config.model},
                       {"stream", config.stream},
                       {"message_count", message_count},
                       {"tool_count", tool_count},
                       {"request_bytes", request_body.size()}});

    long delay_ms = config.retry_initial_delay_ms;
    bool corrected_failed_generation = false;
    bool relaxed_failed_generation = false;
    for (long attempt_index = 0;; ++attempt_index) {
        const auto attempt_number = static_cast<std::size_t>(attempt_index + 1);
        diagnostics::emit(diagnostics::Level::debug, "model.request.attempt",
                          {{"attempt", attempt_number}, {"max_attempts", config.max_retries + 1}});
        progress.emit({.kind = ModelProgressKind::attempt_started, .attempt = attempt_number});
        if (config.stream) {
            progress.emit({.kind = ModelProgressKind::stream_started, .attempt = attempt_number});
        }

        HttpAttempt attempt;
        try {
            attempt = perform_http_attempt(config, request_body);
        } catch (...) {
            progress.emit({.kind = ModelProgressKind::request_failed, .attempt = attempt_number});
            diagnostics::emit(diagnostics::Level::warning, "model.request.failed",
                              {{"attempt", attempt_number}, {"outcome", "transport_exception"}});
            throw;
        }
        if (attempt.response_error != nullptr) {
            progress.emit({.kind = ModelProgressKind::request_failed,
                           .attempt = attempt_number,
                           .http_status = attempt.http_status});
            diagnostics::emit(diagnostics::Level::warning, "model.request.failed",
                              {{"attempt", attempt_number},
                               {"curl_code", attempt.transport_code},
                               {"http_status", attempt.http_status},
                               {"outcome", "response_callback_error"}});
            std::rethrow_exception(attempt.response_error);
        }

        const bool successful_http = attempt.outcome == HttpTransportOutcome::success &&
                                     attempt.http_status >= 200 && attempt.http_status < 300;
        if (successful_http) {
            try {
                return parse_successful_attempt(config, attempt, progress, attempt_number);
            } catch (const Json::exception& error) {
                progress.emit({.kind = ModelProgressKind::request_failed,
                               .attempt = attempt_number,
                               .http_status = attempt.http_status});
                diagnostics::emit(diagnostics::Level::warning, "model.request.failed",
                                  {{"attempt", attempt_number},
                                   {"http_status", attempt.http_status},
                                   {"outcome", "invalid_response"}});
                throw std::runtime_error(message(Message::model_request_invalid_json,
                                                 {arg(Placeholder::error, error.what())}));
            } catch (...) {
                progress.emit({.kind = ModelProgressKind::request_failed,
                               .attempt = attempt_number,
                               .http_status = attempt.http_status});
                diagnostics::emit(diagnostics::Level::warning, "model.request.failed",
                                  {{"attempt", attempt_number},
                                   {"http_status", attempt.http_status},
                                   {"outcome", "response_error"}});
                throw;
            }
        }

        if (attempt.outcome == HttpTransportOutcome::cancelled && config.task_control != nullptr &&
            config.task_control->should_stop()) {
            progress.emit({.kind = ModelProgressKind::request_failed,
                           .attempt = attempt_number,
                           .http_status = attempt.http_status});
            diagnostics::emit(diagnostics::Level::warning, "model.request.failed",
                              {{"attempt", attempt_number},
                               {"curl_code", attempt.transport_code},
                               {"http_status", attempt.http_status},
                               {"outcome", "cancelled"}});
            throw_stopped_request(config);
        }

        const bool failed_tool_generation =
            attempt.outcome == HttpTransportOutcome::success &&
            is_failed_tool_generation(attempt, profile.provider, tool_count);
        const bool retryable = attempt.outcome == HttpTransportOutcome::retryable_failure ||
                               (attempt.outcome == HttpTransportOutcome::success &&
                                is_retryable_http_error(attempt, profile.provider, tool_count));
        if (!retryable || attempt_index >= config.max_retries) {
            progress.emit({.kind = ModelProgressKind::request_failed,
                           .attempt = attempt_number,
                           .http_status = attempt.http_status});
            diagnostics::emit(diagnostics::Level::warning, "model.request.failed",
                              {{"attempt", attempt_number},
                               {"curl_code", attempt.transport_code},
                               {"http_status", attempt.http_status},
                               {"outcome", failed_request_outcome_name(attempt.outcome)}});
            if (attempt.outcome != HttpTransportOutcome::success) {
                throw std::runtime_error(
                    message(Message::model_request_transport_failed,
                            {arg(Placeholder::error, attempt.transport_error)}));
            }
            throw std::runtime_error(response_error(attempt.body, attempt.http_status));
        }

        if (failed_tool_generation) {
            if (corrected_failed_generation && !relaxed_failed_generation) {
                relax_failed_tool_generation_request(request_body, profile.adapter);
                relaxed_failed_generation = true;
            } else if (!corrected_failed_generation) {
                correct_failed_tool_generation_request(request_body, profile.adapter);
                corrected_failed_generation = true;
            }
        }

        const auto effective_delay_ms = retry_delay(attempt, delay_ms);
        progress.emit({.kind = ModelProgressKind::retry_scheduled,
                       .attempt = attempt_number,
                       .http_status = attempt.http_status,
                       .delay_ms = effective_delay_ms});
        diagnostics::emit(diagnostics::Level::warning, "model.request.retry_scheduled",
                          {{"attempt", attempt_number},
                           {"curl_code", attempt.transport_code},
                           {"http_status", attempt.http_status},
                           {"delay_ms", effective_delay_ms}});
        try {
            wait_before_retry(config, effective_delay_ms);
        } catch (...) {
            progress.emit({.kind = ModelProgressKind::request_failed,
                           .attempt = attempt_number,
                           .http_status = attempt.http_status});
            throw;
        }
        delay_ms = std::min<long>(delay_ms * 2, model_provider_bounds::max_retry_initial_delay_ms);
    }
}

} // namespace mint::model_detail
