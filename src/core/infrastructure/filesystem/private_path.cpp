#include "private_path.hpp"

#include "mint/localization/localization.hpp"

#include <cerrno>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
// EN: Windows SDK base types must be declared before aclapi.h.
// ZH-CN: 必须先声明 Windows SDK 基础类型，再包含 aclapi.h。
#include <windows.h>

#include <aclapi.h>

#include <io.h>

#include <vector>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mint::private_path {
namespace {

using localization::arg;
using localization::Message;
using localization::message;
using localization::Placeholder;

std::runtime_error permission_error(std::string message) {
    return std::runtime_error(std::move(message));
}

#if defined(_WIN32)

class WindowsHandle final {
  public:
    explicit WindowsHandle(HANDLE value) : value_(value) {}
    ~WindowsHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    WindowsHandle(const WindowsHandle&) = delete;
    WindowsHandle& operator=(const WindowsHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

  private:
    HANDLE value_ = nullptr;
};

std::vector<unsigned char> current_user_sid() {
    HANDLE raw_token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) == 0) {
        throw permission_error(message(Message::filesystem_private_windows_user_read_failed));
    }
    const WindowsHandle token(raw_token);

    DWORD bytes = 0;
    (void)GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (bytes == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        throw permission_error(message(Message::filesystem_private_windows_user_read_failed));
    }
    std::vector<unsigned char> token_data(bytes);
    if (GetTokenInformation(token.get(), TokenUser, token_data.data(), bytes, &bytes) == 0) {
        throw permission_error(message(Message::filesystem_private_windows_user_read_failed));
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(token_data.data());
    const auto sid_bytes = GetLengthSid(user->User.Sid);
    std::vector<unsigned char> sid(sid_bytes);
    if (CopySid(sid_bytes, sid.data(), user->User.Sid) == 0) {
        throw permission_error(message(Message::filesystem_private_windows_user_copy_failed));
    }
    return sid;
}

std::vector<unsigned char> local_system_sid() {
    DWORD bytes = SECURITY_MAX_SID_SIZE;
    std::vector<unsigned char> sid(bytes);
    if (CreateWellKnownSid(WinLocalSystemSid, nullptr, sid.data(), &bytes) == 0) {
        throw permission_error(message(Message::filesystem_private_windows_system_create_failed));
    }
    sid.resize(bytes);
    return sid;
}

PACL private_acl(bool directory, const std::vector<unsigned char>& user,
                 const std::vector<unsigned char>& system) {
    EXPLICIT_ACCESSW entries[2]{};
    const auto inheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    for (auto& entry : entries) {
        entry.grfAccessPermissions = FILE_ALL_ACCESS;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = inheritance;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
    }
    entries[0].Trustee.ptstrName =
        reinterpret_cast<LPWSTR>(const_cast<unsigned char*>(user.data()));
    entries[1].Trustee.ptstrName =
        reinterpret_cast<LPWSTR>(const_cast<unsigned char*>(system.data()));

    PACL acl = nullptr;
    const auto result = SetEntriesInAclW(2, entries, nullptr, &acl);
    if (result != ERROR_SUCCESS) {
        throw permission_error(message(Message::filesystem_private_windows_acl_create_failed));
    }
    return acl;
}

constexpr SECURITY_INFORMATION private_security_information =
    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;

void apply_private_acl(const std::filesystem::path& path, bool directory) {
    const auto user = current_user_sid();
    const auto system = local_system_sid();
    PACL acl = private_acl(directory, user, system);
    const auto result = SetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, private_security_information,
        const_cast<unsigned char*>(user.data()), nullptr, acl, nullptr);
    (void)LocalFree(acl);
    if (result != ERROR_SUCCESS) {
        throw permission_error(message(Message::filesystem_private_windows_acl_apply_failed));
    }
}

void apply_private_acl(HANDLE handle, bool directory) {
    const auto user = current_user_sid();
    const auto system = local_system_sid();
    PACL acl = private_acl(directory, user, system);
    const auto result =
        SetSecurityInfo(handle, SE_FILE_OBJECT, private_security_information,
                        const_cast<unsigned char*>(user.data()), nullptr, acl, nullptr);
    (void)LocalFree(acl);
    if (result != ERROR_SUCCESS) {
        throw permission_error(message(Message::filesystem_private_windows_acl_apply_failed));
    }
}

bool matches_private_acl(PSECURITY_DESCRIPTOR descriptor, PSID owner, PACL acl, bool directory,
                         const std::vector<unsigned char>& user,
                         const std::vector<unsigned char>& system) {
    if (descriptor == nullptr || owner == nullptr || acl == nullptr) {
        return false;
    }

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    ACL_SIZE_INFORMATION size{};
    bool private_acl = EqualSid(owner, const_cast<unsigned char*>(user.data())) != 0 &&
                       GetSecurityDescriptorControl(descriptor, &control, &revision) != 0 &&
                       (control & SE_DACL_PROTECTED) != 0 &&
                       GetAclInformation(acl, &size, sizeof(size), AclSizeInformation) != 0;
    bool user_allowed = false;
    bool system_allowed = false;
    for (DWORD index = 0; private_acl && index < size.AceCount; ++index) {
        void* raw_ace = nullptr;
        if (GetAce(acl, index, &raw_ace) == 0) {
            private_acl = false;
            break;
        }
        const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            private_acl = false;
            break;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
        auto* sid = const_cast<DWORD*>(&ace->SidStart);
        const auto required_flags = directory ? CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE : 0;
        if ((ace->Mask & FILE_ALL_ACCESS) != FILE_ALL_ACCESS ||
            (header->AceFlags & required_flags) != required_flags) {
            private_acl = false;
            break;
        }
        if (EqualSid(sid, const_cast<unsigned char*>(user.data())) != 0) {
            user_allowed = true;
        } else if (EqualSid(sid, const_cast<unsigned char*>(system.data())) != 0) {
            system_allowed = true;
        } else {
            private_acl = false;
        }
    }
    return private_acl && user_allowed && system_allowed;
}

bool has_private_acl(const std::filesystem::path& path, bool directory) {
    const auto user = current_user_sid();
    const auto system = local_system_sid();
    PACL acl = nullptr;
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto result =
        GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                              nullptr, &acl, nullptr, &descriptor);
    const bool matches = result == ERROR_SUCCESS &&
                         matches_private_acl(descriptor, owner, acl, directory, user, system);
    if (descriptor != nullptr) {
        (void)LocalFree(descriptor);
    }
    return matches;
}

bool owned_by_current_user(const std::filesystem::path& path) {
    const auto user = current_user_sid();
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto result = GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                                              OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr,
                                              nullptr, &descriptor);
    const bool owned = result == ERROR_SUCCESS && owner != nullptr &&
                       EqualSid(owner, const_cast<unsigned char*>(user.data())) != 0;
    if (descriptor != nullptr) {
        (void)LocalFree(descriptor);
    }
    return owned;
}

bool has_private_acl(HANDLE handle, bool directory) {
    const auto user = current_user_sid();
    const auto system = local_system_sid();
    PACL acl = nullptr;
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto result = GetSecurityInfo(handle, SE_FILE_OBJECT,
                                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                        &owner, nullptr, &acl, nullptr, &descriptor);
    const bool matches = result == ERROR_SUCCESS &&
                         matches_private_acl(descriptor, owner, acl, directory, user, system);
    if (descriptor != nullptr) {
        (void)LocalFree(descriptor);
    }
    return matches;
}

WindowsHandle security_handle(std::FILE* stream) {
    const int descriptor = ::_fileno(stream);
    if (descriptor < 0) {
        throw permission_error(message(Message::filesystem_private_windows_handle_read_failed));
    }
    const auto raw_handle = ::_get_osfhandle(descriptor);
    if (raw_handle == -1) {
        throw permission_error(message(Message::filesystem_private_windows_handle_read_failed));
    }
    const auto reopened =
        ReOpenFile(reinterpret_cast<HANDLE>(raw_handle), READ_CONTROL | WRITE_DAC | WRITE_OWNER,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (reopened == INVALID_HANDLE_VALUE) {
        throw permission_error(message(Message::filesystem_private_windows_reopen_failed));
    }
    return WindowsHandle(reopened);
}

#endif

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

void secure_directory(const std::filesystem::path& path, bool created, std::string_view description,
                      ExistingDirectoryPolicy existing_policy) {
#if defined(_WIN32)
    if (created) {
        apply_private_acl(path, true);
    } else if (!has_private_acl(path, true) &&
               existing_policy == ExistingDirectoryPolicy::migrate_owned) {
        if (!owned_by_current_user(path)) {
            throw permission_error(message(Message::filesystem_private_windows_wrong_owner,
                                           {arg(Placeholder::description, description)}));
        }
        apply_private_acl(path, true);
    }
    if (!has_private_acl(path, true)) {
        throw permission_error(message(Message::filesystem_private_not_private_directory,
                                       {arg(Placeholder::description, description)}));
    }
#else
    (void)existing_policy;
    if (created && ::chmod(path.c_str(), S_IRWXU) != 0) {
        throw permission_error(message(Message::filesystem_private_permission_failed,
                                       {arg(Placeholder::description, description)}));
    }
    struct stat status = {};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != ::geteuid() || (status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        throw permission_error(message(Message::filesystem_private_not_private_directory,
                                       {arg(Placeholder::description, description)}));
    }
#endif
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
    reject_symlink(path, description);
    std::error_code error;
    const auto created = std::filesystem::create_directories(path, error);
    if (error) {
        throw permission_error(message(Message::filesystem_private_create_failed,
                                       {arg(Placeholder::description, description)}));
    }
    verify_directory(path, description);
    secure_directory(path, created, description, existing_policy);
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
    secure_directory(path, true, description, ExistingDirectoryPolicy::require_private);
    return true;
}

void secure_open_file(const std::filesystem::path& path, std::FILE* stream) {
    if (stream == nullptr) {
        throw permission_error(message(Message::filesystem_private_file_permission_failed));
    }
#if defined(_WIN32)
    (void)path;
    const auto handle = security_handle(stream);
    apply_private_acl(handle.get(), false);
    if (!has_private_acl(handle.get(), false)) {
        throw permission_error(message(Message::filesystem_private_windows_file_verify_failed));
    }
#else
    (void)path;
    if (::fchmod(::fileno(stream), S_IRUSR | S_IWUSR) != 0) {
        throw permission_error(message(Message::filesystem_private_file_permission_failed));
    }
#endif
}

} // namespace mint::private_path
