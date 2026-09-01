#include "provider_command.hpp"

#include "provider_acceptance.hpp"

#include "mint/infrastructure/config.hpp"
#include "mint/infrastructure/model_provider_client.hpp"
#include "mint/runtime/terminal_text.hpp"

#include <algorithm>
#include <string>

namespace mint::cli {
namespace {

std::string public_endpoint(std::string endpoint) {
    if (const auto suffix = endpoint.find_first_of("?#"); suffix != std::string::npos) {
        endpoint.erase(suffix);
    }
    return endpoint;
}

Json request_limits(const ModelProviderConfig& config,
                    const ModelRequestLimits* effective_limits = nullptr) {
    const auto automatic = config.max_request_tokens == 0;
    const auto maximum = effective_limits == nullptr
                             ? (automatic ? model_provider_defaults::automatic_max_request_tokens
                                          : static_cast<std::size_t>(config.max_request_tokens))
                             : effective_limits->max_request_tokens;
    const auto source =
        effective_limits == nullptr
            ? (automatic ? std::string_view{"automatic"} : std::string_view{"config"})
            : model_request_limit_source_name(effective_limits->max_request_tokens_source);
    const auto learned = effective_limits == nullptr
                             ? std::size_t{0}
                             : effective_limits->response_header_max_request_tokens;
    return {
        {"max_request_tokens", maximum},
        {"max_request_tokens_source", source},
        {"response_header_max_request_tokens", learned == 0 ? Json(nullptr) : Json(learned)},
        {"request_token_safety_margin", config.request_token_safety_margin},
        {"request_token_estimate_bytes_per_token", config.request_token_estimate_bytes_per_token},
        {"max_completion_tokens", config.max_completion_tokens},
        {"max_attempts_per_request", config.max_retries + 1}};
}

Json provider_report(const CommandLine& command_line, const ModelProviderConfig& config,
                     const ModelProviderProfile& profile) {
    auto report = model_provider_profile_to_json(profile);
    report["schema_version"] = 1;
    report["operation"] = "inspect";
    report["config"] = normalized_path(command_line.config).generic_string();
    report["endpoint"] = public_endpoint(config.api_url);
    report["model"] = config.model;
    report["stream"] = config.stream;
    report["limits"] = request_limits(config);
    if (!config.api_key_env.empty()) {
        report["authentication"] = "environment";
        report["api_key_env"] = config.api_key_env;
    } else {
        report["authentication"] = config.api_key.empty() ? "none" : "inline_api_key";
        report["api_key_env"] = nullptr;
    }
    return report;
}

const char* enabled(bool value) {
    return value ? "yes" : "no";
}

void print_provider(const Json& report, Console& console) {
    const auto& capabilities = report.at("capabilities");
    console.write_line(
        "Provider: ", escape_terminal_field(report.at("provider").get<std::string>()), "（",
        escape_terminal_field(report.at("source").get<std::string>()), "）");
    console.write_line("Adapter: ", escape_terminal_field(report.at("adapter").get<std::string>()));
    console.write_line("Endpoint: ",
                       escape_terminal_field(report.at("endpoint").get<std::string>()));
    console.write_line("Model: ", escape_terminal_field(report.at("model").get<std::string>()));
    console.write_line("Authentication: ",
                       escape_terminal_field(report.at("authentication").get<std::string>()));
    if (report.at("api_key_env").is_string()) {
        console.write_line("API Key env: ",
                           escape_terminal_field(report.at("api_key_env").get<std::string>()));
    }
    console.write_line(
        "Capabilities: tools=", enabled(capabilities.at("function_tools").get<bool>()),
        ", streaming=", enabled(capabilities.at("streaming").get<bool>()),
        ", stream_usage=", enabled(capabilities.at("stream_usage").get<bool>()),
        ", explicit_tool_choice=", enabled(capabilities.at("explicit_tool_choice").get<bool>()),
        ", chat_reasoning_replay=", enabled(capabilities.at("chat_reasoning_replay").get<bool>()),
        ", requires_tool_call_content=",
        enabled(capabilities.at("requires_tool_call_content").get<bool>()),
        ", responses_reasoning_replay=",
        enabled(capabilities.at("stateless_reasoning_replay").get<bool>()));
    console.write_line(
        "Token limit field: ",
        escape_terminal_field(capabilities.at("token_limit_parameter").get<std::string>()));
    const auto& limits = report.at("limits");
    console.write_line(
        "Request limits: total_tokens=", limits.at("max_request_tokens").get<std::size_t>(), " (",
        escape_terminal_field(limits.at("max_request_tokens_source").get<std::string>()),
        "), output_tokens=", limits.at("max_completion_tokens").get<long>(),
        ", safety_margin=", limits.at("request_token_safety_margin").get<long>(),
        ", max_attempts=", limits.at("max_attempts_per_request").get<long>());
}

void print_acceptance(const Json& report, Console& console) {
    const auto& acceptance = report.at("acceptance");
    const auto& usage = acceptance.at("usage");
    console.write_line("Live test: passed");
    console.write_line("Requests: ", acceptance.at("requests").get<std::size_t>(),
                       ", attempts: ", acceptance.at("attempts").get<std::size_t>(),
                       ", retries: ", acceptance.at("retries").get<std::size_t>());
    console.write_line("Tool loop: function call, arguments and continuation passed");
    console.write_line("Usage: ", usage.at("total_tokens").get<std::size_t>(), " tokens from ",
                       usage.at("reported_requests").get<std::size_t>(), " reported requests");
    if (usage.at("cache_hit_rate").is_number()) {
        console.write_line("Prompt cache: ", usage.at("cached_tokens").get<std::size_t>(), "/",
                           usage.at("prompt_tokens").get<std::size_t>(), " tokens (",
                           usage.at("cache_hit_rate").get<double>() * 100.0, "% hit)");
    } else {
        console.write_line("Prompt cache: n/a (no input tokens reported)");
    }
}

} // namespace

int run_provider_command(const CommandLine& command_line, Console& console) {
    auto config = load_model_provider_config(command_line.config);
    const auto profile = resolve_model_provider_profile(config);
    auto report = provider_report(command_line, config, profile);

    if (command_line.provider_action == ProviderCommandAction::test) {
        constexpr long acceptance_max_completion_tokens = 1024;
        constexpr long acceptance_max_retries = 0;
        config.max_completion_tokens =
            std::min(config.max_completion_tokens, acceptance_max_completion_tokens);
        config.max_retries = std::min(config.max_retries, acceptance_max_retries);
        ModelProviderClient client(config);
        report["operation"] = "test";
        report["limits"] = request_limits(config);
        report["acceptance"] = provider_detail::run_provider_acceptance(client, config.stream);
        const auto effective_limits = client.request_limits(Json::array());
        report["limits"] = request_limits(config, &effective_limits);
        report["status"] = "passed";
    }

    if (command_line.json_output) {
        console.write_line(report.dump());
        return 0;
    }
    print_provider(report, console);
    if (command_line.provider_action == ProviderCommandAction::test) {
        print_acceptance(report, console);
    }
    return 0;
}

} // namespace mint::cli
