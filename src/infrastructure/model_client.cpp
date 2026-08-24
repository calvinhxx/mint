#include "aiagent/infrastructure/model_provider_client.hpp"
#include "aiagent/runtime/task_control.hpp"
#include "aiagent/version.hpp"

#include "model_protocol.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include <curl/curl.h>

namespace aiagent {
namespace {

class CurlGlobal final {
  public:
    CurlGlobal() {
        const auto result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result != CURLE_OK) {
            throw std::runtime_error("初始化 HTTP 运行时失败");
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

struct ResponseHeaders {
    long http_status = 0;
    long retry_after_ms = -1;
    long token_reset_ms = -1;
};

void ensure_curl_is_initialized() {
    static CurlGlobal global;
    (void)global;
}

long timeout_milliseconds(long seconds) {
    if (seconds > std::numeric_limits<long>::max() / 1000L) {
        return std::numeric_limits<long>::max();
    }
    return seconds * 1000L;
}

struct ResponseSink {
    std::string body;
    std::unique_ptr<detail::ModelStreamDecoder> stream;
    std::exception_ptr stream_error;
    ResponseHeaders headers;
};

std::size_t append_response(char* data, std::size_t size, std::size_t count, void* user_data) {
    const auto bytes = size * count;
    auto& response = *static_cast<ResponseSink*>(user_data);
    try {
        response.body.append(data, bytes);
        if (response.stream != nullptr &&
            (response.headers.http_status == 0 ||
             (response.headers.http_status >= 200 && response.headers.http_status < 300))) {
            response.stream->feed(std::string_view(data, bytes));
        }
        return bytes;
    } catch (...) {
        response.stream_error = std::current_exception();
        return 0;
    }
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

        constexpr double maximum_delay_ms = 60000.0;
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

std::size_t capture_response_header(char* data, std::size_t size, std::size_t count,
                                    void* user_data) {
    const auto bytes = size * count;
    auto& headers = static_cast<ResponseSink*>(user_data)->headers;
    const std::string_view line(data, bytes);
    if (line.starts_with("HTTP/")) {
        const auto separator = line.find(' ');
        if (separator != std::string_view::npos) {
            long status = 0;
            const auto begin = line.data() + separator + 1;
            const auto end = line.data() + line.size();
            const auto parsed = std::from_chars(begin, end, status);
            if (parsed.ec == std::errc{}) {
                headers.http_status = status;
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
        headers.retry_after_ms = parse_rate_limit_duration_ms(value);
    } else if (name == "x-ratelimit-reset-tokens") {
        headers.token_reset_ms = parse_rate_limit_duration_ms(value);
    }
    return bytes;
}

int abort_stopped_request(void* user_data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* control = static_cast<const TaskControl*>(user_data);
    return control != nullptr && control->should_stop() ? 1 : 0;
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
        // Fall through to a short raw response. It often contains useful proxy errors.
    }

    constexpr std::size_t max_error_length = 800;
    const auto shortened = body.substr(0, max_error_length);
    return "模型接口返回 HTTP " + std::to_string(status) +
           (shortened.empty() ? std::string{} : ": " + shortened);
}

void truncate_utf8_at_boundary(std::string& value, std::size_t byte_limit) {
    if (value.size() <= byte_limit) {
        return;
    }

    auto boundary = byte_limit;
    while (boundary > 0 && (static_cast<unsigned char>(value[boundary]) & 0xC0U) == 0x80U) {
        --boundary;
    }
    value.resize(boundary);
    value += "...";
}

ModelReply demo_tool_call(std::string id, std::string name, Json arguments) {
    Json raw_call = {{"id", id},
                     {"type", "function"},
                     {"function", {{"name", name}, {"arguments", arguments.dump()}}}};

    ModelReply reply;
    reply.assistant_message = {
        {"role", "assistant"}, {"content", nullptr}, {"tool_calls", Json::array({raw_call})}};
    reply.tool_calls.push_back(ToolCall{std::move(id), std::move(name), std::move(arguments)});
    return reply;
}

} // namespace

std::string_view model_adapter_name(ModelAdapter adapter) noexcept {
    switch (adapter) {
    case ModelAdapter::chat_completions:
        return "chat_completions";
    case ModelAdapter::responses:
        return "responses";
    }
    return "unknown";
}

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
        config_.max_retries < 0 || config_.max_retries > 10 ||
        config_.retry_initial_delay_ms <= 0 || config_.retry_initial_delay_ms > 60000 ||
        config_.max_completion_tokens <= 0 || config_.max_completion_tokens > 65536) {
        throw std::invalid_argument("模型超时或重试配置超出允许范围");
    }
    ensure_curl_is_initialized();
}

ModelReply ModelProviderClient::complete(const Json& messages, const Json& tools) {
    const auto request_started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - request_started)
            .count();
    };
    const auto emit_progress = [&](ModelProgress progress) {
        if (config_.progress) {
            progress.max_attempts = static_cast<std::size_t>(config_.max_retries + 1);
            progress.elapsed_ms = elapsed_ms();
            config_.progress(progress);
        }
    };
    const auto request_body = detail::build_provider_request(config_, messages, tools).dump();
    struct Attempt {
        CURLcode curl_result = CURLE_OK;
        long http_status = 0;
        ResponseSink response;
        std::string curl_error;
    };

    const auto perform_attempt = [&]() {
        Attempt attempt;
        if (config_.stream) {
            attempt.response.stream =
                std::make_unique<detail::ModelStreamDecoder>(config_.adapter, config_.stream_event);
        }
        std::array<char, CURL_ERROR_SIZE> error_buffer{};
        CurlHandle handle(curl_easy_init());
        if (!handle) {
            throw std::runtime_error("无法创建 HTTP 请求");
        }

        curl_slist* raw_headers = nullptr;
        raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");
        if (!config_.api_key.empty()) {
            const auto authorization = "Authorization: Bearer " + config_.api_key;
            raw_headers = curl_slist_append(raw_headers, authorization.c_str());
        }
        HeaderList headers(raw_headers);
        if (!headers) {
            throw std::runtime_error("无法创建 HTTP 请求头");
        }

        curl_easy_setopt(handle.get(), CURLOPT_URL, config_.api_url.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, request_body.data());
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(request_body.size()));
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append_response);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &attempt.response);
        curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, capture_response_header);
        curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &attempt.response);
        auto connect_timeout_ms = timeout_milliseconds(config_.connect_timeout_seconds);
        auto request_timeout_ms = timeout_milliseconds(config_.request_timeout_seconds);
        if (config_.task_control != nullptr) {
            const auto remaining = config_.task_control->remaining_milliseconds();
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
        const auto user_agent = "aiagent/" + std::string(version);
        curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, user_agent.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, error_buffer.data());
        if (config_.task_control != nullptr) {
            curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, abort_stopped_request);
            curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, config_.task_control.get());
        }

        attempt.curl_result = curl_easy_perform(handle.get());
        curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &attempt.http_status);
        if (error_buffer.front() != '\0') {
            attempt.curl_error = error_buffer.data();
        }
        return attempt;
    };

    const auto transient_curl_error = [](CURLcode result) {
        return result == CURLE_COULDNT_RESOLVE_HOST || result == CURLE_COULDNT_CONNECT ||
               result == CURLE_OPERATION_TIMEDOUT || result == CURLE_SEND_ERROR ||
               result == CURLE_RECV_ERROR || result == CURLE_PARTIAL_FILE ||
               result == CURLE_GOT_NOTHING || result == CURLE_HTTP2;
    };
    const auto transient_http_error = [](long status) {
        return status == 408 || status == 429 || (status >= 500 && status <= 599);
    };
    const auto wait_before_retry = [&](long delay_ms) {
        auto remaining = delay_ms;
        while (remaining > 0) {
            if (config_.task_control != nullptr && config_.task_control->should_stop()) {
                throw std::runtime_error(config_.task_control->cancellation_requested()
                                             ? "模型请求已取消"
                                             : "模型请求超过任务总时间预算");
            }
            const auto slice = std::min<long>(remaining, 25);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            remaining -= slice;
        }
    };

    long delay_ms = config_.retry_initial_delay_ms;
    for (long attempt_index = 0;; ++attempt_index) {
        const auto attempt_number = static_cast<std::size_t>(attempt_index + 1);
        emit_progress({.kind = ModelProgressKind::attempt_started, .attempt = attempt_number});
        if (config_.stream) {
            emit_progress({.kind = ModelProgressKind::stream_started, .attempt = attempt_number});
        }
        Attempt attempt;
        try {
            attempt = perform_attempt();
        } catch (...) {
            emit_progress({.kind = ModelProgressKind::request_failed, .attempt = attempt_number});
            throw;
        }
        if (attempt.response.stream_error != nullptr) {
            emit_progress({.kind = ModelProgressKind::request_failed,
                           .attempt = attempt_number,
                           .http_status = attempt.http_status});
            std::rethrow_exception(attempt.response.stream_error);
        }
        if (attempt.curl_result == CURLE_OK && attempt.http_status >= 200 &&
            attempt.http_status < 300) {
            try {
                Json response;
                std::size_t stream_events = 0;
                std::size_t streamed_bytes = 0;
                if (config_.stream) {
                    response = attempt.response.stream->finish();
                    stream_events = attempt.response.stream->event_count();
                    streamed_bytes = attempt.response.stream->streamed_bytes();
                } else {
                    response = Json::parse(attempt.response.body);
                }
                auto reply = detail::parse_provider_response(config_.adapter, response);
                reply.metadata.adapter = model_adapter_name(config_.adapter);
                if (reply.metadata.model.empty()) {
                    reply.metadata.model = config_.model;
                }
                reply.metadata.attempts = static_cast<std::size_t>(attempt_index + 1);
                reply.metadata.retries = static_cast<std::size_t>(attempt_index);
                reply.metadata.http_status = attempt.http_status;
                reply.metadata.duration_ms = elapsed_ms();
                reply.metadata.streamed = config_.stream;
                reply.metadata.stream_events = stream_events;
                reply.metadata.streamed_bytes = streamed_bytes;
                if (config_.stream) {
                    emit_progress({.kind = ModelProgressKind::stream_completed,
                                   .attempt = attempt_number,
                                   .http_status = attempt.http_status,
                                   .stream_events = stream_events,
                                   .streamed_bytes = streamed_bytes});
                }
                emit_progress({.kind = ModelProgressKind::request_succeeded,
                               .attempt = attempt_number,
                               .http_status = attempt.http_status});
                return reply;
            } catch (const Json::exception& error) {
                emit_progress({.kind = ModelProgressKind::request_failed,
                               .attempt = attempt_number,
                               .http_status = attempt.http_status});
                throw std::runtime_error("模型返回的内容不是有效 JSON: " +
                                         std::string(error.what()));
            } catch (...) {
                emit_progress({.kind = ModelProgressKind::request_failed,
                               .attempt = attempt_number,
                               .http_status = attempt.http_status});
                throw;
            }
        }

        if (attempt.curl_result == CURLE_ABORTED_BY_CALLBACK && config_.task_control != nullptr &&
            config_.task_control->should_stop()) {
            emit_progress({.kind = ModelProgressKind::request_failed,
                           .attempt = attempt_number,
                           .http_status = attempt.http_status});
            throw std::runtime_error(config_.task_control->cancellation_requested()
                                         ? "模型请求已取消"
                                         : "模型请求超过任务总时间预算");
        }

        const bool retryable = attempt.curl_result != CURLE_OK
                                   ? transient_curl_error(attempt.curl_result)
                                   : transient_http_error(attempt.http_status);
        if (!retryable || attempt_index >= config_.max_retries) {
            emit_progress({.kind = ModelProgressKind::request_failed,
                           .attempt = attempt_number,
                           .http_status = attempt.http_status});
            if (attempt.curl_result != CURLE_OK) {
                const auto details = attempt.curl_error.empty()
                                         ? std::string(curl_easy_strerror(attempt.curl_result))
                                         : attempt.curl_error;
                throw std::runtime_error("请求模型失败: " + details);
            }
            throw std::runtime_error(response_error(attempt.response.body, attempt.http_status));
        }

        auto effective_delay_ms = delay_ms;
        if (attempt.http_status == 429) {
            const auto server_delay_ms = std::max(attempt.response.headers.retry_after_ms,
                                                  attempt.response.headers.token_reset_ms);
            if (server_delay_ms >= 0) {
                effective_delay_ms = std::max(effective_delay_ms, server_delay_ms + 50);
            }
        }
        emit_progress({.kind = ModelProgressKind::retry_scheduled,
                       .attempt = attempt_number,
                       .http_status = attempt.http_status,
                       .delay_ms = effective_delay_ms});
        try {
            wait_before_retry(effective_delay_ms);
        } catch (...) {
            emit_progress({.kind = ModelProgressKind::request_failed,
                           .attempt = attempt_number,
                           .http_status = attempt.http_status});
            throw;
        }
        delay_ms = std::min<long>(delay_ms * 2, 60000);
    }
}

ModelReply DemoModelClient::complete(const Json& messages, const Json& tools) {
    (void)tools;

    switch (step_++) {
    case 0: {
        auto reply = demo_tool_call("demo-list", "list_files", {{"path", "."}, {"max_depth", 2}});
        reply.metadata = {.adapter = "demo", .model = "deterministic-demo"};
        return reply;
    }
    case 1: {
        auto reply = demo_tool_call("demo-search", "search_text",
                                    {{"path", "."}, {"query", "Agent"}, {"case_sensitive", false}});
        reply.metadata = {.adapter = "demo", .model = "deterministic-demo"};
        return reply;
    }
    case 2: {
        auto reply = demo_tool_call("demo-read", "read_file", {{"path", "README.md"}});
        reply.metadata = {.adapter = "demo", .model = "deterministic-demo"};
        return reply;
    }
    default: {
        std::string last_result;
        if (messages.is_array() && !messages.empty()) {
            const auto& last = messages.back();
            if (last.is_object() && last.contains("content") && last.at("content").is_string()) {
                last_result = last.at("content").get<std::string>();
            }
        }
        constexpr std::size_t evidence_limit = 700;
        truncate_utf8_at_boundary(last_result, evidence_limit);

        ModelReply reply;
        reply.text = "离线演示完成：Agent 已依次列出文件、搜索文本并读取 README.md。"
                     "\n\n最后一次工具结果：\n" +
                     last_result;
        reply.assistant_message = {{"role", "assistant"}, {"content", reply.text}};
        reply.metadata = {.adapter = "demo", .model = "deterministic-demo"};
        return reply;
    }
    }
}

} // namespace aiagent
