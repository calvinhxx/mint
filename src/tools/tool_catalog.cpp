#include "tool_catalog.hpp"

#include "tool_contract.hpp"

#include <string>
#include <utility>

namespace mint::tools::detail {
namespace {

Json read_only_definitions(const ToolRuntimeSettings& runtime) {
    return Json::array(
        {{{"type", "function"},
          {"function",
           {{"name", "list_files"},
            {"description",
             "List files and directories below the allowed workspace root. Use relative paths."},
            {"parameters",
             {{"type", "object"},
              {"properties",
               {{"path",
                 {{"type", "string"},
                  {"description", "Relative directory path. Defaults to the workspace root."}}},
                {"max_depth",
                 {{"type", "integer"},
                  {"minimum", contract::min_list_depth},
                  {"maximum", contract::max_list_depth},
                  {"description", "How many directory levels to include. Defaults to " +
                                      std::to_string(contract::default_list_depth) + "."}}}}},
              {"additionalProperties", false}}}}}},
         {{"type", "function"},
          {"function",
           {{"name", "read_file"},
            {"description",
             "Read a text file inside the allowed workspace root. Large files are truncated."},
            {"parameters",
             {{"type", "object"},
              {"properties",
               {{"path",
                 {{"type", "string"}, {"description", "Relative path of the text file to read."}}},
                {"offset",
                 {{"type", "integer"},
                  {"minimum", 0},
                  {"description", "Byte offset to start from. Defaults to 0."}}},
                {"max_bytes",
                 {{"type", "integer"},
                  {"minimum", runtime_bounds::min_read_file_bytes},
                  {"maximum", runtime_bounds::max_read_file_bytes},
                  {"description", "Maximum bytes to return. Defaults to " +
                                      std::to_string(runtime.read_file_bytes) +
                                      ". Use next_offset for another chunk."}}}}},
              {"required", Json::array({"path"})},
              {"additionalProperties", false}}}}}},
         {{"type", "function"},
          {"function",
           {{"name", "search_text"},
            {"description",
             "Search text files inside the allowed workspace root and return matching lines."},
            {"parameters",
             {{"type", "object"},
              {"properties",
               {{"query",
                 {{"type", "string"},
                  {"description", "Literal text to search for, not a regular expression. At most " +
                                      std::to_string(contract::max_search_query_bytes) +
                                      " UTF-8 bytes."}}},
                {"path",
                 {{"type", "string"},
                  {"description",
                   "Relative file or directory path. Defaults to the workspace root."}}},
                {"case_sensitive",
                 {{"type", "boolean"},
                  {"description",
                   "Whether ASCII letter matching is case-sensitive. Defaults to false."}}}}},
              {"required", Json::array({"query"})},
              {"additionalProperties", false}}}}}}});
}

void append_write_definitions(Json& definitions) {
    definitions.push_back(
        {{"type", "function"},
         {"function",
          {{"name", "apply_patch"},
           {"description",
            "Create one text file or replace one exact, unique text block in an existing file. "
            "Read existing files first. This tool cannot delete files or run commands."},
           {"parameters",
            {{"type", "object"},
             {"properties",
              {{"path",
                {{"type", "string"},
                 {"description", "Relative path inside the allowed workspace root."}}},
               {"operation",
                {{"type", "string"},
                 {"enum", Json::array({"replace", "create"})},
                 {"description", "Use replace for an existing file or create for a new file."}}},
               {"old_text",
                {{"type", "string"},
                 {"description", "For replace: the non-empty text block that must occur exactly "
                                 "once."}}},
               {"new_text",
                {{"type", "string"},
                 {"description",
                  "Complete replacement block, or complete contents for a new file."}}}}},
             {"required", Json::array({"path", "operation", "new_text"})},
             {"additionalProperties", false}}}}}});
    definitions.push_back(
        {{"type", "function"},
         {"function",
          {{"name", "apply_changeset"},
           {"description",
            "Apply 1-" + std::to_string(contract::max_changes) +
                " validated text-file changes as one rollback transaction. Supports create, "
                "exact replace, delete, and move. Delete/move require old_text to equal the "
                "complete current file. Use only these exact fields per item: "
                "create(operation,path,new_text), replace(operation,path,old_text,new_text), "
                "delete(operation,path,old_text), or "
                "move(operation,path,old_text,destination). All paths are validated before "
                "approval and commit."},
           {"parameters",
            {{"type", "object"},
             {"properties",
              {{"changes",
                {{"type", "array"},
                 {"minItems", 1},
                 {"maxItems", contract::max_changes},
                 {"items",
                  {{"type", "object"},
                   {"properties",
                    {{"operation",
                      {{"type", "string"},
                       {"enum", Json::array({"create", "replace", "delete", "move"})}}},
                     {"path", {{"type", "string"}}},
                     {"old_text", {{"type", "string"}}},
                     {"new_text", {{"type", "string"}}},
                     {"destination", {{"type", "string"}}}}},
                   {"required", Json::array({"operation", "path"})},
                   {"additionalProperties", false}}}}}}},
             {"required", Json::array({"changes"})},
             {"additionalProperties", false}}}}}});
    definitions.push_back(
        {{"type", "function"},
         {"function",
          {{"name", "workspace_changes"},
           {"description", "Return every file changed by file-edit tools in this session and a "
                           "bounded unified diff from the original contents to the current "
                           "contents."},
           {"parameters",
            {{"type", "object"},
             {"properties", Json::object()},
             {"additionalProperties", false}}}}}});
}

std::string command_summary(const ToolCall& call) {
    Json summary = Json::object();
    if (call.name == "run_recipe") {
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
    if (call.name == "run_recipe" || call.name == "run_command") {
        return command_summary(call);
    }
    if (call.name == "apply_changeset") {
        return changeset_summary(call.arguments);
    }
    if (call.name == "apply_patch") {
        return patch_summary(call.arguments);
    }
    return call.arguments.dump();
}

} // namespace mint::tools::detail
