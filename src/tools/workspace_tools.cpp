#include "aiagent/tools/tool_registry.hpp"

#include "aiagent/domain/change_journal.hpp"

#include "file_support.hpp"
#include "tool_support.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace aiagent {
namespace {

constexpr std::uintmax_t default_read_bytes = 16 * 1024;
constexpr std::uintmax_t max_search_file_bytes = 1024 * 1024;
constexpr std::size_t max_list_entries = 200;
constexpr std::size_t max_search_hits = 100;
constexpr std::size_t max_search_files = 2000;

} // namespace

using tools::detail::contains_ignored_component;
using tools::detail::contains_nul;
using tools::detail::display_path;
using tools::detail::error_result;
using tools::detail::is_ignored_directory;
using tools::detail::is_inside;
using tools::detail::is_valid_utf8;
using tools::detail::lowercase_ascii;
using tools::detail::max_edit_file_bytes;
using tools::detail::max_read_bytes;
using tools::detail::replace_file_safely;
using tools::detail::require_string;
using tools::detail::shorten_line;

std::filesystem::path ToolRegistry::resolve_inside_root(const std::string& requested) const {
    const std::filesystem::path input = requested.empty() ? "." : requested;
    const auto candidate = input.is_absolute() ? input : root_ / input;

    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(candidate, error);
    if (error) {
        throw std::runtime_error("无法解析路径: " + requested);
    }
    if (!is_inside(root_, resolved)) {
        throw std::runtime_error("拒绝访问工作目录之外的路径: " + requested);
    }
    return resolved;
}

Json ToolRegistry::list_files(const Json& arguments) const {
    std::string requested = ".";
    if (arguments.contains("path")) {
        requested = require_string(arguments, "path");
    }

    int max_depth = 2;
    if (arguments.contains("max_depth")) {
        if (!arguments.at("max_depth").is_number_integer()) {
            throw std::invalid_argument("参数 max_depth 必须是整数");
        }
        max_depth = arguments.at("max_depth").get<int>();
    }
    if (max_depth < 1 || max_depth > 4) {
        throw std::invalid_argument("max_depth 必须在 1 到 4 之间");
    }

    const auto target = resolve_inside_root(requested);
    if (is_protected(target)) {
        return error_result("拒绝访问受保护的配置文件: " + requested);
    }
    if (contains_ignored_component(root_, target)) {
        return error_result("拒绝访问忽略目录中的路径: " + requested);
    }
    std::error_code error;
    if (!std::filesystem::exists(target, error)) {
        return error_result("路径不存在: " + requested);
    }

    Json entries = Json::array();
    if (std::filesystem::is_regular_file(target, error)) {
        entries.push_back({{"path", display_path(root_, target)},
                           {"type", "file"},
                           {"size", std::filesystem::file_size(target, error)}});
        return {{"ok", true}, {"entries", entries}, {"truncated", false}};
    }
    if (!std::filesystem::is_directory(target, error)) {
        return error_result("目标不是普通文件或目录: " + requested);
    }

    bool truncated = false;
    std::filesystem::recursive_directory_iterator iterator(
        target, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;

    while (iterator != end) {
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }

        const auto path = iterator->path();
        if (is_protected(path)) {
            iterator.increment(error);
            continue;
        }
        const auto status = iterator->symlink_status(error);
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }

        const bool is_directory = std::filesystem::is_directory(status);
        if (is_directory && is_ignored_directory(path)) {
            iterator.disable_recursion_pending();
            iterator.increment(error);
            continue;
        }
        if (is_directory && iterator.depth() + 1 >= max_depth) {
            iterator.disable_recursion_pending();
        }

        std::string type = "other";
        if (std::filesystem::is_symlink(status)) {
            type = "symlink";
            iterator.disable_recursion_pending();
        } else if (is_directory) {
            type = "directory";
        } else if (std::filesystem::is_regular_file(status)) {
            type = "file";
        }

        Json entry = {{"path", display_path(root_, path)}, {"type", type}};
        if (type == "file") {
            const auto size = std::filesystem::file_size(path, error);
            if (!error) {
                entry["size"] = size;
            }
            error.clear();
        }
        entries.push_back(std::move(entry));

        if (entries.size() >= max_list_entries) {
            truncated = true;
            break;
        }
        iterator.increment(error);
    }

    std::sort(entries.begin(), entries.end(), [](const Json& left, const Json& right) {
        return left.at("path").get<std::string>() < right.at("path").get<std::string>();
    });
    return {{"ok", true},
            {"root", display_path(root_, target)},
            {"entries", std::move(entries)},
            {"truncated", truncated}};
}

Json ToolRegistry::read_file(const Json& arguments) const {
    const auto requested = require_string(arguments, "path");
    const auto path = resolve_inside_root(requested);
    if (is_protected(path)) {
        return error_result("拒绝访问受保护的配置文件: " + requested);
    }
    if (contains_ignored_component(root_, path)) {
        return error_result("拒绝访问忽略目录中的路径: " + requested);
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return error_result("文件不存在或不是普通文件: " + requested);
    }

    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return error_result("无法获取文件大小: " + requested);
    }

    std::uintmax_t offset = 0;
    if (arguments.contains("offset")) {
        if (!arguments.at("offset").is_number_integer()) {
            throw std::invalid_argument("参数 offset 必须是非负整数");
        }
        const auto parsed = arguments.at("offset").get<long long>();
        if (parsed < 0) {
            throw std::invalid_argument("参数 offset 必须是非负整数");
        }
        offset = static_cast<std::uintmax_t>(parsed);
    }
    if (offset > size) {
        return error_result("offset 超过文件大小: " + requested);
    }

    std::uintmax_t requested_bytes = default_read_bytes;
    if (arguments.contains("max_bytes")) {
        if (!arguments.at("max_bytes").is_number_integer()) {
            throw std::invalid_argument("参数 max_bytes 必须是整数");
        }
        const auto parsed = arguments.at("max_bytes").get<long long>();
        if (parsed < 1024 || parsed > static_cast<long long>(max_read_bytes)) {
            throw std::invalid_argument("参数 max_bytes 必须在 1024 到 65536 之间");
        }
        requested_bytes = static_cast<std::uintmax_t>(parsed);
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return error_result("无法读取文件: " + requested);
    }
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        return error_result("无法定位文件读取位置: " + requested);
    }

    const auto bytes_to_read = static_cast<std::size_t>(std::min(size - offset, requested_bytes));
    std::string content(bytes_to_read, '\0');
    input.read(content.data(), static_cast<std::streamsize>(bytes_to_read));
    content.resize(static_cast<std::size_t>(input.gcount()));
    if (contains_nul(content)) {
        return error_result("拒绝读取疑似二进制文件: " + requested);
    }

    if (offset + content.size() < size && content.size() > 4096) {
        const auto last_newline = content.rfind('\n');
        if (last_newline != std::string::npos && last_newline + 1 >= content.size() / 2) {
            content.resize(last_newline + 1);
        }
    }
    const auto next_offset = offset + content.size();
    const auto truncated = next_offset < size;

    return {{"ok", true},
            {"path", display_path(root_, path)},
            {"size", size},
            {"offset", offset},
            {"next_offset", truncated ? Json(next_offset) : Json(nullptr)},
            {"truncated", truncated},
            {"content", std::move(content)}};
}

Json ToolRegistry::search_text(const Json& arguments) const {
    const auto query = require_string(arguments, "query");
    if (query.empty()) {
        throw std::invalid_argument("搜索内容不能为空");
    }
    if (query.size() > 256) {
        throw std::invalid_argument("搜索内容不能超过 256 字节");
    }

    std::string requested = ".";
    if (arguments.contains("path")) {
        requested = require_string(arguments, "path");
    }
    bool case_sensitive = false;
    if (arguments.contains("case_sensitive")) {
        if (!arguments.at("case_sensitive").is_boolean()) {
            throw std::invalid_argument("参数 case_sensitive 必须是布尔值");
        }
        case_sensitive = arguments.at("case_sensitive").get<bool>();
    }

    const auto target = resolve_inside_root(requested);
    if (is_protected(target)) {
        return error_result("拒绝访问受保护的配置文件: " + requested);
    }
    if (contains_ignored_component(root_, target)) {
        return error_result("拒绝访问忽略目录中的路径: " + requested);
    }
    std::error_code error;
    if (!std::filesystem::exists(target, error)) {
        return error_result("路径不存在: " + requested);
    }

    Json hits = Json::array();
    std::size_t scanned_files = 0;
    bool truncated = false;
    const auto comparable_query = case_sensitive ? query : lowercase_ascii(query);

    const auto scan_file = [&](const std::filesystem::path& path) {
        if (hits.size() >= max_search_hits || scanned_files >= max_search_files) {
            truncated = true;
            return;
        }
        if (is_protected(path)) {
            return;
        }

        std::error_code file_error;
        if (!std::filesystem::is_regular_file(path, file_error)) {
            return;
        }
        const auto size = std::filesystem::file_size(path, file_error);
        if (file_error || size > max_search_file_bytes) {
            return;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return;
        }
        std::string probe(4096, '\0');
        input.read(probe.data(), static_cast<std::streamsize>(probe.size()));
        probe.resize(static_cast<std::size_t>(input.gcount()));
        if (contains_nul(probe)) {
            return;
        }
        input.clear();
        input.seekg(0);
        ++scanned_files;

        std::string line;
        std::size_t line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            const auto comparable_line = case_sensitive ? line : lowercase_ascii(line);
            if (comparable_line.find(comparable_query) == std::string::npos) {
                continue;
            }
            hits.push_back({{"path", display_path(root_, path)},
                            {"line", line_number},
                            {"text", shorten_line(line)}});
            if (hits.size() >= max_search_hits) {
                truncated = true;
                return;
            }
        }
    };

    if (std::filesystem::is_regular_file(target, error)) {
        scan_file(target);
    } else if (std::filesystem::is_directory(target, error)) {
        std::filesystem::recursive_directory_iterator iterator(
            target, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        while (iterator != end && !truncated) {
            if (error) {
                error.clear();
                iterator.increment(error);
                continue;
            }

            const auto status = iterator->symlink_status(error);
            if (error) {
                error.clear();
                iterator.increment(error);
                continue;
            }
            if (std::filesystem::is_symlink(status)) {
                iterator.disable_recursion_pending();
            } else if (std::filesystem::is_directory(status)) {
                if (is_ignored_directory(iterator->path())) {
                    iterator.disable_recursion_pending();
                }
            } else if (std::filesystem::is_regular_file(status)) {
                scan_file(iterator->path());
            }
            iterator.increment(error);
        }
    } else {
        return error_result("目标不是普通文件或目录: " + requested);
    }

    return {{"ok", true},
            {"query", query},
            {"scanned_files", scanned_files},
            {"hits", std::move(hits)},
            {"truncated", truncated}};
}

Json ToolRegistry::apply_patch(const Json& arguments) const {
    const auto requested = require_string(arguments, "path");
    const auto operation = require_string(arguments, "operation");
    const auto new_text = require_string(arguments, "new_text");

    const std::filesystem::path input(requested);
    if (input.empty() || input == "." || input.is_absolute()) {
        throw std::invalid_argument("apply_patch 的 path 必须是工作目录内的相对文件路径");
    }
    if (new_text.size() > max_edit_file_bytes) {
        throw std::invalid_argument("new_text 不能超过 256 KiB");
    }
    if (contains_nul(new_text) || !is_valid_utf8(new_text)) {
        throw std::invalid_argument("apply_patch 只支持有效 UTF-8 文本内容");
    }

    const auto unresolved = root_ / input;
    std::error_code status_error;
    const auto unresolved_status = std::filesystem::symlink_status(unresolved, status_error);
    if (!status_error && std::filesystem::is_symlink(unresolved_status)) {
        throw std::runtime_error("apply_patch 拒绝直接修改符号链接: " + requested);
    }

    const auto target = resolve_inside_root(requested);
    if (!is_write_allowed(target)) {
        return error_result("写入路径未获用户 --allow-write-path 授权: " + requested);
    }
    if (is_protected(target)) {
        return error_result("拒绝修改受保护的配置文件: " + requested);
    }
    if (contains_ignored_component(root_, target)) {
        return error_result("拒绝修改忽略目录中的路径: " + requested);
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(target, error);
    if (error) {
        throw std::runtime_error("无法检查目标文件: " + requested);
    }

    if (operation == "create") {
        if (arguments.contains("old_text")) {
            if (!arguments.at("old_text").is_string()) {
                throw std::invalid_argument("参数 old_text 必须是字符串");
            }
            if (!arguments.at("old_text").get_ref<const std::string&>().empty()) {
                throw std::invalid_argument("create 操作不能提供非空 old_text");
            }
        }
        if (exists) {
            return error_result("create 拒绝覆盖已经存在的路径: " + requested);
        }
        if (!std::filesystem::is_directory(target.parent_path(), error) || error) {
            return error_result("create 的父目录不存在或不是目录: " + requested);
        }

        replace_file_safely(target, new_text, false);
        const auto relative_path = display_path(root_, target);
        change_journal_->record_created(relative_path, new_text);
        return {{"ok", true},
                {"operation", "create"},
                {"path", relative_path},
                {"bytes_before", 0},
                {"bytes_after", new_text.size()}};
    }

    if (operation != "replace") {
        throw std::invalid_argument("operation 只支持 replace 或 create");
    }

    const auto old_text = require_string(arguments, "old_text");
    if (old_text.empty()) {
        throw std::invalid_argument("replace 操作的 old_text 不能为空");
    }
    if (old_text.size() > max_edit_file_bytes) {
        throw std::invalid_argument("old_text 不能超过 256 KiB");
    }
    if (!exists || !std::filesystem::is_regular_file(target, error) || error) {
        return error_result("replace 的目标不存在或不是普通文件: " + requested);
    }

    const auto size = std::filesystem::file_size(target, error);
    if (error) {
        return error_result("无法获取目标文件大小: " + requested);
    }
    if (size > max_edit_file_bytes) {
        return error_result("replace 暂不修改超过 256 KiB 的文件: " + requested);
    }

    std::ifstream input_stream(target, std::ios::binary);
    if (!input_stream) {
        return error_result("无法读取待修改文件: " + requested);
    }
    std::string original((std::istreambuf_iterator<char>(input_stream)),
                         std::istreambuf_iterator<char>());
    if (!input_stream.eof() && input_stream.fail()) {
        return error_result("读取待修改文件失败: " + requested);
    }
    if (contains_nul(original) || !is_valid_utf8(original)) {
        return error_result("apply_patch 拒绝修改二进制或非 UTF-8 文件: " + requested);
    }

    const auto match = original.find(old_text);
    if (match == std::string::npos) {
        return error_result("old_text 在目标文件中不存在；请重新读取文件后再修改");
    }
    if (original.find(old_text, match + 1) != std::string::npos) {
        return error_result("old_text 在目标文件中出现多次；请提供更精确的上下文");
    }

    std::string updated = original;
    updated.replace(match, old_text.size(), new_text);
    if (updated == original) {
        return error_result("修改前后内容相同，没有需要写入的变化");
    }
    if (updated.size() > max_edit_file_bytes) {
        return error_result("修改后的文件不能超过 256 KiB");
    }

    replace_file_safely(target, updated, true);
    const auto relative_path = display_path(root_, target);
    change_journal_->record_modified(relative_path, original, updated);
    return {{"ok", true},
            {"operation", "replace"},
            {"path", relative_path},
            {"bytes_before", original.size()},
            {"bytes_after", updated.size()}};
}

} // namespace aiagent
