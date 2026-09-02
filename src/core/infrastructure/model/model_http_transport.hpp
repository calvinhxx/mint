#pragma once

#include "model_protocol.hpp"

#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

namespace mint::model_detail {

enum class HttpTransportOutcome { success, cancelled, retryable_failure, failure };

struct HttpAttempt {
    HttpTransportOutcome outcome = HttpTransportOutcome::success;
    int transport_code = 0;
    long http_status = 0;
    long retry_after_ms = -1;
    long token_reset_ms = -1;
    std::size_t token_limit = 0;
    std::size_t received_body_bytes = 0;
    std::string body;
    std::string transport_error;
    std::unique_ptr<detail::ModelStreamDecoder> stream;
    std::exception_ptr response_error;
    ModelResponseLimits response_limits{};
};

void ensure_http_runtime();

[[nodiscard]] HttpAttempt perform_http_attempt(const ModelProviderConfig& config,
                                               std::string_view request_body);

} // namespace mint::model_detail
