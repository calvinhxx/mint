#include "command_appcontainer_windows.hpp"

#include <aclapi.h>
#include <objbase.h>
#include <sddl.h>
#include <userenv.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace mint::command_detail {
namespace {

constexpr DWORD workspace_access =
    FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE | FILE_DELETE_CHILD;
constexpr DWORD readonly_access = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
constexpr DWORD denied_access = workspace_access;

[[noreturn]] void throw_windows_error(DWORD error, std::string_view context) {
    throw std::system_error(static_cast<int>(error), std::system_category(), std::string(context));
}

[[noreturn]] void throw_hresult(HRESULT result, std::string_view context) {
    throw std::system_error(static_cast<int>(result), std::system_category(), std::string(context));
}

bool path_component_equal(const std::filesystem::path& left, const std::filesystem::path& right) {
    const auto& left_native = left.native();
    const auto& right_native = right.native();
    return ::CompareStringOrdinal(left_native.c_str(), -1, right_native.c_str(), -1, TRUE) ==
           CSTR_EQUAL;
}

bool is_inside(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() ||
            !path_component_equal(*root_part, *candidate_part)) {
            return false;
        }
    }
    return true;
}

std::filesystem::path canonical_existing_path(const std::filesystem::path& path,
                                              std::string_view description) {
    std::error_code error;
    auto resolved = std::filesystem::weakly_canonical(path, error);
    if (error || resolved.empty() || !std::filesystem::exists(resolved, error) || error) {
        throw std::invalid_argument(std::string(description) + "不存在: " + path.string());
    }
    return resolved;
}

std::optional<std::filesystem::path> environment_path(const wchar_t* name) {
    DWORD size = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), size);
    if (written == 0 || written >= size) {
        return std::nullopt;
    }
    value.resize(written);
    std::filesystem::path path(std::move(value));
    std::error_code error;
    path = std::filesystem::weakly_canonical(std::move(path), error);
    if (error || path.empty() || !std::filesystem::exists(path, error) || error) {
        return std::nullopt;
    }
    return path;
}

std::vector<std::filesystem::path> system_managed_roots() {
    std::vector<std::filesystem::path> paths;
    for (const auto* name :
         {L"SYSTEMROOT", L"PROGRAMFILES", L"PROGRAMFILES(X86)", L"PROGRAMW6432", L"PROGRAMDATA"}) {
        if (const auto path = environment_path(name)) {
            paths.push_back(*path);
        }
    }
    return paths;
}

std::vector<std::filesystem::path>
executable_read_paths(const std::filesystem::path& workspace,
                      const std::vector<std::filesystem::path>& allowed_executables) {
    const auto system_roots = system_managed_roots();
    std::vector<std::filesystem::path> paths;
    for (const auto& executable : allowed_executables) {
        const bool system_managed =
            std::any_of(system_roots.begin(), system_roots.end(),
                        [&](const auto& root) { return is_inside(root, executable); });
        if (system_managed || is_inside(workspace, executable)) {
            continue;
        }
        paths.push_back(executable);
        const auto parent = executable.parent_path();
        if (parent.empty() || is_inside(parent, workspace)) {
            continue;
        }
        paths.push_back(parent);
        std::error_code error;
        for (std::filesystem::directory_iterator entry(parent, error), end; !error && entry != end;
             entry.increment(error)) {
            const auto status = entry->symlink_status(error);
            if (!error && std::filesystem::is_regular_file(status)) {
                paths.push_back(entry->path());
            }
            error.clear();
        }
    }

    std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
        return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    paths.erase(std::unique(paths.begin(), paths.end(), path_component_equal), paths.end());
    return paths;
}

std::wstring unique_profile_name() {
    GUID identifier{};
    const HRESULT create_result = ::CoCreateGuid(&identifier);
    if (FAILED(create_result)) {
        throw_hresult(create_result, "无法生成 Windows AppContainer 标识");
    }
    std::array<wchar_t, 39> text{};
    if (::StringFromGUID2(identifier, text.data(), static_cast<int>(text.size())) == 0) {
        throw std::runtime_error("无法格式化 Windows AppContainer 标识");
    }
    return L"Mint.CommandSandbox." + std::wstring(text.data() + 1, text.size() - 3);
}

bool path_is_directory(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error;
}

void update_acl(const std::filesystem::path& path, PSID sid, ACCESS_MODE mode, DWORD access,
                DWORD inheritance) {
    auto native_path = path.wstring();
    PACL existing_acl = nullptr;
    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    const DWORD security_error =
        ::GetNamedSecurityInfoW(native_path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, &existing_acl, nullptr, &security_descriptor);
    if (security_error != ERROR_SUCCESS) {
        throw_windows_error(security_error, "无法读取 Windows 沙箱路径 ACL");
    }

    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = access;
    entry.grfAccessMode = mode;
    entry.grfInheritance = inheritance;
    ::BuildTrusteeWithSidW(&entry.Trustee, sid);

    PACL updated_acl = nullptr;
    const DWORD acl_error = ::SetEntriesInAclW(1, &entry, existing_acl, &updated_acl);
    if (acl_error != ERROR_SUCCESS) {
        (void)::LocalFree(security_descriptor);
        throw_windows_error(acl_error, "无法构建 Windows 沙箱路径 ACL");
    }

    const DWORD apply_error =
        ::SetNamedSecurityInfoW(native_path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, updated_acl, nullptr);
    (void)::LocalFree(updated_acl);
    (void)::LocalFree(security_descriptor);
    if (apply_error != ERROR_SUCCESS) {
        throw_windows_error(apply_error, "无法应用 Windows 沙箱路径 ACL");
    }
}

void grant_path(const std::filesystem::path& path, PSID sid, DWORD access, bool recursive) {
    const DWORD inheritance =
        recursive && path_is_directory(path) ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    update_acl(path, sid, GRANT_ACCESS, access, inheritance);
}

void revoke_path(const std::filesystem::path& path, PSID sid) {
    update_acl(path, sid, REVOKE_ACCESS, 0, NO_INHERITANCE);
}

std::vector<std::byte> read_security_descriptor(const std::filesystem::path& path,
                                                bool& dacl_protected) {
    auto native_path = path.wstring();
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD error =
        ::GetNamedSecurityInfoW(native_path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, &dacl, nullptr, &descriptor);
    if (error != ERROR_SUCCESS) {
        throw_windows_error(error, "无法保存 Windows 保护路径 ACL");
    }
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    if (!::GetSecurityDescriptorControl(descriptor, &control, &revision)) {
        const auto control_error = ::GetLastError();
        (void)::LocalFree(descriptor);
        throw_windows_error(control_error, "无法读取 Windows 保护路径 ACL 状态");
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    if (!::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present ||
        dacl == nullptr) {
        (void)::LocalFree(descriptor);
        throw std::invalid_argument("Windows 保护路径必须具有明确 DACL: " + path.string());
    }
    std::vector<std::byte> result;
    if ((control & SE_SELF_RELATIVE) != 0) {
        const DWORD bytes = ::GetSecurityDescriptorLength(descriptor);
        result.resize(bytes);
        std::memcpy(result.data(), descriptor, bytes);
    } else {
        DWORD bytes = 0;
        (void)::MakeSelfRelativeSD(descriptor, nullptr, &bytes);
        if (bytes == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            const auto relative_error = ::GetLastError();
            (void)::LocalFree(descriptor);
            throw_windows_error(relative_error, "无法计算 Windows 保护路径 ACL 快照空间");
        }
        result.resize(bytes);
        if (!::MakeSelfRelativeSD(descriptor, result.data(), &bytes)) {
            const auto relative_error = ::GetLastError();
            (void)::LocalFree(descriptor);
            throw_windows_error(relative_error, "无法保存 Windows 保护路径 ACL 快照");
        }
        result.resize(bytes);
    }
    (void)::LocalFree(descriptor);
    dacl_protected = (control & SE_DACL_PROTECTED) != 0;
    return result;
}

PACL saved_dacl(const std::vector<std::byte>& security_descriptor) {
    auto* descriptor =
        reinterpret_cast<PSECURITY_DESCRIPTOR>(const_cast<std::byte*>(security_descriptor.data()));
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (!::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present ||
        dacl == nullptr) {
        throw std::logic_error("保存的 Windows DACL 无效");
    }
    return dacl;
}

void protect_path(const std::filesystem::path& path,
                  const std::vector<std::byte>& security_descriptor, PSID sid) {
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = denied_access;
    entry.grfAccessMode = DENY_ACCESS;
    entry.grfInheritance = NO_INHERITANCE;
    ::BuildTrusteeWithSidW(&entry.Trustee, sid);

    PACL protected_dacl = nullptr;
    const DWORD acl_error =
        ::SetEntriesInAclW(1, &entry, saved_dacl(security_descriptor), &protected_dacl);
    if (acl_error != ERROR_SUCCESS) {
        throw_windows_error(acl_error, "无法构建 Windows 保护路径 DACL");
    }
    auto native_path = path.wstring();
    const DWORD error =
        ::SetNamedSecurityInfoW(native_path.data(), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, protected_dacl, nullptr);
    (void)::LocalFree(protected_dacl);
    if (error != ERROR_SUCCESS) {
        throw_windows_error(error, "无法应用 Windows 保护路径 DACL");
    }
}

void restore_path(const std::filesystem::path& path,
                  const std::vector<std::byte>& security_descriptor, bool dacl_protected) {
    auto native_path = path.wstring();
    const SECURITY_INFORMATION inheritance = dacl_protected ? PROTECTED_DACL_SECURITY_INFORMATION
                                                            : UNPROTECTED_DACL_SECURITY_INFORMATION;
    const DWORD error = ::SetNamedSecurityInfoW(native_path.data(), SE_FILE_OBJECT,
                                                DACL_SECURITY_INFORMATION | inheritance, nullptr,
                                                nullptr, saved_dacl(security_descriptor), nullptr);
    if (error != ERROR_SUCCESS) {
        throw_windows_error(error, "无法恢复 Windows 保护路径 DACL");
    }
}

} // namespace

std::shared_ptr<WindowsAppContainer>
WindowsAppContainer::create(const std::filesystem::path& workspace,
                            std::vector<std::filesystem::path> allowed_executables,
                            std::vector<std::filesystem::path> read_only_paths,
                            std::vector<std::filesystem::path> denied_paths) {
    return std::shared_ptr<WindowsAppContainer>(
        new WindowsAppContainer(workspace, std::move(allowed_executables),
                                std::move(read_only_paths), std::move(denied_paths)));
}

WindowsAppContainer::WindowsAppContainer(const std::filesystem::path& workspace,
                                         std::vector<std::filesystem::path> allowed_executables,
                                         std::vector<std::filesystem::path> read_only_paths,
                                         std::vector<std::filesystem::path> denied_paths) {
    try {
        initialize(workspace, std::move(allowed_executables), std::move(read_only_paths),
                   std::move(denied_paths));
    } catch (...) {
        cleanup();
        throw;
    }
}

WindowsAppContainer::~WindowsAppContainer() {
    cleanup();
}

PSID WindowsAppContainer::sid() const noexcept {
    return sid_;
}

const std::filesystem::path& WindowsAppContainer::profile_directory() const noexcept {
    return profile_directory_;
}

const std::filesystem::path& WindowsAppContainer::temp_directory() const noexcept {
    return temp_directory_;
}

void WindowsAppContainer::initialize(const std::filesystem::path& workspace,
                                     std::vector<std::filesystem::path> allowed_executables,
                                     std::vector<std::filesystem::path> read_only_paths,
                                     std::vector<std::filesystem::path> denied_paths) {
    const auto resolved_workspace = canonical_existing_path(workspace, "命令工作区");
    if (!path_is_directory(resolved_workspace)) {
        throw std::invalid_argument("命令工作区不是目录: " + resolved_workspace.string());
    }

    for (auto& denied : denied_paths) {
        std::error_code error;
        denied = std::filesystem::weakly_canonical(std::move(denied), error);
        if (error || denied.empty() || !std::filesystem::exists(denied, error) || error) {
            continue;
        }
        if (is_inside(denied, resolved_workspace)) {
            throw std::invalid_argument("命令保护路径不能包含整个工作区: " + denied.string());
        }
        ProtectedPathState state{.path = std::move(denied)};
        state.security_descriptor = read_security_descriptor(state.path, state.dacl_protected);
        protected_paths_.push_back(std::move(state));
    }

    for (auto& path : read_only_paths) {
        path = canonical_existing_path(path, "命令外部只读路径");
        if (is_inside(resolved_workspace, path) || is_inside(path, resolved_workspace)) {
            throw std::invalid_argument("命令外部只读路径不能位于工作区内或包含工作区: " +
                                        path.string());
        }
        for (const auto& protected_path : protected_paths_) {
            if (is_inside(path, protected_path.path) || is_inside(protected_path.path, path)) {
                throw std::invalid_argument("命令外部只读路径不能与保护路径重叠: " + path.string());
            }
        }
    }
    std::sort(read_only_paths.begin(), read_only_paths.end(),
              [](const auto& left, const auto& right) {
                  return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
                         CSTR_LESS_THAN;
              });
    read_only_paths.erase(
        std::unique(read_only_paths.begin(), read_only_paths.end(), path_component_equal),
        read_only_paths.end());

    profile_name_ = unique_profile_name();
    const HRESULT create_result =
        ::CreateAppContainerProfile(profile_name_.c_str(), L"Mint command sandbox",
                                    L"Temporary Mint command sandbox", nullptr, 0, &sid_);
    if (FAILED(create_result)) {
        throw_hresult(create_result, "无法创建 Windows AppContainer 配置文件");
    }

    PWSTR sid_string = nullptr;
    if (!::ConvertSidToStringSidW(sid_, &sid_string)) {
        throw_windows_error(::GetLastError(), "无法格式化 Windows AppContainer SID");
    }
    PWSTR profile_path = nullptr;
    const HRESULT path_result = ::GetAppContainerFolderPath(sid_string, &profile_path);
    (void)::LocalFree(sid_string);
    if (FAILED(path_result)) {
        throw_hresult(path_result, "无法定位 Windows AppContainer 数据目录");
    }
    profile_directory_ = profile_path;
    ::CoTaskMemFree(profile_path);
    temp_directory_ = profile_directory_ / "Temp";
    std::error_code directory_error;
    std::filesystem::create_directories(temp_directory_, directory_error);
    if (directory_error) {
        throw std::system_error(directory_error, "无法创建 Windows AppContainer 临时目录");
    }

    acl_paths_.push_back(resolved_workspace);
    grant_path(resolved_workspace, sid_, workspace_access, true);

    for (const auto& path : read_only_paths) {
        acl_paths_.push_back(path);
        grant_path(path, sid_, readonly_access, true);
    }

    for (const auto& path : executable_read_paths(resolved_workspace, allowed_executables)) {
        const bool covered = std::any_of(read_only_paths.begin(), read_only_paths.end(),
                                         [&](const auto& root) { return is_inside(root, path); });
        if (covered) {
            continue;
        }
        acl_paths_.push_back(path);
        grant_path(path, sid_, readonly_access, false);
    }

    for (auto& protected_path : protected_paths_) {
        protect_path(protected_path.path, protected_path.security_descriptor, sid_);
        protected_path.applied = true;
    }
}

void WindowsAppContainer::cleanup() noexcept {
    if (sid_ != nullptr) {
        for (auto path = acl_paths_.rbegin(); path != acl_paths_.rend(); ++path) {
            try {
                revoke_path(*path, sid_);
            } catch (...) {
            }
        }
        acl_paths_.clear();
    }
    for (auto path = protected_paths_.rbegin(); path != protected_paths_.rend(); ++path) {
        if (!path->applied) {
            continue;
        }
        try {
            restore_path(path->path, path->security_descriptor, path->dacl_protected);
        } catch (...) {
        }
    }
    protected_paths_.clear();
    if (!profile_name_.empty()) {
        (void)::DeleteAppContainerProfile(profile_name_.c_str());
        profile_name_.clear();
    }
    if (sid_ != nullptr) {
        (void)::FreeSid(sid_);
        sid_ = nullptr;
    }
}

} // namespace mint::command_detail
