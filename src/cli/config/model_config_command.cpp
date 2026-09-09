#include "model_config_command.hpp"

#include "mint/infrastructure/model_config_service.hpp"
#include "mint/localization/localization.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace mint::cli {
namespace {

using localization::Message;
using localization::message;

constexpr std::size_t max_input_bytes = 64 * 1024;

struct PrivateInput {
    std::string text;
    Json document;
    ~PrivateInput() {
        std::fill(text.begin(), text.end(), '\0');
        if (document.is_object() && document.contains("api_key") &&
            document["api_key"].is_string()) {
            auto& key = document["api_key"].get_ref<std::string&>();
            std::fill(key.begin(), key.end(), '\0');
        }
    }
};

[[noreturn]] void invalid_input() {
    throw ModelConfigError(ModelConfigErrorCode::invalid_input,
                           message(Message::cli_config_invalid_input));
}

ModelConfigUpdate read_update(Console& console) {
    PrivateInput input;
    char byte = 0;
    while (console.input_stream().get(byte)) {
        if (input.text.size() == max_input_bytes) {
            invalid_input();
        }
        input.text.push_back(byte);
    }
    if (console.input_stream().bad()) {
        invalid_input();
    }
    input.document = Json::parse(input.text, nullptr, false);
    if (!input.document.is_object()) {
        invalid_input();
    }
    constexpr std::array<std::string_view, 6> fields = {
        "expected_revision", "adapter", "base_url", "model", "api_key", "remove_api_key"};
    for (const auto& item : input.document.items()) {
        if (std::find(fields.begin(), fields.end(), item.key()) == fields.end()) {
            invalid_input();
        }
    }
    for (const auto* field : {"expected_revision", "adapter", "base_url", "model"}) {
        if (!input.document.contains(field) || !input.document[field].is_string()) {
            invalid_input();
        }
    }
    ModelConfigUpdate update;
    const auto adapter = input.document["adapter"].get<std::string>();
    if (adapter == "chat_completions") {
        update.adapter = ModelAdapter::chat_completions;
    } else if (adapter == "responses") {
        update.adapter = ModelAdapter::responses;
    } else if (adapter == "anthropic_messages") {
        update.adapter = ModelAdapter::anthropic_messages;
    } else {
        invalid_input();
    }
    update.expected_revision = input.document["expected_revision"].get<std::string>();
    update.base_url = input.document["base_url"].get<std::string>();
    update.model = input.document["model"].get<std::string>();
    if (input.document.contains("remove_api_key")) {
        if (!input.document["remove_api_key"].is_boolean()) {
            invalid_input();
        }
        update.remove_api_key = input.document["remove_api_key"].get<bool>();
    }
    if (input.document.contains("api_key")) {
        if (!input.document["api_key"].is_string()) {
            invalid_input();
        }
        update.api_key = input.document["api_key"].get<std::string>();
    }
    return update;
}

Json public_snapshot(const ModelConfigSnapshot& snapshot, ConfigCommandAction action) {
    auto endpoint = snapshot.config.api_url;
    if (const auto suffix = endpoint.find_first_of("?#"); suffix != endpoint.npos) {
        endpoint.erase(suffix);
    }
    return {{"schema_version", 1},
            {"status", "ok"},
            {"operation", action == ConfigCommandAction::set      ? "set"
                          : action == ConfigCommandAction::import ? "import"
                                                                  : "show"},
            {"configured", snapshot.configured},
            {"config", snapshot.path.generic_string()},
            {"revision", snapshot.revision},
            {"override", snapshot.is_override},
            {"adapter", snapshot.configured ? model_adapter_name(snapshot.config.adapter) : ""},
            {"provider", snapshot.configured ? model_provider_name(snapshot.config.provider) : ""},
            {"endpoint", snapshot.configured ? endpoint : ""},
            {"model", snapshot.configured ? snapshot.config.model : ""},
            {"api_key_env", snapshot.config.api_key_env},
            {"saved_api_key", snapshot.saved_api_key},
            {"credential_available", snapshot.credential_available}};
}

std::string_view error_code(ModelConfigErrorCode code) {
    switch (code) {
    case ModelConfigErrorCode::invalid_input:
        return "invalid_input";
    case ModelConfigErrorCode::invalid_config:
        return "invalid_config";
    case ModelConfigErrorCode::conflict:
        return "conflict";
    case ModelConfigErrorCode::unavailable:
        return "unavailable";
    }
    return "unavailable";
}

} // namespace

int run_model_config_command(const CommandLine& command_line, Console& console) {
    try {
        ModelConfigService service(command_line.state_dir);
        ModelConfigSnapshot snapshot;
        switch (command_line.config_action) {
        case ConfigCommandAction::show:
            snapshot = service.inspect(command_line.config_specified ? command_line.config
                                                                     : std::filesystem::path{});
            break;
        case ConfigCommandAction::set:
            snapshot = service.update(read_update(console));
            break;
        case ConfigCommandAction::import:
            if (command_line.config_replace != !command_line.config_expected_revision.empty()) {
                invalid_input();
            }
            snapshot = service.migrate_legacy(
                command_line.config, command_line.config_replace
                                         ? std::optional(command_line.config_expected_revision)
                                         : std::nullopt);
            break;
        }
        console.write_line(public_snapshot(snapshot, command_line.config_action)
                               .dump(command_line.json_output ? -1 : 2));
        return 0;
    } catch (const ModelConfigError& error) {
        const auto explanation =
            error.code() == ModelConfigErrorCode::conflict
                ? message(command_line.config_action == ConfigCommandAction::import &&
                                  !command_line.config_replace
                              ? Message::cli_config_import_conflict
                              : Message::cli_config_conflict)
            : error.code() == ModelConfigErrorCode::invalid_input
                ? message(Message::cli_config_invalid_input)
                : message(Message::cli_config_unavailable);
        if (command_line.json_output) {
            console.write_line(Json{
                {"schema_version", 1},
                {"status", "error"},
                {"code", error_code(error.code())},
                {"error",
                 explanation}}.dump());
        } else {
            console.write_error_line(explanation);
        }
        return error.code() == ModelConfigErrorCode::conflict ? 2 : 1;
    } catch (const std::exception&) {
        const auto explanation = message(Message::cli_config_unavailable);
        if (command_line.json_output) {
            console.write_line(Json{
                {"schema_version", 1},
                {"status", "error"},
                {"code", "unavailable"},
                {"error",
                 explanation}}.dump());
        } else {
            console.write_error_line(explanation);
        }
        return 1;
    }
}

} // namespace mint::cli
