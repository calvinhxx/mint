#include "aiagent/infrastructure/config.hpp"

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace aiagent {
namespace {

std::string required_string(const Json& document, const char* field,
                            const std::filesystem::path& config_path) {
    if (!document.contains(field) || !document.at(field).is_string() ||
        document.at(field).get_ref<const std::string&>().empty()) {
        throw std::runtime_error("配置文件 " + config_path.string() + " 中的 \"" + field +
                                 "\" 必须是非空字符串");
    }
    return document.at(field).get<std::string>();
}

std::string optional_string(const Json& document, const char* field,
                            const std::filesystem::path& config_path) {
    if (!document.contains(field)) {
        return {};
    }
    if (!document.at(field).is_string()) {
        throw std::runtime_error("配置文件 " + config_path.string() + " 中的 \"" + field +
                                 "\" 必须是字符串");
    }
    return document.at(field).get<std::string>();
}

long optional_positive_integer(const Json& document, const char* field, long fallback,
                               const std::filesystem::path& config_path) {
    if (!document.contains(field)) {
        return fallback;
    }
    const auto& value = document.at(field);
    if (!value.is_number_integer()) {
        throw std::runtime_error("配置文件 " + config_path.string() + " 中的 \"" + field +
                                 "\" 必须是正整数");
    }
    const auto parsed = value.get<long long>();
    if (parsed <= 0 || parsed > std::numeric_limits<long>::max()) {
        throw std::runtime_error("配置文件 " + config_path.string() + " 中的 \"" + field +
                                 "\" 必须是正整数");
    }
    return static_cast<long>(parsed);
}

long optional_nonnegative_integer(const Json& document, const char* field, long fallback,
                                  long maximum, const std::filesystem::path& config_path) {
    if (!document.contains(field)) {
        return fallback;
    }
    const auto& value = document.at(field);
    if (!value.is_number_integer()) {
        throw std::runtime_error("配置文件 " + config_path.string() + " 中的 \"" + field +
                                 "\" 必须是非负整数");
    }
    const auto parsed = value.get<long long>();
    if (parsed < 0 || parsed > maximum) {
        throw std::runtime_error("配置文件 " + config_path.string() + " 中的 \"" + field +
                                 "\" 必须在 0 到 " + std::to_string(maximum) + " 之间");
    }
    return static_cast<long>(parsed);
}

} // namespace

ChatCompletionsConfig load_chat_completions_config(const std::filesystem::path& config_path) {
    std::ifstream input(config_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("找不到配置文件 " + config_path.string() +
                                 "。请复制 config.example.json 为 config.json，然后填写 API Key。");
    }

    Json document;
    try {
        input >> document;
    } catch (const Json::exception& error) {
        throw std::runtime_error("配置文件 " + config_path.string() +
                                 " 不是有效 JSON: " + error.what());
    }

    if (!document.is_object()) {
        throw std::runtime_error("配置文件 " + config_path.string() + " 的最外层必须是 JSON 对象");
    }

    ChatCompletionsConfig config;
    config.api_url = required_string(document, "api_url", config_path);
    config.api_key = optional_string(document, "api_key", config_path);
    config.model = required_string(document, "model", config_path);
    config.connect_timeout_seconds = optional_positive_integer(
        document, "connect_timeout_seconds", config.connect_timeout_seconds, config_path);
    config.request_timeout_seconds = optional_positive_integer(
        document, "request_timeout_seconds", config.request_timeout_seconds, config_path);
    config.max_retries =
        optional_nonnegative_integer(document, "max_retries", config.max_retries, 10, config_path);
    config.retry_initial_delay_ms = optional_positive_integer(
        document, "retry_initial_delay_ms", config.retry_initial_delay_ms, config_path);
    if (config.retry_initial_delay_ms > 60000) {
        throw std::runtime_error("配置文件 " + config_path.string() +
                                 " 中的 \"retry_initial_delay_ms\" 不能超过 60000");
    }
    config.max_completion_tokens = optional_positive_integer(
        document, "max_completion_tokens", config.max_completion_tokens, config_path);
    if (config.max_completion_tokens > 65536) {
        throw std::runtime_error("配置文件 " + config_path.string() +
                                 " 中的 \"max_completion_tokens\" 不能超过 65536");
    }
    return config;
}

} // namespace aiagent
