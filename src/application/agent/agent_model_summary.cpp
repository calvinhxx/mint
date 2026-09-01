#include "agent_model_summary.hpp"

#include <stdexcept>

namespace mint::agent_detail {

Json model_usage_json(const ModelUsage& usage) {
    if (!usage.available) {
        return nullptr;
    }
    return {{"prompt_tokens", usage.prompt_tokens},
            {"completion_tokens", usage.completion_tokens},
            {"total_tokens", usage.total_tokens},
            {"cached_tokens", usage.cached_tokens},
            {"cache_hit_rate", model_usage::cache_hit_rate_json(usage)}};
}

Json model_metadata_json(const ModelCallMetadata& metadata) {
    return {
        {"adapter", metadata.adapter},
        {"provider", metadata.provider},
        {"response_id", metadata.response_id.empty() ? Json(nullptr) : Json(metadata.response_id)},
        {"model", metadata.model},
        {"attempts", metadata.attempts},
        {"retries", metadata.retries},
        {"http_status", metadata.http_status},
        {"duration_ms", metadata.duration_ms},
        {"streamed", metadata.streamed},
        {"stream_events", metadata.stream_events},
        {"streamed_bytes", metadata.streamed_bytes},
        {"request_token_limit", metadata.request_token_limit},
        {"max_request_tokens", metadata.max_request_tokens},
        {"max_request_tokens_source",
         model_request_limit_source_name(metadata.max_request_tokens_source)},
        {"response_header_max_request_tokens", metadata.response_header_max_request_tokens},
        {"request_token_estimate_bytes_per_token",
         metadata.request_token_estimate_bytes_per_token}};
}

void record_model_call(ModelSummary& summary, const ModelReply& reply) {
    ++summary.calls;
    summary.attempts += reply.metadata.attempts;
    summary.retries += reply.metadata.retries;
    summary.duration_ms += reply.metadata.duration_ms;
    if (reply.metadata.streamed) {
        ++summary.streamed_calls;
        summary.stream_events += reply.metadata.stream_events;
        summary.streamed_bytes += reply.metadata.streamed_bytes;
    }
    if (!reply.metadata.adapter.empty()) {
        summary.adapter = reply.metadata.adapter;
    }
    if (!reply.metadata.provider.empty()) {
        summary.provider = reply.metadata.provider;
    }
    if (!reply.metadata.model.empty()) {
        summary.model = reply.metadata.model;
    }
    if (!reply.metadata.response_id.empty()) {
        summary.last_response_id = reply.metadata.response_id;
    }
    if (reply.metadata.max_request_tokens != 0) {
        summary.max_request_tokens = reply.metadata.max_request_tokens;
        summary.max_request_tokens_source =
            model_request_limit_source_name(reply.metadata.max_request_tokens_source);
        summary.response_header_max_request_tokens =
            reply.metadata.response_header_max_request_tokens;
        summary.request_token_estimate_bytes_per_token =
            reply.metadata.request_token_estimate_bytes_per_token;
    }
    if (reply.usage.available) {
        ++summary.usage_reports;
        summary.prompt_tokens += reply.usage.prompt_tokens;
        summary.completion_tokens += reply.usage.completion_tokens;
        summary.total_tokens += reply.usage.total_tokens;
        summary.cached_tokens += reply.usage.cached_tokens;
    }
}

Json model_summary_to_json(const ModelSummary& summary) {
    return {
        {"calls", summary.calls},
        {"attempts", summary.attempts},
        {"retries", summary.retries},
        {"usage_reports", summary.usage_reports},
        {"prompt_tokens", summary.prompt_tokens},
        {"completion_tokens", summary.completion_tokens},
        {"total_tokens", summary.total_tokens},
        {"cached_tokens", summary.cached_tokens},
        {"cache_hit_rate",
         model_usage::cache_hit_rate_json(summary.cached_tokens, summary.prompt_tokens)},
        {"streamed_calls", summary.streamed_calls},
        {"stream_events", summary.stream_events},
        {"streamed_bytes", summary.streamed_bytes},
        {"duration_ms", summary.duration_ms},
        {"adapter", summary.adapter},
        {"provider", summary.provider},
        {"model", summary.model},
        {"max_request_tokens", summary.max_request_tokens},
        {"max_request_tokens_source", summary.max_request_tokens_source.empty()
                                          ? Json(nullptr)
                                          : Json(summary.max_request_tokens_source)},
        {"response_header_max_request_tokens",
         summary.response_header_max_request_tokens == 0
             ? Json(nullptr)
             : Json(summary.response_header_max_request_tokens)},
        {"request_token_estimate_bytes_per_token", summary.request_token_estimate_bytes_per_token},
        {"last_response_id",
         summary.last_response_id.empty() ? Json(nullptr) : Json(summary.last_response_id)}};
}

ModelSummary model_summary_from_json(const Json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("会话模型摘要格式无效");
    }
    ModelSummary summary;
    const auto read_size = [&](const char* field) {
        if (!value.contains(field) || !value.at(field).is_number_unsigned()) {
            throw std::invalid_argument("会话模型摘要字段无效: " + std::string(field));
        }
        return value.at(field).get<std::size_t>();
    };
    summary.calls = read_size("calls");
    summary.attempts = read_size("attempts");
    summary.retries = read_size("retries");
    summary.usage_reports = read_size("usage_reports");
    summary.prompt_tokens = read_size("prompt_tokens");
    summary.completion_tokens = read_size("completion_tokens");
    summary.total_tokens = read_size("total_tokens");
    summary.cached_tokens = read_size("cached_tokens");
    const auto read_optional_size = [&](const char* field) {
        if (!value.contains(field)) {
            return std::size_t{0};
        }
        if (!value.at(field).is_number_unsigned()) {
            throw std::invalid_argument("会话模型摘要字段无效: " + std::string(field));
        }
        return value.at(field).get<std::size_t>();
    };
    summary.streamed_calls = read_optional_size("streamed_calls");
    summary.stream_events = read_optional_size("stream_events");
    summary.streamed_bytes = read_optional_size("streamed_bytes");
    if (!value.contains("duration_ms") || !value.at("duration_ms").is_number_integer() ||
        !value.contains("adapter") || !value.at("adapter").is_string() ||
        !value.contains("model") || !value.at("model").is_string()) {
        throw std::invalid_argument("会话模型摘要标识或耗时无效");
    }
    summary.duration_ms = value.at("duration_ms").get<long long>();
    summary.adapter = value.at("adapter").get<std::string>();
    if (value.contains("provider")) {
        if (!value.at("provider").is_string()) {
            throw std::invalid_argument("会话模型摘要 provider 无效");
        }
        summary.provider = value.at("provider").get<std::string>();
    }
    summary.model = value.at("model").get<std::string>();
    if (value.contains("last_response_id") && value.at("last_response_id").is_string()) {
        summary.last_response_id = value.at("last_response_id").get<std::string>();
    } else if (!value.contains("last_response_id") || !value.at("last_response_id").is_null()) {
        throw std::invalid_argument("会话模型摘要 response id 无效");
    }
    summary.max_request_tokens = read_optional_size("max_request_tokens");
    if (value.contains("max_request_tokens_source") &&
        value.at("max_request_tokens_source").is_string()) {
        summary.max_request_tokens_source =
            value.at("max_request_tokens_source").get<std::string>();
    } else if (value.contains("max_request_tokens_source") &&
               !value.at("max_request_tokens_source").is_null()) {
        throw std::invalid_argument("会话模型摘要请求预算来源无效");
    }
    if (value.contains("response_header_max_request_tokens") &&
        !value.at("response_header_max_request_tokens").is_null()) {
        summary.response_header_max_request_tokens =
            read_optional_size("response_header_max_request_tokens");
    }
    if (value.contains("request_token_estimate_bytes_per_token")) {
        summary.request_token_estimate_bytes_per_token =
            read_optional_size("request_token_estimate_bytes_per_token");
        if (summary.request_token_estimate_bytes_per_token == 0) {
            throw std::invalid_argument("会话模型摘要 Token 估算无效");
        }
    }
    if (summary.attempts < summary.calls || summary.retries > summary.attempts ||
        summary.usage_reports > summary.calls || summary.duration_ms < 0 ||
        summary.cached_tokens > summary.prompt_tokens || summary.streamed_calls > summary.calls) {
        throw std::invalid_argument("会话模型摘要计数不一致");
    }
    return summary;
}

void print_model_usage(std::ostream& output, const ModelUsage& usage) {
    if (!usage.available) {
        return;
    }
    output << "[Token] 输入 " << usage.prompt_tokens;
    if (usage.prompt_tokens != 0) {
        output << "（缓存 " << usage.cached_tokens << "，命中 "
               << static_cast<std::size_t>(*model_usage::cache_hit_rate(usage) * 100.0) << "%）";
    }
    output << "，输出 " << usage.completion_tokens << "，合计 " << usage.total_tokens << '\n';
}

} // namespace mint::agent_detail
