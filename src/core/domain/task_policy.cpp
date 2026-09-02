#include "mint/domain/task_policy.hpp"

#include "mint/domain/model.hpp"
#include "mint/localization/localization.hpp"
#include "mint/version.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mint {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

constexpr std::size_t max_policy_bytes = 256 * 1024;
constexpr std::size_t max_recipe_count = 32;
constexpr std::size_t max_command_read_path_count = 64;

void reject_unknown_fields(const Json& object, const std::set<std::string>& allowed,
                           std::string_view context) {
    for (const auto& [key, value] : object.items()) {
        (void)value;
        if (!allowed.contains(key)) {
            throw std::invalid_argument(
                message(Message::validation_unknown_field,
                        {arg(Placeholder::context, context), arg(Placeholder::field, key)}));
        }
    }
}

std::size_t optional_size(const Json& object, const char* key, std::size_t fallback,
                          std::size_t minimum, std::size_t maximum) {
    if (!object.contains(key)) {
        return fallback;
    }
    if (!object.at(key).is_number_integer()) {
        throw std::invalid_argument(
            message(Message::validation_nonnegative_integer,
                    {arg(Placeholder::context, "policy"), arg(Placeholder::field, key)}));
    }
    if (!object.at(key).is_number_unsigned() && object.at(key).get<long long>() < 0) {
        throw std::invalid_argument(
            message(Message::validation_nonnegative_integer,
                    {arg(Placeholder::context, "policy"), arg(Placeholder::field, key)}));
    }
    const auto value = object.at(key).get<std::size_t>();
    if (value < minimum || value > maximum) {
        throw std::invalid_argument(
            message(Message::validation_out_of_range,
                    {arg(Placeholder::context, "policy"), arg(Placeholder::field, key)}));
    }
    return value;
}

long optional_long(const Json& object, const char* key, long fallback, long minimum, long maximum) {
    if (!object.contains(key)) {
        return fallback;
    }
    if (!object.at(key).is_number_integer()) {
        throw std::invalid_argument(
            message(Message::validation_integer,
                    {arg(Placeholder::context, "policy"), arg(Placeholder::field, key)}));
    }
    const auto value = object.at(key).get<long>();
    if (value < minimum || value > maximum) {
        throw std::invalid_argument(
            message(Message::validation_out_of_range,
                    {arg(Placeholder::context, "policy"), arg(Placeholder::field, key)}));
    }
    return value;
}

std::string required_string(const Json& object, const char* key, std::string_view context) {
    if (!object.contains(key) || !object.at(key).is_string()) {
        throw std::invalid_argument(
            message(Message::validation_string,
                    {arg(Placeholder::context, context), arg(Placeholder::field, key)}));
    }
    auto value = object.at(key).get<std::string>();
    if (value.empty() || value.find('\0') != std::string::npos) {
        throw std::invalid_argument(
            message(Message::validation_nonempty_string,
                    {arg(Placeholder::context, context), arg(Placeholder::field, key)}));
    }
    return value;
}

bool valid_recipe_name(std::string_view name) {
    return name.size() <= 64 && std::all_of(name.begin(), name.end(), [](unsigned char value) {
               return std::isalnum(value) != 0 || value == '_' || value == '-' || value == '.';
           });
}

std::string fingerprint(std::string_view canonical) {
    // EN: A stable change detector for checkpoint capability matching. It is not a cryptographic
    //     signature and is never used to establish trust.
    // ZH-CN: 这是用于 checkpoint 能力匹配的稳定变更检测值，不是密码学签名，也不会用于
    //        建立信任。
    std::uint64_t value = 14695981039346656037ULL;
    for (const unsigned char byte : canonical) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

CommandRecipe parse_recipe(const Json& value, std::size_t index) {
    const auto context = "policy recipes[" + std::to_string(index) + "]";
    if (!value.is_object()) {
        throw std::invalid_argument(
            message(Message::validation_object, {arg(Placeholder::context, context)}));
    }
    reject_unknown_fields(
        value, {"name", "description", "program", "args", "cwd", "timeout_seconds", "verification"},
        context);

    CommandRecipe recipe;
    recipe.name = required_string(value, "name", context);
    if (!valid_recipe_name(recipe.name)) {
        throw std::invalid_argument(
            message(Message::policy_recipe_invalid_name, {arg(Placeholder::context, context)}));
    }
    recipe.program = required_string(value, "program", context);
    if (value.contains("description")) {
        if (!value.at("description").is_string()) {
            throw std::invalid_argument(
                message(Message::validation_string, {arg(Placeholder::context, context),
                                                     arg(Placeholder::field, "description")}));
        }
        recipe.description = value.at("description").get<std::string>();
        if (recipe.description.size() > 240 || recipe.description.find('\0') != std::string::npos) {
            throw std::invalid_argument(message(Message::policy_recipe_invalid_description,
                                                {arg(Placeholder::context, context)}));
        }
    }
    if (value.contains("args")) {
        if (!value.at("args").is_array() ||
            value.at("args").size() > runtime_bounds::max_command_arguments) {
            throw std::invalid_argument(message(Message::policy_recipe_too_many_arguments,
                                                {arg(Placeholder::context, context)}));
        }
        std::size_t total_bytes = 0;
        for (const auto& argument : value.at("args")) {
            if (!argument.is_string()) {
                throw std::invalid_argument(message(Message::policy_recipe_arguments_strings,
                                                    {arg(Placeholder::context, context)}));
            }
            auto text = argument.get<std::string>();
            total_bytes += text.size();
            if (text.find('\0') != std::string::npos ||
                total_bytes > runtime_bounds::max_command_argument_bytes) {
                throw std::invalid_argument(message(Message::policy_recipe_arguments_invalid,
                                                    {arg(Placeholder::context, context)}));
            }
            recipe.args.push_back(std::move(text));
        }
    }
    if (value.contains("cwd")) {
        const auto cwd = required_string(value, "cwd", context);
        recipe.cwd = cwd;
        if (recipe.cwd.is_absolute()) {
            throw std::invalid_argument(
                message(Message::policy_recipe_relative_cwd, {arg(Placeholder::context, context)}));
        }
    }
    recipe.timeout_seconds =
        optional_long(value, "timeout_seconds", runtime_defaults::command_timeout_seconds, 1,
                      runtime_bounds::max_recipe_timeout_seconds);
    if (value.contains("verification")) {
        if (!value.at("verification").is_boolean()) {
            throw std::invalid_argument(
                message(Message::validation_boolean, {arg(Placeholder::context, context),
                                                      arg(Placeholder::field, "verification")}));
        }
        recipe.verification = value.at("verification").get<bool>();
    }
    return recipe;
}

} // namespace

TaskPolicy load_task_policy(const std::filesystem::path& path) {
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(path, error);
    if (error || !std::filesystem::is_regular_file(resolved, error) || error) {
        throw std::invalid_argument(
            message(Message::policy_read_failed, {arg(Placeholder::path, path.generic_string())}));
    }
    const auto bytes = std::filesystem::file_size(resolved, error);
    if (error || bytes > max_policy_bytes) {
        throw std::invalid_argument(message(Message::policy_too_large));
    }

    std::ifstream input(resolved, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if ((!input.eof() && input.fail()) || text.find('\0') != std::string::npos) {
        throw std::invalid_argument(message(Message::policy_invalid_contents));
    }

    Json document;
    try {
        document = Json::parse(text);
    } catch (const Json::exception& parse_error) {
        throw std::invalid_argument(
            message(Message::policy_invalid_json, {arg(Placeholder::error, parse_error.what())}));
    }
    return parse_task_policy(document, resolved);
}

TaskPolicy parse_task_policy(const Json& document, std::filesystem::path source_path) {
    if (!document.is_object()) {
        throw std::invalid_argument(message(Message::policy_root_object));
    }
    reject_unknown_fields(document,
                          {"schema_version", "write_paths", "command_read_paths", "recipes",
                           "require_verification", "max_turns", "max_context_bytes", "max_seconds",
                           "max_total_tokens", "tool_limits"},
                          "task policy");
    if (!document.contains("schema_version") ||
        !document.at("schema_version").is_number_integer() ||
        document.at("schema_version") != policy_schema_version) {
        throw std::invalid_argument(message(Message::policy_unsupported_schema));
    }

    TaskPolicy policy;
    policy.source_path = std::move(source_path);
    policy.max_turns = optional_size(document, "max_turns", policy.max_turns,
                                     runtime_bounds::min_turns, runtime_bounds::max_turns);
    policy.max_context_bytes =
        optional_size(document, "max_context_bytes", policy.max_context_bytes,
                      runtime_bounds::min_context_bytes, runtime_bounds::max_context_bytes);
    policy.max_total_tokens = optional_size(document, "max_total_tokens", policy.max_total_tokens,
                                            0, runtime_bounds::max_total_tokens);
    policy.max_seconds =
        optional_long(document, "max_seconds", policy.max_seconds, 0, runtime_bounds::max_seconds);
    if (document.contains("tool_limits")) {
        policy.tool_limits =
            parse_tool_runtime_settings(document.at("tool_limits"), "policy tool_limits");
    }
    if (document.contains("require_verification")) {
        if (!document.at("require_verification").is_boolean()) {
            throw std::invalid_argument(message(Message::validation_boolean,
                                                {arg(Placeholder::context, "policy"),
                                                 arg(Placeholder::field, "require_verification")}));
        }
        policy.require_verification = document.at("require_verification").get<bool>();
    }

    if (document.contains("write_paths")) {
        if (!document.at("write_paths").is_array() || document.at("write_paths").size() > 128) {
            throw std::invalid_argument(message(Message::policy_write_paths_array));
        }
        std::set<std::string> unique_paths;
        for (const auto& item : document.at("write_paths")) {
            if (!item.is_string()) {
                throw std::invalid_argument(message(Message::policy_write_paths_strings));
            }
            const auto path_text = item.get<std::string>();
            const std::filesystem::path write_path(path_text);
            if (path_text.empty() || path_text.find('\0') != std::string::npos ||
                write_path == "." || write_path.is_absolute()) {
                throw std::invalid_argument(message(Message::policy_write_paths_relative));
            }
            const auto normalized = write_path.lexically_normal().generic_string();
            if (normalized == ".." || normalized.starts_with("../")) {
                throw std::invalid_argument(message(Message::policy_write_paths_escape));
            }
            if (unique_paths.insert(normalized).second) {
                policy.write_paths.emplace_back(normalized);
            }
        }
    }

    if (document.contains("command_read_paths")) {
        if (!document.at("command_read_paths").is_array() ||
            document.at("command_read_paths").size() > max_command_read_path_count) {
            throw std::invalid_argument(message(Message::policy_command_read_paths_array));
        }
        std::set<std::string> unique_paths;
        for (const auto& item : document.at("command_read_paths")) {
            if (!item.is_string()) {
                throw std::invalid_argument(message(Message::policy_command_read_paths_strings));
            }
            const auto path_text = item.get<std::string>();
            const std::filesystem::path read_path(path_text);
            if (path_text.empty() || path_text.find('\0') != std::string::npos ||
                !read_path.is_absolute()) {
                throw std::invalid_argument(message(Message::policy_command_read_paths_absolute));
            }
            const auto normalized = read_path.lexically_normal().generic_string();
            if (unique_paths.insert(normalized).second) {
                policy.command_read_paths.emplace_back(normalized);
            }
        }
    }

    if (document.contains("recipes")) {
        if (!document.at("recipes").is_array() ||
            document.at("recipes").size() > max_recipe_count) {
            throw std::invalid_argument(message(Message::policy_recipes_array));
        }
        std::set<std::string> recipe_names;
        for (std::size_t index = 0; index < document.at("recipes").size(); ++index) {
            auto recipe = parse_recipe(document.at("recipes").at(index), index);
            if (!recipe_names.insert(recipe.name).second) {
                throw std::invalid_argument(message(Message::policy_recipe_duplicate,
                                                    {arg(Placeholder::name, recipe.name)}));
            }
            policy.recipes.push_back(std::move(recipe));
        }
    }
    if (policy.require_verification) {
        if (policy.write_paths.empty()) {
            throw std::invalid_argument(message(Message::policy_verification_write_path));
        }
        const auto has_verification =
            std::any_of(policy.recipes.begin(), policy.recipes.end(),
                        [](const CommandRecipe& recipe) { return recipe.verification; });
        if (!has_verification) {
            throw std::invalid_argument(message(Message::policy_verification_recipe));
        }
    }

    Json normalized = {{"schema_version", policy_schema_version},
                       {"write_paths", Json::array()},
                       {"recipes", Json::array()},
                       {"require_verification", policy.require_verification},
                       {"max_turns", policy.max_turns},
                       {"max_context_bytes", policy.max_context_bytes},
                       {"max_seconds", policy.max_seconds}};
    // EN: Keep policies that omit the disabled budget fingerprint-compatible with checkpoints
    //     written before this optional field existed.
    // ZH-CN: 未启用预算时省略该字段，以保持 fingerprint 与该可选字段引入前的 checkpoint
    //        兼容。
    if (policy.max_total_tokens != 0) {
        normalized["max_total_tokens"] = policy.max_total_tokens;
    }
    if (policy.tool_limits != ToolRuntimeSettings{}) {
        normalized["tool_limits"] = tool_runtime_settings_to_json(policy.tool_limits);
    }
    if (!policy.command_read_paths.empty()) {
        normalized["command_read_paths"] = Json::array();
        for (const auto& read_path : policy.command_read_paths) {
            normalized["command_read_paths"].push_back(read_path.generic_string());
        }
    }
    for (const auto& write_path : policy.write_paths) {
        normalized["write_paths"].push_back(write_path.generic_string());
    }
    for (const auto& recipe : policy.recipes) {
        normalized["recipes"].push_back({{"name", recipe.name},
                                         {"description", recipe.description},
                                         {"program", recipe.program},
                                         {"args", recipe.args},
                                         {"cwd", recipe.cwd.generic_string()},
                                         {"timeout_seconds", recipe.timeout_seconds},
                                         {"verification", recipe.verification}});
    }
    policy.fingerprint = fingerprint(normalized.dump());
    return policy;
}

} // namespace mint
