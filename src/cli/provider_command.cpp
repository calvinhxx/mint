#include "provider_command.hpp"

#include "provider_acceptance.hpp"

#include "mint/infrastructure/config.hpp"
#include "mint/infrastructure/model_provider_client.hpp"

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

Json provider_report(const CommandLine& command_line, const ModelProviderConfig& config,
                     const ModelProviderProfile& profile) {
    auto report = model_provider_profile_to_json(profile);
    report["schema_version"] = 1;
    report["operation"] = "inspect";
    report["config"] = normalized_path(command_line.config).generic_string();
    report["endpoint"] = public_endpoint(config.api_url);
    report["model"] = config.model;
    report["stream"] = config.stream;
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
    console.write_line("Provider: ", report.at("provider").get<std::string>(), "（",
                       report.at("source").get<std::string>(), "）");
    console.write_line("Adapter: ", report.at("adapter").get<std::string>());
    console.write_line("Endpoint: ", report.at("endpoint").get<std::string>());
    console.write_line("Model: ", report.at("model").get<std::string>());
    console.write_line("Authentication: ", report.at("authentication").get<std::string>());
    if (report.at("api_key_env").is_string()) {
        console.write_line("API Key env: ", report.at("api_key_env").get<std::string>());
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
    console.write_line("Token limit field: ",
                       capabilities.at("token_limit_parameter").get<std::string>());
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
}

} // namespace

int run_provider_command(const CommandLine& command_line, Console& console) {
    auto config = load_model_provider_config(command_line.config);
    const auto profile = resolve_model_provider_profile(config);
    auto report = provider_report(command_line, config, profile);

    if (command_line.provider_action == ProviderCommandAction::test) {
        constexpr long acceptance_max_completion_tokens = 1024;
        constexpr long acceptance_max_retries = 1;
        config.max_completion_tokens =
            std::min(config.max_completion_tokens, acceptance_max_completion_tokens);
        config.max_retries = std::min(config.max_retries, acceptance_max_retries);
        ModelProviderClient client(config);
        report["operation"] = "test";
        report["limits"] = {{"max_completion_tokens", config.max_completion_tokens},
                            {"max_attempts_per_request", config.max_retries + 1}};
        report["acceptance"] = provider_detail::run_provider_acceptance(client, config.stream);
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
