#include "mint/infrastructure/model_provider_client.hpp"

#include "model_http_transport.hpp"
#include "model_protocol.hpp"
#include "model_provider_profile.hpp"
#include "model_request.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mint {
namespace {

std::size_t estimated_tokens(std::size_t serialized_bytes, std::size_t bytes_per_token) {
    return model_token_estimation::from_serialized_bytes(serialized_bytes, bytes_per_token);
}

std::size_t estimated_tokens(const Json& value, std::size_t bytes_per_token) {
    return estimated_tokens(value.dump().size(), bytes_per_token);
}

} // namespace

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
    model_detail::normalize_model_provider_endpoint(config_);
    if (config_.model.empty()) {
        throw std::invalid_argument("模型名称不能为空");
    }
    if (config_.connect_timeout_seconds <= 0 || config_.request_timeout_seconds <= 0 ||
        config_.max_retries < 0 || config_.max_retries > model_provider_bounds::max_retries ||
        config_.retry_initial_delay_ms <= 0 ||
        config_.retry_initial_delay_ms > model_provider_bounds::max_retry_initial_delay_ms ||
        config_.max_completion_tokens <= 0 ||
        config_.max_completion_tokens > model_provider_bounds::max_completion_tokens ||
        config_.max_request_tokens < 0 || config_.request_token_safety_margin < 0 ||
        config_.request_token_estimate_bytes_per_token <
            model_provider_bounds::min_request_token_estimate_bytes_per_token ||
        config_.request_token_estimate_bytes_per_token >
            model_provider_bounds::max_request_token_estimate_bytes_per_token) {
        throw std::invalid_argument("模型超时或重试配置超出允许范围");
    }
    if (config_.max_request_tokens != 0 &&
        (config_.request_token_safety_margin >= config_.max_request_tokens ||
         config_.max_completion_tokens >=
             config_.max_request_tokens - config_.request_token_safety_margin)) {
        throw std::invalid_argument("max_request_tokens 必须大于输出 Token 与请求安全余量之和");
    }
    if (!valid_model_response_limits(config_.response_limits)) {
        throw std::invalid_argument("模型响应资源上限超出允许范围");
    }
    (void)resolve_model_provider_profile(config_);
    model_detail::resolve_model_provider_credentials(config_);
    model_detail::ensure_http_runtime();
}

ModelRequestLimits ModelProviderClient::request_limits(const Json& tools) const {
    const auto automatic = config_.max_request_tokens == 0;
    auto maximum = automatic ? model_provider_defaults::automatic_max_request_tokens
                             : static_cast<std::size_t>(config_.max_request_tokens);
    auto source = automatic ? ModelRequestLimitSource::automatic : ModelRequestLimitSource::config;
    if (learned_max_request_tokens_ != 0) {
        if (automatic || learned_max_request_tokens_ <= maximum) {
            maximum = learned_max_request_tokens_;
            source = ModelRequestLimitSource::response_header;
        }
    }

    const auto bytes_per_token =
        static_cast<std::size_t>(config_.request_token_estimate_bytes_per_token);
    const auto empty_messages = Json::array();
    const auto request_overhead = estimated_tokens(
        detail::build_provider_request(config_, empty_messages, tools), bytes_per_token);
    return {.max_request_tokens = maximum,
            .reserved_output_tokens = static_cast<std::size_t>(config_.max_completion_tokens),
            .safety_margin_tokens = static_cast<std::size_t>(config_.request_token_safety_margin),
            .request_overhead_tokens = request_overhead,
            .max_request_tokens_source = source,
            .response_header_max_request_tokens = learned_max_request_tokens_,
            .request_token_estimate_bytes_per_token = bytes_per_token};
}

ModelReply ModelProviderClient::complete(const Json& messages, const Json& tools) {
    const auto limits = request_limits(tools);
    auto request_body = detail::build_provider_request(config_, messages, tools).dump();
    const auto input_tokens = limits.estimated_tokens(request_body.size());
    const auto available_request = limits.available_request_tokens();
    if (available_request == 0 || input_tokens > available_request) {
        throw std::runtime_error("模型请求超过 Token 预算；请缩短上下文，或按模型/账户限额设置 "
                                 "max_request_tokens");
    }

    auto reply = model_detail::complete_provider_request(config_, std::move(request_body),
                                                         messages.size(), tools.size());
    if (reply.metadata.request_token_limit != 0) {
        learned_max_request_tokens_ = reply.metadata.request_token_limit;
    }
    const auto effective_limits = request_limits(tools);
    reply.metadata.max_request_tokens = effective_limits.max_request_tokens;
    reply.metadata.max_request_tokens_source = effective_limits.max_request_tokens_source;
    reply.metadata.response_header_max_request_tokens =
        effective_limits.response_header_max_request_tokens;
    reply.metadata.request_token_estimate_bytes_per_token =
        effective_limits.request_token_estimate_bytes_per_token;
    return reply;
}

} // namespace mint
