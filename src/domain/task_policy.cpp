#include "mint/domain/task_policy.hpp"

#include "mint/domain/model.hpp"
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

constexpr std::size_t max_policy_bytes = 256 * 1024;
constexpr std::size_t max_recipe_count = 32;
constexpr std::size_t max_command_read_path_count = 64;

void reject_unknown_fields(const Json& object, const std::set<std::string>& allowed,
                           std::string_view context) {
    for (const auto& [key, value] : object.items()) {
        (void)value;
        if (!allowed.contains(key)) {
            throw std::invalid_argument(std::string(context) + " 包含未知字段: " + key);
        }
    }
}

std::size_t optional_size(const Json& object, const char* key, std::size_t fallback,
                          std::size_t minimum, std::size_t maximum) {
    if (!object.contains(key)) {
        return fallback;
    }
    if (!object.at(key).is_number_integer()) {
        throw std::invalid_argument(std::string("policy 字段 ") + key + " 必须是非负整数");
    }
    if (!object.at(key).is_number_unsigned() && object.at(key).get<long long>() < 0) {
        throw std::invalid_argument(std::string("policy 字段 ") + key + " 必须是非负整数");
    }
    const auto value = object.at(key).get<std::size_t>();
    if (value < minimum || value > maximum) {
        throw std::invalid_argument(std::string("policy 字段 ") + key + " 超出允许范围");
    }
    return value;
}

long optional_long(const Json& object, const char* key, long fallback, long minimum, long maximum) {
    if (!object.contains(key)) {
        return fallback;
    }
    if (!object.at(key).is_number_integer()) {
        throw std::invalid_argument(std::string("policy 字段 ") + key + " 必须是整数");
    }
    const auto value = object.at(key).get<long>();
    if (value < minimum || value > maximum) {
        throw std::invalid_argument(std::string("policy 字段 ") + key + " 超出允许范围");
    }
    return value;
}

std::string required_string(const Json& object, const char* key, std::string_view context) {
    if (!object.contains(key) || !object.at(key).is_string()) {
        throw std::invalid_argument(std::string(context) + " 的 " + key + " 必须是字符串");
    }
    auto value = object.at(key).get<std::string>();
    if (value.empty() || value.find('\0') != std::string::npos) {
        throw std::invalid_argument(std::string(context) + " 的 " + key + " 不能为空或包含 NUL");
    }
    return value;
}

bool valid_recipe_name(std::string_view name) {
    return name.size() <= 64 && std::all_of(name.begin(), name.end(), [](unsigned char value) {
               return std::isalnum(value) != 0 || value == '_' || value == '-' || value == '.';
           });
}

std::string fingerprint(std::string_view canonical) {
    // A stable change detector for checkpoint capability matching. It is not a
    // cryptographic signature and is never used to establish trust.
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
        throw std::invalid_argument(context + " 必须是对象");
    }
    reject_unknown_fields(
        value, {"name", "description", "program", "args", "cwd", "timeout_seconds", "verification"},
        context);

    CommandRecipe recipe;
    recipe.name = required_string(value, "name", context);
    if (!valid_recipe_name(recipe.name)) {
        throw std::invalid_argument(context + " 的 name 只允许字母、数字、点、下划线和连字符");
    }
    recipe.program = required_string(value, "program", context);
    if (value.contains("description")) {
        if (!value.at("description").is_string()) {
            throw std::invalid_argument(context + " 的 description 必须是字符串");
        }
        recipe.description = value.at("description").get<std::string>();
        if (recipe.description.size() > 240 || recipe.description.find('\0') != std::string::npos) {
            throw std::invalid_argument(context + " 的 description 不能超过 240 字节或包含 NUL");
        }
    }
    if (value.contains("args")) {
        if (!value.at("args").is_array() ||
            value.at("args").size() > runtime_bounds::max_command_arguments) {
            throw std::invalid_argument(context + " 的 args 项数超过允许上限");
        }
        std::size_t total_bytes = 0;
        for (const auto& argument : value.at("args")) {
            if (!argument.is_string()) {
                throw std::invalid_argument(context + " 的 args 必须全部是字符串");
            }
            auto text = argument.get<std::string>();
            total_bytes += text.size();
            if (text.find('\0') != std::string::npos ||
                total_bytes > runtime_bounds::max_command_argument_bytes) {
                throw std::invalid_argument(context + " 的 args 包含 NUL 或总长度超过允许上限");
            }
            recipe.args.push_back(std::move(text));
        }
    }
    if (value.contains("cwd")) {
        const auto cwd = required_string(value, "cwd", context);
        recipe.cwd = cwd;
        if (recipe.cwd.is_absolute()) {
            throw std::invalid_argument(context + " 的 cwd 必须是工作区内的相对路径");
        }
    }
    recipe.timeout_seconds =
        optional_long(value, "timeout_seconds", runtime_defaults::command_timeout_seconds, 1,
                      runtime_bounds::max_recipe_timeout_seconds);
    if (value.contains("verification")) {
        if (!value.at("verification").is_boolean()) {
            throw std::invalid_argument(context + " 的 verification 必须是布尔值");
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
        throw std::invalid_argument("无法读取 task policy: " + path.generic_string());
    }
    const auto bytes = std::filesystem::file_size(resolved, error);
    if (error || bytes > max_policy_bytes) {
        throw std::invalid_argument("task policy 不能超过 256 KiB");
    }

    std::ifstream input(resolved, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if ((!input.eof() && input.fail()) || text.find('\0') != std::string::npos) {
        throw std::invalid_argument("task policy 读取失败或包含 NUL");
    }

    Json document;
    try {
        document = Json::parse(text);
    } catch (const Json::exception& parse_error) {
        throw std::invalid_argument("task policy 不是有效 JSON: " +
                                    std::string(parse_error.what()));
    }
    return parse_task_policy(document, resolved);
}

TaskPolicy parse_task_policy(const Json& document, std::filesystem::path source_path) {
    if (!document.is_object()) {
        throw std::invalid_argument("task policy 顶层必须是 JSON 对象");
    }
    reject_unknown_fields(document,
                          {"schema_version", "write_paths", "command_read_paths", "recipes",
                           "require_verification", "max_turns", "max_context_bytes", "max_seconds",
                           "max_total_tokens", "tool_limits"},
                          "task policy");
    if (!document.contains("schema_version") ||
        !document.at("schema_version").is_number_integer() ||
        document.at("schema_version") != policy_schema_version) {
        throw std::invalid_argument("task policy schema_version 不受支持");
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
            throw std::invalid_argument("policy require_verification 必须是布尔值");
        }
        policy.require_verification = document.at("require_verification").get<bool>();
    }

    if (document.contains("write_paths")) {
        if (!document.at("write_paths").is_array() || document.at("write_paths").size() > 128) {
            throw std::invalid_argument("policy write_paths 必须是最多 128 项的字符串数组");
        }
        std::set<std::string> unique_paths;
        for (const auto& item : document.at("write_paths")) {
            if (!item.is_string()) {
                throw std::invalid_argument("policy write_paths 必须全部是字符串");
            }
            const auto path_text = item.get<std::string>();
            const std::filesystem::path write_path(path_text);
            if (path_text.empty() || path_text.find('\0') != std::string::npos ||
                write_path == "." || write_path.is_absolute()) {
                throw std::invalid_argument(
                    "policy write_paths 只接受工作区内的相对文件或目录，不接受 '.'");
            }
            const auto normalized = write_path.lexically_normal().generic_string();
            if (normalized == ".." || normalized.starts_with("../")) {
                throw std::invalid_argument("policy write_paths 不能逃逸工作区");
            }
            if (unique_paths.insert(normalized).second) {
                policy.write_paths.emplace_back(normalized);
            }
        }
    }

    if (document.contains("command_read_paths")) {
        if (!document.at("command_read_paths").is_array() ||
            document.at("command_read_paths").size() > max_command_read_path_count) {
            throw std::invalid_argument("policy command_read_paths 必须是最多 64 项的字符串数组");
        }
        std::set<std::string> unique_paths;
        for (const auto& item : document.at("command_read_paths")) {
            if (!item.is_string()) {
                throw std::invalid_argument("policy command_read_paths 必须全部是字符串");
            }
            const auto path_text = item.get<std::string>();
            const std::filesystem::path read_path(path_text);
            if (path_text.empty() || path_text.find('\0') != std::string::npos ||
                !read_path.is_absolute()) {
                throw std::invalid_argument("policy command_read_paths 只接受绝对路径");
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
            throw std::invalid_argument("policy recipes 必须是最多 32 项的数组");
        }
        std::set<std::string> recipe_names;
        for (std::size_t index = 0; index < document.at("recipes").size(); ++index) {
            auto recipe = parse_recipe(document.at("recipes").at(index), index);
            if (!recipe_names.insert(recipe.name).second) {
                throw std::invalid_argument("policy recipe 名称重复: " + recipe.name);
            }
            policy.recipes.push_back(std::move(recipe));
        }
    }
    if (policy.require_verification) {
        if (policy.write_paths.empty()) {
            throw std::invalid_argument("require_verification 需要至少一个 write_paths");
        }
        const auto has_verification =
            std::any_of(policy.recipes.begin(), policy.recipes.end(),
                        [](const CommandRecipe& recipe) { return recipe.verification; });
        if (!has_verification) {
            throw std::invalid_argument(
                "require_verification 需要至少一个 verification=true 的 recipe");
        }
    }

    Json normalized = {{"schema_version", policy_schema_version},
                       {"write_paths", Json::array()},
                       {"recipes", Json::array()},
                       {"require_verification", policy.require_verification},
                       {"max_turns", policy.max_turns},
                       {"max_context_bytes", policy.max_context_bytes},
                       {"max_seconds", policy.max_seconds}};
    // Keep policies that omit the disabled budget fingerprint-compatible with
    // checkpoints written before this optional field existed.
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
