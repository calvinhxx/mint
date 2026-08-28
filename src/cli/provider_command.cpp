#include "provider_command.hpp"

#include "mint/infrastructure/config.hpp"
#include "mint/infrastructure/model_provider_client.hpp"

#include <string>

namespace mint::cli {
namespace {

std::string public_endpoint(std::string endpoint) {
    if (const auto suffix = endpoint.find_first_of("?#"); suffix != std::string::npos) {
        endpoint.erase(suffix);
    }
    return endpoint;
}

Json provider_report(const CommandLine& command_line) {
    const auto config = load_model_provider_config(command_line.config);
    const auto profile = resolve_model_provider_profile(config);
    auto report = model_provider_profile_to_json(profile);
    report["schema_version"] = 1;
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

} // namespace

int run_provider_command(const CommandLine& command_line, Console& console) {
    const auto report = provider_report(command_line);
    if (command_line.json_output) {
        console.write_line(report.dump());
        return 0;
    }

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
        ", reasoning_replay=", enabled(capabilities.at("stateless_reasoning_replay").get<bool>()));
    console.write_line("Token limit field: ",
                       capabilities.at("token_limit_parameter").get<std::string>());
    return 0;
}

} // namespace mint::cli
