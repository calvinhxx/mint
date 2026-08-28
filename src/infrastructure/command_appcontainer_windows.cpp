#include "command_appcontainer_windows.hpp"

#include <aclapi.h>
#include <objbase.h>
#include <sddl.h>
#include <userenv.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace mint::command_detail {
namespace {

constexpr DWORD workspace_access =
    FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE | FILE_DELETE_CHILD;
constexpr DWORD readonly_access = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
constexpr DWORD denied_access = workspace_access;

std::atomic<unsigned long long> profile_counter{0};

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

std::optional<std::filesystem::path> normalized_existing_path(std::wstring_view value) {
    if (value.empty() || value.find(L'\0') != std::wstring_view::npos) {
        return std::nullopt;
    }
    std::filesystem::path path(value);
    if (!path.is_absolute()) {
        return std::nullopt;
    }
    std::error_code error;
    path = std::filesystem::weakly_canonical(std::move(path), error);
    if (error || path.empty() || !std::filesystem::exists(path, error) || error) {
        return std::nullopt;
    }
    return path;
}

std::optional<std::wstring> environment_value(const wchar_t* name) {
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
    return value;
}

void append_path_list(std::vector<std::filesystem::path>& paths, const wchar_t* name) {
    const auto raw_value = environment_value(name);
    if (!raw_value) {
        return;
    }
    std::size_t begin = 0;
    while (begin <= raw_value->size()) {
        const auto end = raw_value->find(L';', begin);
        const auto length = end == std::wstring::npos ? raw_value->size() - begin : end - begin;
        auto value = std::wstring_view(*raw_value).substr(begin, length);
        if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
        if (const auto path = normalized_existing_path(value)) {
            paths.push_back(*path);
        }
        if (end == std::wstring::npos) {
            break;
        }
        begin = end + 1;
    }
}

void append_single_path(std::vector<std::filesystem::path>& paths, const wchar_t* name) {
    const auto value = environment_value(name);
    if (!value) {
        return;
    }
    if (const auto path = normalized_existing_path(*value)) {
        paths.push_back(*path);
    }
}

std::vector<std::filesystem::path>
runtime_read_paths(const std::filesystem::path& workspace,
                   const std::vector<std::filesystem::path>& allowed_executables) {
    std::vector<std::filesystem::path> paths;
    paths.reserve(allowed_executables.size() * 2 + 32);
    for (const auto& executable : allowed_executables) {
        paths.push_back(executable);
        paths.push_back(executable.parent_path());
    }

    for (const auto* name : {L"PATH", L"INCLUDE", L"LIB", L"LIBPATH", L"CMAKE_PREFIX_PATH"}) {
        append_path_list(paths, name);
    }
    for (const auto* name : {L"VCPKG_ROOT", L"VCPKG_INSTALLATION_ROOT", L"CMAKE_TOOLCHAIN_FILE",
                             L"VCINSTALLDIR", L"VCTOOLSINSTALLDIR", L"WINDOWSSDKDIR",
                             L"UNIVERSALCRTSDKDIR", L"VSINSTALLDIR", L"DEVENVDIR", L"CC", L"CXX"}) {
        append_single_path(paths, name);
    }

    paths.erase(std::remove_if(paths.begin(), paths.end(),
                               [&](const auto& path) {
                                   return path.empty() || is_inside(workspace, path) ||
                                          is_inside(path, workspace);
                               }),
                paths.end());
    std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
        return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    paths.erase(std::unique(paths.begin(), paths.end(), path_component_equal), paths.end());
    return paths;
}

std::wstring unique_profile_name() {
    const auto sequence = profile_counter.fetch_add(1, std::memory_order_relaxed);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return L"Mint.CommandSandbox." + std::to_wstring(::GetCurrentProcessId()) + L"." +
           std::to_wstring(stamp) + L"." + std::to_wstring(sequence);
}

bool is_directory(const std::filesystem::path& path) {
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

void grant_path(const std::filesystem::path& path, PSID sid, DWORD access) {
    const DWORD inheritance =
        is_directory(path) ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    update_acl(path, sid, GRANT_ACCESS, access, inheritance);
}

void deny_path(const std::filesystem::path& path, PSID sid) {
    const DWORD inheritance =
        is_directory(path) ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    update_acl(path, sid, DENY_ACCESS, denied_access, inheritance);
}

void revoke_path(const std::filesystem::path& path, PSID sid) {
    update_acl(path, sid, REVOKE_ACCESS, 0, NO_INHERITANCE);
}

} // namespace

std::shared_ptr<WindowsAppContainer>
WindowsAppContainer::create(const std::filesystem::path& workspace,
                            std::vector<std::filesystem::path> allowed_executables,
                            std::vector<std::filesystem::path> denied_paths) {
    return std::shared_ptr<WindowsAppContainer>(new WindowsAppContainer(
        workspace, std::move(allowed_executables), std::move(denied_paths)));
}

WindowsAppContainer::WindowsAppContainer(const std::filesystem::path& workspace,
                                         std::vector<std::filesystem::path> allowed_executables,
                                         std::vector<std::filesystem::path> denied_paths) {
    try {
        initialize(workspace, std::move(allowed_executables), std::move(denied_paths));
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
                                     std::vector<std::filesystem::path> denied_paths) {
    const auto resolved_workspace = canonical_existing_path(workspace, "命令工作区");
    if (!is_directory(resolved_workspace)) {
        throw std::invalid_argument("命令工作区不是目录: " + resolved_workspace.string());
    }

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
    grant_path(resolved_workspace, sid_, workspace_access);

    for (const auto& path : runtime_read_paths(resolved_workspace, allowed_executables)) {
        acl_paths_.push_back(path);
        grant_path(path, sid_, readonly_access);
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
        acl_paths_.push_back(std::move(denied));
        deny_path(acl_paths_.back(), sid_);
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
