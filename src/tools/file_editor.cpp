#include "mint/tools/tool_registry.hpp"

#include "mint/domain/change_journal.hpp"

#include "file_support.hpp"
#include "tool_support.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace mint {

using tools::detail::contains_ignored_component;
using tools::detail::contains_nul;
using tools::detail::display_path;
using tools::detail::error_result;
using tools::detail::is_valid_utf8;
using tools::detail::replace_file_safely;
using tools::detail::require_string;

Json ToolRegistry::apply_patch(const Json& arguments) const {
    const auto requested = require_string(arguments, "path");
    const auto operation = require_string(arguments, "operation");
    const auto new_text = require_string(arguments, "new_text");

    const std::filesystem::path input(requested);
    if (input.empty() || input == "." || input.is_absolute()) {
        throw std::invalid_argument("apply_patch 的 path 必须是工作目录内的相对文件路径");
    }
    if (new_text.size() > runtime_bounds::max_edit_file_bytes) {
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
    if (old_text.size() > runtime_bounds::max_edit_file_bytes) {
        throw std::invalid_argument("old_text 不能超过 256 KiB");
    }
    if (!exists || !std::filesystem::is_regular_file(target, error) || error) {
        return error_result("replace 的目标不存在或不是普通文件: " + requested);
    }

    const auto size = std::filesystem::file_size(target, error);
    if (error) {
        return error_result("无法获取目标文件大小: " + requested);
    }
    if (size > runtime_bounds::max_edit_file_bytes) {
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
    input_stream.close();
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
    if (updated.size() > runtime_bounds::max_edit_file_bytes) {
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

} // namespace mint
