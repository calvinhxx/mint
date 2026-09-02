#include "tool_catalog.hpp"

#include "tool_contract.hpp"
#include "tool_names.hpp"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace mint::tools::detail {
namespace {

Json string_array(std::initializer_list<std::string_view> values) {
    auto result = Json::array();
    for (const auto value : values) {
        result.push_back(value);
    }
    return result;
}

Json property_schema(std::string_view type, std::string description = {}) {
    auto schema = Json::object();
    schema["type"] = type;
    if (!description.empty()) {
        schema["description"] = std::move(description);
    }
    return schema;
}

Json object_schema() {
    auto schema = property_schema("object");
    schema["properties"] = Json::object();
    schema["additionalProperties"] = false;
    return schema;
}

Json tool_definition(std::string_view name, std::string description, Json parameters) {
    auto function = Json::object();
    function["name"] = name;
    function["description"] = std::move(description);
    function["parameters"] = std::move(parameters);

    auto definition = Json::object();
    definition["type"] = "function";
    definition["function"] = std::move(function);
    return definition;
}

Json list_files_definition() {
    auto parameters = object_schema();
    parameters["properties"]["path"] =
        property_schema("string", "Relative directory path. Defaults to the workspace root.");

    auto max_depth =
        property_schema("integer", "How many directory levels to include. Defaults to " +
                                       std::to_string(contract::default_list_depth) + ".");
    max_depth["minimum"] = contract::min_list_depth;
    max_depth["maximum"] = contract::max_list_depth;
    parameters["properties"]["max_depth"] = std::move(max_depth);

    return tool_definition(
        name::list_files,
        "List files and directories below the allowed workspace root. Use relative paths.",
        std::move(parameters));
}

Json read_file_definition(const ToolRuntimeSettings& runtime) {
    auto parameters = object_schema();
    parameters["properties"]["path"] =
        property_schema("string", "Relative path of the text file to read.");

    auto offset = property_schema("integer", "Byte offset to start from. Defaults to 0.");
    offset["minimum"] = 0;
    parameters["properties"]["offset"] = std::move(offset);

    auto max_bytes = property_schema("integer", "Maximum bytes to return. Defaults to " +
                                                    std::to_string(runtime.read_file_bytes) +
                                                    ". Use next_offset for another chunk.");
    max_bytes["minimum"] = runtime_bounds::min_read_file_bytes;
    max_bytes["maximum"] = runtime_bounds::max_read_file_bytes;
    parameters["properties"]["max_bytes"] = std::move(max_bytes);
    parameters["required"] = string_array({"path"});

    return tool_definition(
        name::read_file,
        "Read a text file inside the allowed workspace root. Large files are truncated.",
        std::move(parameters));
}

Json search_text_definition() {
    auto parameters = object_schema();
    parameters["properties"]["query"] = property_schema(
        "string", "Literal text to search for, not a regular expression. At most " +
                      std::to_string(contract::max_search_query_bytes) + " UTF-8 bytes.");
    parameters["properties"]["path"] = property_schema(
        "string", "Relative file or directory path. Defaults to the workspace root.");
    parameters["properties"]["case_sensitive"] = property_schema(
        "boolean", "Whether ASCII letter matching is case-sensitive. Defaults to false.");
    parameters["required"] = string_array({"query"});

    return tool_definition(
        name::search_text,
        "Search text files inside the allowed workspace root and return matching lines.",
        std::move(parameters));
}

Json read_only_definitions(const ToolRuntimeSettings& runtime) {
    auto definitions = Json::array();
    definitions.push_back(list_files_definition());
    definitions.push_back(read_file_definition(runtime));
    definitions.push_back(search_text_definition());
    return definitions;
}

Json apply_patch_definition() {
    auto parameters = object_schema();
    parameters["properties"]["path"] =
        property_schema("string", "Relative path inside the allowed workspace root.");

    auto operation =
        property_schema("string", "Use replace for an existing file or create for a new file.");
    operation["enum"] = string_array({"replace", "create"});
    parameters["properties"]["operation"] = std::move(operation);
    parameters["properties"]["old_text"] = property_schema(
        "string", "For replace: the non-empty text block that must occur exactly once.");
    parameters["properties"]["new_text"] = property_schema(
        "string", "Complete replacement block, or complete contents for a new file.");
    parameters["required"] = string_array({"path", "operation", "new_text"});

    return tool_definition(
        name::apply_patch,
        "Create one text file or replace one exact, unique text block in an existing file. "
        "Read existing files first. This tool cannot delete files or run commands.",
        std::move(parameters));
}

Json apply_changeset_definition() {
    auto item = object_schema();
    auto operation = property_schema("string");
    operation["enum"] = string_array({"create", "replace", "delete", "move"});
    item["properties"]["operation"] = std::move(operation);
    for (const auto field : {"path", "old_text", "new_text", "destination"}) {
        item["properties"][field] = property_schema("string");
    }
    item["required"] = string_array({"operation", "path"});

    auto changes = property_schema("array");
    changes["minItems"] = 1;
    changes["maxItems"] = contract::max_changes;
    changes["items"] = std::move(item);

    auto parameters = object_schema();
    parameters["properties"]["changes"] = std::move(changes);
    parameters["required"] = string_array({"changes"});

    return tool_definition(
        name::apply_changeset,
        "Apply 1-" + std::to_string(contract::max_changes) +
            " validated text-file changes as one rollback transaction. Supports create, "
            "exact replace, delete, and move. Delete/move require old_text to equal the "
            "complete current file. Use only these exact fields per item: "
            "create(operation,path,new_text), replace(operation,path,old_text,new_text), "
            "delete(operation,path,old_text), or "
            "move(operation,path,old_text,destination). All paths are validated before "
            "approval and commit.",
        std::move(parameters));
}

Json workspace_changes_definition() {
    return tool_definition(
        name::workspace_changes,
        "Return every workspace file changed by file-edit or command tools in this session and "
        "a bounded unified diff when the change is auditable text. Non-text or policy-violating "
        "command changes are reported with an explicit status.",
        object_schema());
}

void append_write_definitions(Json& definitions) {
    definitions.push_back(apply_patch_definition());
    definitions.push_back(apply_changeset_definition());
    definitions.push_back(workspace_changes_definition());
}

std::string command_summary(const ToolCall& call) {
    Json summary = Json::object();
    if (call.name == name::run_recipe) {
        if (call.arguments.contains("recipe") && call.arguments.at("recipe").is_string()) {
            summary["recipe"] = call.arguments.at("recipe");
        }
        return summary.dump();
    }
    if (call.arguments.contains("program") && call.arguments.at("program").is_string()) {
        summary["program"] = call.arguments.at("program");
    }
    if (call.arguments.contains("cwd") && call.arguments.at("cwd").is_string()) {
        summary["cwd"] = call.arguments.at("cwd");
    }
    if (call.arguments.contains("timeout_seconds") &&
        call.arguments.at("timeout_seconds").is_number_integer()) {
        summary["timeout_seconds"] = call.arguments.at("timeout_seconds");
    }
    if (call.arguments.contains("args") && call.arguments.at("args").is_array()) {
        summary["arg_count"] = call.arguments.at("args").size();
    }
    return summary.dump();
}

std::string changeset_summary(const Json& arguments) {
    Json summary = Json::object();
    if (!arguments.contains("changes") || !arguments.at("changes").is_array()) {
        return summary.dump();
    }
    summary["operation_count"] = arguments.at("changes").size();
    Json paths = Json::array();
    for (const auto& item : arguments.at("changes")) {
        if (!item.is_object()) {
            continue;
        }
        Json change = Json::object();
        for (const auto* field : {"operation", "path", "destination"}) {
            if (item.contains(field) && item.at(field).is_string()) {
                change[field] = item.at(field);
            }
        }
        if (item.contains("old_text") && item.at("old_text").is_string()) {
            change["old_bytes"] = item.at("old_text").get_ref<const std::string&>().size();
        }
        if (item.contains("new_text") && item.at("new_text").is_string()) {
            change["new_bytes"] = item.at("new_text").get_ref<const std::string&>().size();
        }
        paths.push_back(std::move(change));
    }
    summary["changes"] = std::move(paths);
    return summary.dump();
}

std::string patch_summary(const Json& arguments) {
    Json summary = Json::object();
    for (const auto* field : {"path", "operation"}) {
        if (arguments.contains(field) && arguments.at(field).is_string()) {
            summary[field] = arguments.at(field);
        }
    }
    if (arguments.contains("old_text") && arguments.at("old_text").is_string()) {
        summary["old_bytes"] = arguments.at("old_text").get_ref<const std::string&>().size();
    }
    if (arguments.contains("new_text") && arguments.at("new_text").is_string()) {
        summary["new_bytes"] = arguments.at("new_text").get_ref<const std::string&>().size();
    }
    return summary.dump();
}

} // namespace

Json workspace_tool_definitions(bool allow_write, const ToolRuntimeSettings& runtime) {
    auto definitions = read_only_definitions(runtime);
    if (allow_write) {
        append_write_definitions(definitions);
    }
    return definitions;
}

std::string summarize_tool_call(const ToolCall& call) {
    if (!call.arguments.is_object()) {
        return call.arguments.dump();
    }
    if (call.name == name::run_recipe || call.name == name::run_command) {
        return command_summary(call);
    }
    if (call.name == name::apply_changeset) {
        return changeset_summary(call.arguments);
    }
    if (call.name == name::apply_patch) {
        return patch_summary(call.arguments);
    }
    return call.arguments.dump();
}

} // namespace mint::tools::detail
