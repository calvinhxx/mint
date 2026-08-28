#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "command_appcontainer_windows.hpp"
#include "command_process.hpp"

#include "command_resource_monitor.hpp"

#include "mint/runtime/task_control.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace mint::command_detail {
namespace {

class UniqueHandle final {
  public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueHandle() {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE release() noexcept {
        const auto handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (*this) {
            (void)::CloseHandle(handle_);
        }
        handle_ = handle;
    }

  private:
    HANDLE handle_ = nullptr;
};

class AttributeList final {
  public:
    AttributeList(const std::array<HANDLE, 2>& inherited_handles, PSID appcontainer_sid) {
        const DWORD attribute_count = appcontainer_sid == nullptr ? 1 : 2;
        SIZE_T bytes = 0;
        (void)::InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &bytes);
        if (bytes == 0) {
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    "无法计算 Windows 进程属性空间");
        }
        storage_.resize(bytes);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!::InitializeProcThreadAttributeList(list_, attribute_count, 0, &bytes)) {
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    "无法初始化 Windows 进程属性");
        }
        if (!::UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                         const_cast<HANDLE*>(inherited_handles.data()),
                                         sizeof(inherited_handles), nullptr, nullptr)) {
            const auto error = ::GetLastError();
            ::DeleteProcThreadAttributeList(list_);
            list_ = nullptr;
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "无法限制 Windows 子进程继承句柄");
        }
        if (appcontainer_sid != nullptr) {
            security_capabilities_.AppContainerSid = appcontainer_sid;
            if (!::UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                             &security_capabilities_,
                                             sizeof(security_capabilities_), nullptr, nullptr)) {
                const auto error = ::GetLastError();
                ::DeleteProcThreadAttributeList(list_);
                list_ = nullptr;
                throw std::system_error(static_cast<int>(error), std::system_category(),
                                        "无法配置 Windows AppContainer 进程属性");
            }
        }
    }

    ~AttributeList() {
        if (list_ != nullptr) {
            ::DeleteProcThreadAttributeList(list_);
        }
    }

    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;

    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept {
        return list_;
    }

  private:
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
    SECURITY_CAPABILITIES security_capabilities_{};
};

class EnvironmentStrings final {
  public:
    EnvironmentStrings() : data_(::GetEnvironmentStringsW()) {
        if (data_ == nullptr) {
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    "无法读取 Windows 环境变量");
        }
    }

    ~EnvironmentStrings() {
        (void)::FreeEnvironmentStringsW(data_);
    }

    EnvironmentStrings(const EnvironmentStrings&) = delete;
    EnvironmentStrings& operator=(const EnvironmentStrings&) = delete;

    [[nodiscard]] const wchar_t* get() const noexcept {
        return data_;
    }

  private:
    LPWCH data_ = nullptr;
};

[[noreturn]] void throw_last_error(std::string_view context) {
    throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                            std::string(context));
}

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("Windows 命令参数过长");
    }
    const auto input_size = static_cast<int>(value.size());
    const auto output_size =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0);
    if (output_size <= 0) {
        throw_last_error("Windows 命令参数不是有效 UTF-8");
    }
    std::wstring result(static_cast<std::size_t>(output_size), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size,
                              result.data(), output_size) != output_size) {
        throw_last_error("无法转换 Windows 命令参数");
    }
    return result;
}

std::wstring quote_argument(std::wstring_view argument) {
    std::wstring result;
    result.reserve(argument.size() + 2);
    result.push_back(L'"');

    std::size_t backslashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::vector<wchar_t> command_line(const std::vector<std::string>& arguments) {
    std::wstring line;
    for (const auto& argument : arguments) {
        if (!line.empty()) {
            line.push_back(L' ');
        }
        line += quote_argument(utf8_to_wide(argument));
    }
    std::vector<wchar_t> result(line.begin(), line.end());
    result.push_back(L'\0');
    return result;
}

std::wstring uppercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towupper(character));
    });
    return value;
}

std::vector<wchar_t>
filtered_environment(const std::shared_ptr<const WindowsAppContainer>& appcontainer) {
    static const std::unordered_set<std::wstring> allowed_names = {L"PATH",
                                                                   L"PATHEXT",
                                                                   L"SYSTEMROOT",
                                                                   L"WINDIR",
                                                                   L"COMSPEC",
                                                                   L"TEMP",
                                                                   L"TMP",
                                                                   L"HOME",
                                                                   L"LOCALAPPDATA",
                                                                   L"USERPROFILE",
                                                                   L"HOMEDRIVE",
                                                                   L"HOMEPATH",
                                                                   L"USERNAME",
                                                                   L"LANG",
                                                                   L"LC_ALL",
                                                                   L"LC_CTYPE",
                                                                   L"TERM",
                                                                   L"CC",
                                                                   L"CXX",
                                                                   L"CFLAGS",
                                                                   L"CXXFLAGS",
                                                                   L"CPPFLAGS",
                                                                   L"LDFLAGS",
                                                                   L"INCLUDE",
                                                                   L"LIB",
                                                                   L"LIBPATH",
                                                                   L"VCINSTALLDIR",
                                                                   L"VCTOOLSINSTALLDIR",
                                                                   L"WINDOWSSDKDIR",
                                                                   L"WINDOWSSDKVERSION",
                                                                   L"UNIVERSALCRTSDKDIR",
                                                                   L"UCRTVERSION",
                                                                   L"VISUALSTUDIOVERSION",
                                                                   L"VSINSTALLDIR",
                                                                   L"DEVENVDIR",
                                                                   L"CMAKE_PREFIX_PATH",
                                                                   L"CMAKE_TOOLCHAIN_FILE",
                                                                   L"VCPKG_ROOT",
                                                                   L"VCPKG_INSTALLATION_ROOT",
                                                                   L"NINJA_STATUS",
                                                                   L"MAKEFLAGS"};

    const EnvironmentStrings raw_environment;

    static const std::unordered_set<std::wstring> sandbox_overrides = {
        L"TEMP", L"TMP", L"HOME", L"LOCALAPPDATA", L"USERPROFILE", L"HOMEDRIVE", L"HOMEPATH"};

    std::vector<std::wstring> entries;
    for (const wchar_t* current = raw_environment.get(); *current != L'\0';
         current += std::wcslen(current) + 1) {
        const std::wstring entry(current);
        const auto separator = entry.find(L'=');
        if (separator == std::wstring::npos || separator == 0) {
            continue;
        }
        const auto name = uppercase(entry.substr(0, separator));
        if (allowed_names.contains(name) &&
            (appcontainer == nullptr || !sandbox_overrides.contains(name))) {
            entries.push_back(entry);
        }
    }
    if (appcontainer != nullptr) {
        const auto profile = appcontainer->profile_directory().wstring();
        const auto temp = appcontainer->temp_directory().wstring();
        const auto home_drive = appcontainer->profile_directory().root_name().wstring();
        const auto home_path = profile.substr(home_drive.size());
        entries.insert(entries.end(), {L"HOME=" + profile, L"HOMEDRIVE=" + home_drive,
                                       L"HOMEPATH=" + home_path, L"LOCALAPPDATA=" + profile,
                                       L"TEMP=" + temp, L"TMP=" + temp, L"USERPROFILE=" + profile});
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });

    if (entries.empty()) {
        return {L'\0', L'\0'};
    }

    std::size_t bytes = 1;
    for (const auto& entry : entries) {
        bytes += entry.size() + 1;
    }
    std::vector<wchar_t> result;
    result.reserve(bytes);
    for (const auto& entry : entries) {
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

void append_output(std::string& output, const char* data, std::size_t size, std::size_t limit,
                   bool& truncated) {
    const auto remaining = output.size() < limit ? limit - output.size() : 0;
    const auto accepted = std::min(size, remaining);
    output.append(data, accepted);
    truncated = truncated || accepted < size;
}

void drain_output(HANDLE pipe, ProcessResult& result, std::size_t limit) {
    std::array<char, 4096> buffer{};
    while (true) {
        DWORD available = 0;
        if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            if (::GetLastError() == ERROR_BROKEN_PIPE) {
                return;
            }
            throw_last_error("无法读取 Windows 命令输出状态");
        }
        if (available == 0) {
            return;
        }
        DWORD bytes_read = 0;
        const auto requested = static_cast<DWORD>(
            std::min<std::size_t>(buffer.size(), static_cast<std::size_t>(available)));
        if (!::ReadFile(pipe, buffer.data(), requested, &bytes_read, nullptr)) {
            if (::GetLastError() == ERROR_BROKEN_PIPE) {
                return;
            }
            throw_last_error("无法读取 Windows 命令输出");
        }
        append_output(result.output, buffer.data(), bytes_read, limit, result.output_truncated);
    }
}

struct JobHandles {
    UniqueHandle job;
    UniqueHandle completion_port;
};

JobHandles create_job(const CommandResourceLimits& limits) {
    JobHandles handles{UniqueHandle(::CreateJobObjectW(nullptr, nullptr)),
                       UniqueHandle(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1))};
    if (!handles.job) {
        throw_last_error("无法创建 Windows Job Object");
    }
    if (!handles.completion_port) {
        throw_last_error("无法创建 Windows Job Object 通知端口");
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION information{};
    information.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (limits.cpu_seconds != 0) {
        information.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_TIME;
        information.BasicLimitInformation.PerJobUserTimeLimit.QuadPart =
            static_cast<LONGLONG>(limits.cpu_seconds) * 10'000'000LL;
    }
    if (limits.memory_bytes != 0) {
        information.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        information.JobMemoryLimit = static_cast<SIZE_T>(limits.memory_bytes);
    }
    if (limits.max_processes != 0) {
        information.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        information.BasicLimitInformation.ActiveProcessLimit =
            static_cast<DWORD>(limits.max_processes);
    }
    if (!::SetInformationJobObject(handles.job.get(), JobObjectExtendedLimitInformation,
                                   &information, sizeof(information))) {
        throw_last_error("无法配置 Windows Job Object 资源限制");
    }

    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
    association.CompletionKey = handles.job.get();
    association.CompletionPort = handles.completion_port.get();
    if (!::SetInformationJobObject(handles.job.get(), JobObjectAssociateCompletionPortInformation,
                                   &association, sizeof(association))) {
        throw_last_error("无法订阅 Windows Job Object 资源事件");
    }
    return handles;
}

std::string poll_resource_limit(HANDLE completion_port) {
    std::string detected;
    while (true) {
        DWORD message = 0;
        ULONG_PTR completion_key = 0;
        LPOVERLAPPED context = nullptr;
        if (!::GetQueuedCompletionStatus(completion_port, &message, &completion_key, &context, 0)) {
            const auto error = ::GetLastError();
            if (error == WAIT_TIMEOUT) {
                return detected;
            }
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "无法读取 Windows Job Object 资源事件");
        }
        (void)completion_key;
        (void)context;
        switch (message) {
        case JOB_OBJECT_MSG_END_OF_JOB_TIME:
            detected = "cpu";
            break;
        case JOB_OBJECT_MSG_JOB_MEMORY_LIMIT:
        case JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT:
            detected = "memory";
            break;
        case JOB_OBJECT_MSG_ACTIVE_PROCESS_LIMIT:
            detected = "processes";
            break;
        default:
            break;
        }
    }
}

DWORD active_processes(HANDLE job) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION information{};
    if (!::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &information,
                                     sizeof(information), nullptr)) {
        throw_last_error("无法查询 Windows Job Object 状态");
    }
    return information.ActiveProcesses;
}

} // namespace

void validate_process_resource_support(const CommandResourceLimits& limits) {
    if (limits.file_size_bytes != 0) {
        throw std::invalid_argument(
            "Windows Job Object 不支持单文件大小限制；请将 command_resources.file_size_bytes "
            "设为 0");
    }
}

ProcessResult execute_process(ProcessRequest request) {
    if (request.argv.empty()) {
        throw std::logic_error("内部命令请求缺少 argv");
    }
    validate_process_resource_support(request.resource_limits);

    const auto started_at = std::chrono::steady_clock::now();
    if (workspace_disk_limit_exceeded(request.workspace_root,
                                      request.resource_limits.workspace_disk_bytes)) {
        return {.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started_at)
                                   .count(),
                .status = "resource_limited",
                .resource_limited = true,
                .resource_limit = "workspace_disk"};
    }

    SECURITY_ATTRIBUTES inherited_attributes{};
    inherited_attributes.nLength = sizeof(inherited_attributes);
    inherited_attributes.bInheritHandle = TRUE;

    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!::CreatePipe(&read_handle, &write_handle, &inherited_attributes, 0)) {
        throw_last_error("无法创建 Windows 命令输出管道");
    }
    UniqueHandle output_read(read_handle);
    UniqueHandle output_write(write_handle);
    if (!::SetHandleInformation(output_read.get(), HANDLE_FLAG_INHERIT, 0)) {
        throw_last_error("无法保护 Windows 命令输出管道");
    }

    UniqueHandle input(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &inherited_attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                     nullptr));
    if (!input) {
        throw_last_error("无法打开 Windows 空输入设备");
    }

    const std::array inherited_handles = {input.get(), output_write.get()};
    const auto appcontainer_sid =
        request.windows_appcontainer == nullptr ? nullptr : request.windows_appcontainer->sid();
    AttributeList attributes(inherited_handles, appcontainer_sid);
    auto job = create_job(request.resource_limits);
    auto arguments = command_line(request.argv);
    auto environment = filtered_environment(request.windows_appcontainer);
    const auto executable = request.executable.wstring();
    const auto working_directory = request.cwd.wstring();

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = output_write.get();
    startup.StartupInfo.hStdError = output_write.get();
    startup.lpAttributeList = attributes.get();

    PROCESS_INFORMATION process_information{};
    const DWORD creation_flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW |
                                 EXTENDED_STARTUPINFO_PRESENT;
    if (!::CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr, TRUE,
                          creation_flags, environment.data(), working_directory.c_str(),
                          &startup.StartupInfo, &process_information)) {
        throw_last_error("无法启动 Windows 命令进程");
    }

    UniqueHandle process(process_information.hProcess);
    UniqueHandle thread(process_information.hThread);
    if (!::AssignProcessToJobObject(job.job.get(), process.get())) {
        (void)::TerminateProcess(process.get(), 126);
        throw_last_error("无法将 Windows 命令加入 Job Object");
    }
    if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        (void)::TerminateJobObject(job.job.get(), 126);
        throw_last_error("无法恢复 Windows 命令线程");
    }
    thread.reset();
    input.reset();
    output_write.reset();

    ProcessResult result;
    result.output.reserve(std::min<std::size_t>(request.max_output_bytes, 16 * 1024));
    bool root_exited = false;
    bool terminated = false;
    auto next_workspace_check = started_at + std::chrono::milliseconds(100);

    const auto terminate_job = [&]() {
        if (!terminated) {
            if (!::TerminateJobObject(job.job.get(), 1) &&
                ::GetLastError() != ERROR_ACCESS_DENIED) {
                throw_last_error("无法终止 Windows 命令进程树");
            }
            terminated = true;
        }
    };

    const auto deadline = started_at + std::chrono::seconds(request.timeout_seconds);
    while (true) {
        drain_output(output_read.get(), result, request.max_output_bytes);

        if (const auto limit = poll_resource_limit(job.completion_port.get()); !limit.empty()) {
            result.resource_limited = true;
            result.resource_limit = limit;
            terminate_job();
        }
        if (!result.resource_limited && request.resource_limits.workspace_disk_bytes != 0 &&
            std::chrono::steady_clock::now() >= next_workspace_check) {
            next_workspace_check =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            if (workspace_disk_limit_exceeded(request.workspace_root,
                                              request.resource_limits.workspace_disk_bytes)) {
                result.resource_limited = true;
                result.resource_limit = "workspace_disk";
                terminate_job();
            }
        }
        if (!result.resource_limited && request.task_control != nullptr &&
            request.task_control->cancellation_requested()) {
            result.cancelled = true;
            terminate_job();
        }
        if (!result.resource_limited && !result.cancelled && request.task_control != nullptr &&
            request.task_control->budget_exhausted()) {
            result.task_timed_out = true;
            terminate_job();
        }
        if (!result.resource_limited && !result.cancelled && !result.task_timed_out &&
            std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            terminate_job();
        }

        const auto wait_result = ::WaitForSingleObject(process.get(), 0);
        if (wait_result == WAIT_OBJECT_0) {
            root_exited = true;
        } else if (wait_result != WAIT_TIMEOUT) {
            terminate_job();
            throw_last_error("等待 Windows 命令进程失败");
        }

        if (root_exited && active_processes(job.job.get()) == 0) {
            break;
        }
        if (root_exited) {
            ::Sleep(20);
        } else {
            (void)::WaitForSingleObject(process.get(), 20);
        }
    }

    if (!result.resource_limited) {
        if (const auto limit = poll_resource_limit(job.completion_port.get()); !limit.empty()) {
            result.resource_limited = true;
            result.resource_limit = limit;
        }
    }
    if (!result.resource_limited && !result.cancelled && !result.task_timed_out &&
        !result.timed_out &&
        workspace_disk_limit_exceeded(request.workspace_root,
                                      request.resource_limits.workspace_disk_bytes)) {
        result.resource_limited = true;
        result.resource_limit = "workspace_disk";
    }
    drain_output(output_read.get(), result, request.max_output_bytes);
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started_at)
                             .count();

    DWORD exit_code = 0;
    if (!::GetExitCodeProcess(process.get(), &exit_code)) {
        throw_last_error("无法读取 Windows 命令退出码");
    }

    if (result.cancelled) {
        result.status = "cancelled";
    } else if (result.task_timed_out) {
        result.status = "task_timed_out";
    } else if (result.timed_out) {
        result.status = "timed_out";
    } else if (result.resource_limited) {
        result.status = "resource_limited";
    } else {
        result.status = "exited";
        result.exit_code = static_cast<int>(exit_code);
    }
    return result;
}

} // namespace mint::command_detail
