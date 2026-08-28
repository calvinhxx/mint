#include "model_request.hpp"

#include "mint/runtime/task_control.hpp"

#include "diagnostic_log.hpp"
#include "model_http_transport.hpp"
#include "model_protocol.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

namespace mint::model_detail {
namespace {

constexpr std::size_t max_error_body_bytes = 800;
constexpr long retry_poll_interval_ms = 25;
constexpr long server_retry_safety_margin_ms = 50;

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

std::string response_error(const std::string& body, long status) {
    try {
        const auto parsed = Json::parse(body);
        if (parsed.contains("error")) {
            const auto& error = parsed.at("error");
            if (error.is_object() && error.contains("message") && error.at("message").is_string()) {
                return "模型接口返回 HTTP " + std::to_string(status) + ": " +
                       error.at("message").get<std::string>();
            }
            if (error.is_string()) {
                return "模型接口返回 HTTP " + std::to_string(status) + ": " +
                       error.get<std::string>();
            }
        }
    } catch (const Json::exception&) {
        // A short raw response often contains a useful proxy error.
    }

    const auto shortened = body.substr(0, max_error_body_bytes);
    return "模型接口返回 HTTP " + std::to_string(status) +
           (shortened.empty() ? std::string{} : ": " + shortened);
}

[[noreturn]] void throw_stopped_request(const ModelProviderConfig& config) {
    throw std::runtime_error(config.task_control->cancellation_requested()
                                 ? "模型请求已取消"
                                 : "模型请求超过任务总时间预算");
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

    auto reply = detail::parse_provider_response(profile.adapter, response);
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
    diagnostics::emit(diagnostics::Level::debug, "model.request.completed",
                      {{"attempt", attempt_number},
                       {"http_status", attempt.http_status},
                       {"duration_ms", reply.metadata.duration_ms},
                       {"streamed", reply.metadata.streamed}});
    return reply;
}

} // namespace

ModelReply complete_provider_request(const ModelProviderConfig& config, const Json& messages,
                                     const Json& tools) {
    const ProgressReporter progress(config);
    const auto profile = resolve_model_provider_profile(config);
    const auto request_body = detail::build_provider_request(config, messages, tools).dump();
    diagnostics::emit(diagnostics::Level::debug, "model.request.started",
                      {{"provider", model_provider_name(profile.provider)},
                       {"adapter", model_adapter_name(profile.adapter)},
                       {"model", config.model},
                       {"stream", config.stream},
                       {"message_count", messages.size()},
                       {"tool_count", tools.size()},
                       {"request_bytes", request_body.size()}});

    long delay_ms = config.retry_initial_delay_ms;
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
            throw;
        }
        if (attempt.stream_error != nullptr) {
            progress.emit({.kind = ModelProgressKind::request_failed,
                           .attempt = attempt_number,
                           .http_status = attempt.http_status});
            std::rethrow_exception(attempt.stream_error);
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
                throw std::runtime_error("模型返回的内容不是有效 JSON: " +
                                         std::string(error.what()));
            } catch (...) {
                progress.emit({.kind = ModelProgressKind::request_failed,
                               .attempt = attempt_number,
                               .http_status = attempt.http_status});
                throw;
            }
        }

        if (attempt.outcome == HttpTransportOutcome::cancelled && config.task_control != nullptr &&
            config.task_control->should_stop()) {
            progress.emit({.kind = ModelProgressKind::request_failed,
                           .attempt = attempt_number,
                           .http_status = attempt.http_status});
            throw_stopped_request(config);
        }

        const bool retryable = attempt.outcome == HttpTransportOutcome::retryable_failure ||
                               (attempt.outcome == HttpTransportOutcome::success &&
                                is_transient_http_error(attempt.http_status));
        if (!retryable || attempt_index >= config.max_retries) {
            progress.emit({.kind = ModelProgressKind::request_failed,
                           .attempt = attempt_number,
                           .http_status = attempt.http_status});
            if (attempt.outcome != HttpTransportOutcome::success) {
                throw std::runtime_error("请求模型失败: " + attempt.transport_error);
            }
            throw std::runtime_error(response_error(attempt.body, attempt.http_status));
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
