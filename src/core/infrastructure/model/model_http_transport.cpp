#include "model_http_transport.hpp"

#include "mint/localization/localization.hpp"
#include "mint/runtime/task_control.hpp"
#include "mint/version.hpp"

#include "model_provider_profile.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <curl/curl.h>

namespace mint::model_detail {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

class CurlGlobal final {
  public:
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error(message(Message::model_http_runtime_init_failed));
        }
    }

    ~CurlGlobal() {
        curl_global_cleanup();
    }

    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
};

struct CurlDeleter {
    void operator()(CURL* handle) const noexcept {
        curl_easy_cleanup(handle);
    }
};

struct HeaderDeleter {
    void operator()(curl_slist* headers) const noexcept {
        curl_slist_free_all(headers);
    }
};

using CurlHandle = std::unique_ptr<CURL, CurlDeleter>;
using HeaderList = std::unique_ptr<curl_slist, HeaderDeleter>;

long timeout_milliseconds(long seconds) {
    if (seconds > std::numeric_limits<long>::max() / 1000L) {
        return std::numeric_limits<long>::max();
    }
    return seconds * 1000L;
}

std::string trim_ascii(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

long parse_rate_limit_duration_ms(std::string text) noexcept {
    try {
        text = lowercase_ascii(trim_ascii(text));
        if (text.empty()) {
            return -1;
        }

        constexpr double maximum_delay_ms =
            static_cast<double>(model_provider_bounds::max_retry_initial_delay_ms);
        double total_ms = 0.0;
        std::size_t offset = 0;
        while (offset < text.size()) {
            while (offset < text.size() &&
                   std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
                ++offset;
            }
            if (offset == text.size()) {
                break;
            }

            std::size_t consumed = 0;
            const auto amount = std::stod(text.substr(offset), &consumed);
            if (consumed == 0 || amount < 0.0 || !std::isfinite(amount)) {
                return -1;
            }
            offset += consumed;

            double multiplier = 1000.0;
            if (text.compare(offset, 2, "ms") == 0) {
                multiplier = 1.0;
                offset += 2;
            } else if (offset < text.size() && text[offset] == 's') {
                multiplier = 1000.0;
                ++offset;
            } else if (offset < text.size() && text[offset] == 'm') {
                multiplier = 60.0 * 1000.0;
                ++offset;
            } else if (offset < text.size() && text[offset] == 'h') {
                multiplier = 60.0 * 60.0 * 1000.0;
                ++offset;
            } else if (offset != text.size()) {
                return -1;
            }

            total_ms += amount * multiplier;
            if (total_ms >= maximum_delay_ms) {
                return static_cast<long>(maximum_delay_ms);
            }
        }
        return static_cast<long>(std::ceil(total_ms));
    } catch (const std::exception&) {
        return -1;
    }
}

bool parse_token_limit(std::string_view text, std::size_t& token_limit) {
    const auto value = trim_ascii(text);
    if (value.empty()) {
        return false;
    }

    std::size_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return false;
    }

    token_limit = parsed;
    return true;
}

std::size_t append_response(char* data, std::size_t size, std::size_t count, void* user_data) {
    auto& attempt = *static_cast<HttpAttempt*>(user_data);
    try {
        if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
            throw std::runtime_error(message(Message::model_http_body_limit));
        }
        const auto bytes = size * count;
        const auto limit = attempt.response_limits.max_http_body_bytes;
        if (attempt.received_body_bytes > limit || bytes > limit - attempt.received_body_bytes) {
            throw std::runtime_error(
                message(Message::model_http_body_limit_bytes, {arg(Placeholder::limit, limit)}));
        }
        attempt.received_body_bytes += bytes;
        const bool successful_stream =
            attempt.stream != nullptr && attempt.http_status >= 200 && attempt.http_status < 300;
        if (!successful_stream) {
            attempt.body.append(data, bytes);
        }
        if (attempt.stream != nullptr && (attempt.http_status == 0 || successful_stream)) {
            attempt.stream->feed(std::string_view(data, bytes));
        }
        return bytes;
    } catch (...) {
        attempt.response_error = std::current_exception();
        return 0;
    }
}

std::size_t capture_response_header(char* data, std::size_t size, std::size_t count,
                                    void* user_data) {
    auto& attempt = *static_cast<HttpAttempt*>(user_data);
    try {
        if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
            throw std::runtime_error(message(Message::model_http_header_limit));
        }
        const auto bytes = size * count;
        const std::string_view line(data, bytes);
        if (line.starts_with("HTTP/")) {
            const auto separator = line.find(' ');
            if (separator != std::string_view::npos) {
                long status = 0;
                const auto begin = line.data() + separator + 1;
                const auto end = line.data() + line.size();
                if (const auto parsed = std::from_chars(begin, end, status);
                    parsed.ec == std::errc{}) {
                    attempt.http_status = status;
                }
            }
            return bytes;
        }

        const auto separator = line.find(':');
        if (separator == std::string_view::npos) {
            return bytes;
        }
        const auto name = lowercase_ascii(trim_ascii(line.substr(0, separator)));
        const auto value = trim_ascii(line.substr(separator + 1));
        if (name == "retry-after") {
            attempt.retry_after_ms = parse_rate_limit_duration_ms(value);
        } else if (name == "x-ratelimit-reset-tokens") {
            attempt.token_reset_ms = parse_rate_limit_duration_ms(value);
        } else if (name == "x-ratelimit-limit-tokens") {
            (void)parse_token_limit(value, attempt.token_limit);
        }
        return bytes;
    } catch (...) {
        attempt.response_error = std::current_exception();
        return 0;
    }
}

int abort_stopped_request(void* user_data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* control = static_cast<const TaskControl*>(user_data);
    return control != nullptr && control->should_stop() ? 1 : 0;
}

void append_header(HeaderList& headers, const std::string& value) {
    auto* updated = curl_slist_append(headers.get(), value.c_str());
    if (updated == nullptr) {
        throw std::runtime_error(message(Message::model_http_header_create_failed));
    }
    (void)headers.release();
    headers.reset(updated);
}

HttpTransportOutcome transport_outcome(CURLcode result) noexcept {
    if (result == CURLE_OK) {
        return HttpTransportOutcome::success;
    }
    if (result == CURLE_ABORTED_BY_CALLBACK) {
        return HttpTransportOutcome::cancelled;
    }
    if (result == CURLE_COULDNT_RESOLVE_HOST || result == CURLE_COULDNT_CONNECT ||
        result == CURLE_OPERATION_TIMEDOUT || result == CURLE_SEND_ERROR ||
        result == CURLE_RECV_ERROR || result == CURLE_PARTIAL_FILE || result == CURLE_GOT_NOTHING ||
        result == CURLE_HTTP2) {
        return HttpTransportOutcome::retryable_failure;
    }
    return HttpTransportOutcome::failure;
}

} // namespace

void ensure_http_runtime() {
    static CurlGlobal global;
    (void)global;
}

HttpAttempt perform_http_attempt(const ModelProviderConfig& config, std::string_view request_body) {
    HttpAttempt attempt;
    attempt.response_limits = config.response_limits;
    const auto profile = resolve_model_provider_profile(config);
    if (config.stream) {
        attempt.stream = std::make_unique<detail::ModelStreamDecoder>(
            profile.adapter, config.stream_event, config.response_limits);
    }

    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    CurlHandle handle(curl_easy_init());
    if (!handle) {
        throw std::runtime_error(message(Message::model_http_request_create_failed));
    }

    HeaderList headers;
    append_header(headers, "Content-Type: application/json");
    if (!config.api_key.empty()) {
        if (profile.adapter == ModelAdapter::anthropic_messages) {
            append_header(headers, "X-Api-Key: " + config.api_key);
        } else {
            append_header(headers, "Authorization: Bearer " + config.api_key);
        }
    }
    if (profile.adapter == ModelAdapter::anthropic_messages) {
        append_header(headers, "Anthropic-Version: 2023-06-01");
    }
    if (profile.provider == ModelProvider::google) {
        append_header(headers, "X-Goog-Api-Client: mint/" + std::string(version));
    }

    curl_easy_setopt(handle.get(), CURLOPT_URL, config.api_url.c_str());
    if (is_plaintext_loopback_model_endpoint(config.api_url) &&
        curl_easy_setopt(handle.get(), CURLOPT_NOPROXY, "*") != CURLE_OK) {
        throw std::runtime_error(message(Message::model_http_proxy_disable_failed));
    }
    curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, request_body.data());
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(request_body.size()));
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append_response);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &attempt);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, capture_response_header);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &attempt);

    auto connect_timeout_ms = timeout_milliseconds(config.connect_timeout_seconds);
    auto request_timeout_ms = timeout_milliseconds(config.request_timeout_seconds);
    if (config.task_control != nullptr) {
        const auto remaining = config.task_control->remaining_milliseconds();
        if (remaining >= 0) {
            const auto bounded = static_cast<long>(std::max<long long>(
                1, std::min<long long>(remaining, std::numeric_limits<long>::max())));
            connect_timeout_ms = std::min(connect_timeout_ms, bounded);
            request_timeout_ms = std::min(request_timeout_ms, bounded);
        }
    }
    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, request_timeout_ms);
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_ACCEPT_ENCODING, "");
    const auto user_agent = "mint/" + std::string(version);
    curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, error_buffer.data());
    if (config.task_control != nullptr) {
        curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, abort_stopped_request);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, config.task_control.get());
    }

    const auto result = curl_easy_perform(handle.get());
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &attempt.http_status);
    attempt.transport_code = static_cast<int>(result);
    attempt.outcome = transport_outcome(result);
    if (result != CURLE_OK) {
        attempt.transport_error =
            error_buffer.front() == '\0' ? curl_easy_strerror(result) : error_buffer.data();
    }
    return attempt;
}

} // namespace mint::model_detail
