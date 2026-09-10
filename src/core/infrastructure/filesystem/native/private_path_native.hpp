#pragma once

#include "../private_path.hpp"

#include <exception>
#include <system_error>

namespace mint::private_path::native {

enum class AccessFailure {
    user_read,
    user_copy,
    system_sid,
    acl_create,
    acl_apply,
    file_handle,
    file_reopen,
    file_verify,
    directory_owner,
    directory_permissions,
    directory_verify,
    file_permissions,
};

class AccessError final : public std::exception {
  public:
    explicit AccessError(AccessFailure failure_code) noexcept : failure(failure_code) {}
    [[nodiscard]] const char* what() const noexcept override {
        return "Private path access failed";
    }
    AccessFailure failure;
};

[[nodiscard]] bool create_private_directory(const std::filesystem::path& path,
                                            std::error_code& error);
void secure_directory(const std::filesystem::path& path, bool created,
                      ExistingDirectoryPolicy existing_policy);
void secure_open_file(std::FILE* stream);
void verify_open_file(std::FILE* stream);

} // namespace mint::private_path::native
