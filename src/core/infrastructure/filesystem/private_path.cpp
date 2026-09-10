#include "private_path.hpp"

#include "native/private_path_native.hpp"

#include "mint/localization/localization.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mint::private_path {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

std::runtime_error permission_error(std::string message) {
    return std::runtime_error(std::move(message));
}

std::runtime_error permission_error(native::AccessFailure failure,
                                    std::string_view description = {}) {
    using native::AccessFailure;
    switch (failure) {
    case AccessFailure::user_read:
        return permission_error(message(Message::filesystem_private_windows_user_read_failed));
    case AccessFailure::user_copy:
        return permission_error(message(Message::filesystem_private_windows_user_copy_failed));
    case AccessFailure::system_sid:
        return permission_error(message(Message::filesystem_private_windows_system_create_failed));
    case AccessFailure::acl_create:
        return permission_error(message(Message::filesystem_private_windows_acl_create_failed));
    case AccessFailure::acl_apply:
        return permission_error(message(Message::filesystem_private_windows_acl_apply_failed));
    case AccessFailure::file_handle:
        return permission_error(message(Message::filesystem_private_windows_handle_read_failed));
    case AccessFailure::file_reopen:
        return permission_error(message(Message::filesystem_private_windows_reopen_failed));
    case AccessFailure::file_verify:
        return permission_error(message(Message::filesystem_private_windows_file_verify_failed));
    case AccessFailure::directory_owner:
        return permission_error(message(Message::filesystem_private_windows_wrong_owner,
                                        {arg(Placeholder::description, description)}));
    case AccessFailure::directory_permissions:
        return permission_error(message(Message::filesystem_private_permission_failed,
                                        {arg(Placeholder::description, description)}));
    case AccessFailure::directory_verify:
        return permission_error(message(Message::filesystem_private_not_private_directory,
                                        {arg(Placeholder::description, description)}));
    case AccessFailure::file_permissions:
        return permission_error(message(Message::filesystem_private_file_permission_failed));
    }
    return permission_error(message(Message::filesystem_private_file_permission_failed));
}

void reject_symlink(const std::filesystem::path& path, std::string_view description) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw permission_error(message(Message::filesystem_private_inspect_failed,
                                       {arg(Placeholder::description, description)}));
    }
    if (!error && std::filesystem::is_symlink(status)) {
        throw permission_error(message(Message::filesystem_private_symlink,
                                       {arg(Placeholder::description, description)}));
    }
}

void verify_directory(const std::filesystem::path& path, std::string_view description) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
        throw permission_error(message(Message::filesystem_private_secure_create_failed,
                                       {arg(Placeholder::description, description)}));
    }
}

} // namespace

void ensure_directory(const std::filesystem::path& path, std::string_view description,
                      ExistingDirectoryPolicy existing_policy) {
    if (path.empty() || description.empty()) {
        throw std::invalid_argument(message(Message::filesystem_private_directory_argument_empty));
    }
    // EN: A trailing separator gives parent_path() the same directory as the leaf.
    //     Strip only empty final components, without changing symlink / '..' semantics.
    // ZH-CN: 尾分隔符会使 parent_path() 指向叶目录本身；只去掉末尾空组件，
    //        不改变符号链接或 '..' 的路径语义。
    auto directory = path;
    while (!directory.has_filename() && directory.has_relative_path()) {
        directory = directory.parent_path();
    }
    reject_symlink(directory, description);
    std::error_code error;
    if (!directory.parent_path().empty()) {
        std::filesystem::create_directories(directory.parent_path(), error);
    }
    try {
        // EN: Create the directory with final permissions before publishing it.
        // ZH-CN: 创建时即设置最终权限，避免并发首次使用看到尚未收紧权限的目录。
        const auto created = !error && native::create_private_directory(directory, error);
        if (error) {
            throw permission_error(message(Message::filesystem_private_create_failed,
                                           {arg(Placeholder::description, description)}));
        }
        verify_directory(directory, description);
        native::secure_directory(directory, created, existing_policy);
    } catch (const native::AccessError& failure) {
        throw permission_error(failure.failure, description);
    }
}

bool create_directory(const std::filesystem::path& path, std::string_view description) {
    if (path.empty() || description.empty()) {
        throw std::invalid_argument(message(Message::filesystem_private_directory_argument_empty));
    }
    std::error_code error;
    const auto created = std::filesystem::create_directory(path, error);
    if (error) {
        throw permission_error(message(Message::filesystem_private_create_error,
                                       {arg(Placeholder::description, description),
                                        arg(Placeholder::error, error.message())}));
    }
    if (!created) {
        return false;
    }
    verify_directory(path, description);
    try {
        native::secure_directory(path, true, ExistingDirectoryPolicy::require_private);
    } catch (const native::AccessError& failure) {
        throw permission_error(failure.failure, description);
    }
    return true;
}

void secure_open_file(const std::filesystem::path& path, std::FILE* stream) {
    (void)path;
    if (stream == nullptr) {
        throw permission_error(message(Message::filesystem_private_file_permission_failed));
    }
    try {
        native::secure_open_file(stream);
    } catch (const native::AccessError& failure) {
        throw permission_error(failure.failure);
    }
}

void verify_open_file(std::FILE* stream) {
    if (stream == nullptr) {
        throw permission_error(message(Message::filesystem_private_file_permission_failed));
    }
    try {
        native::verify_open_file(stream);
    } catch (const native::AccessError& failure) {
        throw permission_error(failure.failure);
    }
}

} // namespace mint::private_path
