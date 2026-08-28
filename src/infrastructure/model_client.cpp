#include "mint/infrastructure/model_provider_client.hpp"

#include "model_http_transport.hpp"
#include "model_provider_profile.hpp"
#include "model_request.hpp"

#include <stdexcept>
#include <utility>

namespace mint {

std::string_view model_progress_kind_name(ModelProgressKind kind) noexcept {
    switch (kind) {
    case ModelProgressKind::attempt_started:
        return "attempt_started";
    case ModelProgressKind::stream_started:
        return "stream_started";
    case ModelProgressKind::stream_completed:
        return "stream_completed";
    case ModelProgressKind::retry_scheduled:
        return "retry_scheduled";
    case ModelProgressKind::request_succeeded:
        return "request_succeeded";
    case ModelProgressKind::request_failed:
        return "request_failed";
    }
    return "unknown";
}

Json model_progress_to_json(const ModelProgress& progress) {
    return {{"kind", model_progress_kind_name(progress.kind)},
            {"attempt", progress.attempt},
            {"max_attempts", progress.max_attempts},
            {"http_status", progress.http_status},
            {"delay_ms", progress.delay_ms},
            {"elapsed_ms", progress.elapsed_ms},
            {"stream_events", progress.stream_events},
            {"streamed_bytes", progress.streamed_bytes}};
}

ModelProviderClient::ModelProviderClient(ModelProviderConfig config) : config_(std::move(config)) {
    if (config_.api_url.empty()) {
        throw std::invalid_argument("模型接口地址不能为空");
    }
    if (config_.model.empty()) {
        throw std::invalid_argument("模型名称不能为空");
    }
    if (config_.connect_timeout_seconds <= 0 || config_.request_timeout_seconds <= 0 ||
        config_.max_retries < 0 || config_.max_retries > model_provider_bounds::max_retries ||
        config_.retry_initial_delay_ms <= 0 ||
        config_.retry_initial_delay_ms > model_provider_bounds::max_retry_initial_delay_ms ||
        config_.max_completion_tokens <= 0 ||
        config_.max_completion_tokens > model_provider_bounds::max_completion_tokens) {
        throw std::invalid_argument("模型超时或重试配置超出允许范围");
    }
    (void)resolve_model_provider_profile(config_);
    model_detail::resolve_model_provider_credentials(config_);
    model_detail::ensure_http_runtime();
}

ModelReply ModelProviderClient::complete(const Json& messages, const Json& tools) {
    return model_detail::complete_provider_request(config_, messages, tools);
}

} // namespace mint
