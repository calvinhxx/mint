#include "command_process.hpp"

#include "command_process_tree.hpp"
#include "command_resource_monitor.hpp"

#include "mint/runtime/task_control.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#endif

extern char** environ;

namespace mint::command_detail {
namespace {

std::vector<std::string>
filtered_environment(const std::vector<std::pair<std::string, std::string>>& overrides) {
    static const std::unordered_set<std::string> allowed_names = {"PATH",
                                                                  "HOME",
                                                                  "TMPDIR",
                                                                  "TMP",
                                                                  "TEMP",
                                                                  "LANG",
                                                                  "LC_ALL",
                                                                  "LC_CTYPE",
                                                                  "TERM",
                                                                  "USER",
                                                                  "LOGNAME",
                                                                  "SHELL",
                                                                  "SDKROOT",
                                                                  "DEVELOPER_DIR",
                                                                  "MACOSX_DEPLOYMENT_TARGET",
                                                                  "CC",
                                                                  "CXX",
                                                                  "CFLAGS",
                                                                  "CXXFLAGS",
                                                                  "CPPFLAGS",
                                                                  "LDFLAGS",
                                                                  "CMAKE_PREFIX_PATH",
                                                                  "CMAKE_TOOLCHAIN_FILE",
                                                                  "VCPKG_ROOT",
                                                                  "PKG_CONFIG_PATH",
                                                                  "NINJA_STATUS",
                                                                  "MAKEFLAGS"};

    std::unordered_set<std::string> overridden_names;
    overridden_names.reserve(overrides.size());
    for (const auto& [name, value] : overrides) {
        if (name.empty() || name.find('=') != std::string::npos ||
            name.find('\0') != std::string::npos || value.find('\0') != std::string::npos ||
            !allowed_names.contains(name) || !overridden_names.insert(name).second) {
            throw std::logic_error("内部命令环境变量覆盖无效");
        }
    }

    std::vector<std::string> result;
    bool has_path = overridden_names.contains("PATH");
    for (char** current = environ; current != nullptr && *current != nullptr; ++current) {
        const std::string entry(*current);
        const auto separator = entry.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const auto name = entry.substr(0, separator);
        if (!allowed_names.contains(name) || overridden_names.contains(name)) {
            continue;
        }
        has_path = has_path || name == "PATH";
        result.push_back(entry);
    }
    if (!has_path) {
        result.emplace_back("PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin");
    }
    for (const auto& [name, value] : overrides) {
        result.push_back(name + "=" + value);
    }
    return result;
}

std::vector<char*> mutable_pointers(std::vector<std::string>& values) {
    std::vector<char*> result;
    result.reserve(values.size() + 1);
    for (auto& value : values) {
        result.push_back(value.data());
    }
    result.push_back(nullptr);
    return result;
}

void append_output(std::string& output, const char* data, std::size_t size, std::size_t limit,
                   bool& truncated) {
    const auto remaining = output.size() < limit ? limit - output.size() : 0;
    const auto accepted = std::min(size, remaining);
    output.append(data, accepted);
    truncated = truncated || accepted < size;
}

void write_best_effort(int descriptor, std::string_view message) noexcept {
    auto* data = message.data();
    auto remaining = message.size();
    while (remaining > 0) {
        const auto written = ::write(descriptor, data, remaining);
        if (written > 0) {
            const auto accepted = static_cast<std::size_t>(written);
            data += accepted;
            remaining -= accepted;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

void close_inherited_descriptors() {
#if defined(__linux__) && defined(SYS_close_range)
    if (::syscall(SYS_close_range, 3U, ~0U, 0U) == 0) {
        return;
    }
#endif

    const auto descriptor_limit = ::sysconf(_SC_OPEN_MAX);
    const auto last_descriptor = descriptor_limit > 0 ? descriptor_limit : 1024;
    for (long descriptor = 3; descriptor < last_descriptor; ++descriptor) {
        (void)::close(static_cast<int>(descriptor));
    }
}

bool set_resource_limit(int resource, std::size_t value) noexcept {
    if (value == 0) {
        return true;
    }
    const auto limit = static_cast<rlim_t>(value);
    const struct rlimit bounds = {limit, limit};
    return ::setrlimit(resource, &bounds) == 0;
}

bool set_cpu_limit(std::size_t seconds) noexcept {
    if (seconds == 0) {
        return true;
    }
    const auto soft = static_cast<rlim_t>(seconds);
    const auto hard = static_cast<rlim_t>(seconds + 1);
    const struct rlimit bounds = {soft, hard};
    return ::setrlimit(RLIMIT_CPU, &bounds) == 0;
}

enum class ResourceLimitError { none, cpu, memory, file_size };

ResourceLimitError apply_resource_limits(const CommandResourceLimits& limits) noexcept {
    if (!set_cpu_limit(limits.cpu_seconds)) {
        return ResourceLimitError::cpu;
    }
    if (!set_resource_limit(RLIMIT_FSIZE, limits.file_size_bytes)) {
        return ResourceLimitError::file_size;
    }
#if defined(__linux__) && defined(RLIMIT_AS)
    if (!set_resource_limit(RLIMIT_AS, limits.memory_bytes)) {
        return ResourceLimitError::memory;
    }
#elif defined(__APPLE__)
    // macOS rejects finite RLIMIT_AS values. The parent process enforces
    // resident memory with proc_pid_rusage instead.
    (void)limits.memory_bytes;
#else
    if (limits.memory_bytes != 0) {
        return ResourceLimitError::memory;
    }
#endif
    return ResourceLimitError::none;
}

std::string_view resource_limit_error_message(ResourceLimitError error) noexcept {
    switch (error) {
    case ResourceLimitError::cpu:
        return "mint: failed to apply cpu resource limit\n";
    case ResourceLimitError::memory:
        return "mint: failed to apply memory resource limit\n";
    case ResourceLimitError::file_size:
        return "mint: failed to apply file-size resource limit\n";
    case ResourceLimitError::none:
        break;
    }
    return {};
}

void reset_resource_signals() noexcept {
    for (const auto signal : {SIGPIPE, SIGXCPU, SIGXFSZ}) {
        struct sigaction action = {};
        action.sa_handler = SIG_DFL;
        (void)sigemptyset(&action.sa_mask);
        (void)::sigaction(signal, &action, nullptr);
    }
}

std::optional<std::size_t> resident_memory_bytes(pid_t process) noexcept {
#if defined(__APPLE__)
    struct rusage_info_v2 usage = {};
    if (::proc_pid_rusage(process, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&usage)) != 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(usage.ri_resident_size);
#else
    (void)process;
    return std::nullopt;
#endif
}

} // namespace

void validate_process_resource_support(const CommandResourceLimits& limits) {
    (void)limits;
}

ProcessResult execute_process(ProcessRequest request) {
    if (request.argv.empty()) {
        throw std::logic_error("内部命令请求缺少 argv");
    }

    auto argv = mutable_pointers(request.argv);
    auto environment_storage = filtered_environment(request.posix_environment_overrides);
    auto environment = mutable_pointers(environment_storage);

    const auto started_at = std::chrono::steady_clock::now();
    if (workspace_disk_limit_exceeded(request.workspace_root,
                                      request.resource_limits.workspace_disk_bytes)) {
        ProcessResult result;
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started_at)
                                 .count();
        result.status = "resource_limited";
        result.resource_limited = true;
        result.resource_limit = "workspace_disk";
        return result;
    }

    const auto input_descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (input_descriptor < 0) {
        throw std::runtime_error("无法打开命令空输入设备: " + std::string(std::strerror(errno)));
    }

    std::array<int, 2> output_pipe{};
    if (::pipe(output_pipe.data()) != 0) {
        ::close(input_descriptor);
        throw std::runtime_error("无法创建命令输出管道: " + std::string(std::strerror(errno)));
    }

    const auto read_flags = ::fcntl(output_pipe[0], F_GETFL, 0);
    if (read_flags < 0 || ::fcntl(output_pipe[0], F_SETFL, read_flags | O_NONBLOCK) != 0) {
        const auto message = std::string(std::strerror(errno));
        ::close(input_descriptor);
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        throw std::runtime_error("无法配置命令输出管道: " + message);
    }

    const auto process = ::fork();
    if (process < 0) {
        const auto message = std::string(std::strerror(errno));
        ::close(input_descriptor);
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        throw std::runtime_error("无法启动命令进程: " + message);
    }

    if (process == 0) {
        (void)::setpgid(0, 0);
        if (::chdir(request.cwd.c_str()) != 0 || ::dup2(input_descriptor, STDIN_FILENO) < 0 ||
            ::dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
            ::dup2(output_pipe[1], STDERR_FILENO) < 0) {
            constexpr char message[] = "mint: failed to prepare approved command\n";
            write_best_effort(output_pipe[1], std::string_view(message, sizeof(message) - 1));
            ::_exit(126);
        }
        ::close(input_descriptor);
        reset_resource_signals();
        const auto limit_error = apply_resource_limits(request.resource_limits);
        if (limit_error != ResourceLimitError::none) {
            const auto message = resource_limit_error_message(limit_error);
            write_best_effort(STDERR_FILENO, message);
            ::_exit(126);
        }
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        close_inherited_descriptors();
        ::execve(request.executable.c_str(), argv.data(), environment.data());
        constexpr char message[] = "mint: failed to execute approved command\n";
        write_best_effort(STDERR_FILENO, std::string_view(message, sizeof(message) - 1));
        ::_exit(127);
    }

    ::close(input_descriptor);
    ::close(output_pipe[1]);
    (void)::setpgid(process, process);

    ProcessResult result;
    result.output.reserve(std::min<std::size_t>(request.max_output_bytes, 16 * 1024));
    bool root_exited = false;
    int wait_status = 0;
    ProcessTreeMonitor process_tree(process);
    auto next_workspace_check = started_at + std::chrono::milliseconds(100);

    const auto drain_output = [&]() {
        std::array<char, 4096> buffer{};
        while (true) {
            const auto bytes = ::read(output_pipe[0], buffer.data(), buffer.size());
            if (bytes > 0) {
                append_output(result.output, buffer.data(), static_cast<std::size_t>(bytes),
                              request.max_output_bytes, result.output_truncated);
                continue;
            }
            if (bytes < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    };

    const auto terminate_process = [&]() {
        process_tree.signal_all(SIGTERM);
        const auto grace_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < grace_deadline) {
            drain_output();
            if (!root_exited) {
                const auto grace_wait = ::waitpid(process, &wait_status, WNOHANG);
                if (grace_wait == process) {
                    root_exited = true;
                }
            }
            if (root_exited && process_tree.refresh() == 0) {
                return;
            }
            pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
            (void)::poll(&descriptor, 1, 10);
        }
        process_tree.signal_all(SIGKILL);
        if (!root_exited) {
            while (::waitpid(process, &wait_status, 0) < 0 && errno == EINTR) {
            }
            root_exited = true;
        }
        const auto kill_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (process_tree.refresh() != 0 && std::chrono::steady_clock::now() < kill_deadline) {
            process_tree.signal_all(SIGKILL);
            pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
            (void)::poll(&descriptor, 1, 10);
        }
    };

    const auto deadline = started_at + std::chrono::seconds(request.timeout_seconds);
    while (true) {
        drain_output();
        if (!root_exited) {
            const auto waited = ::waitpid(process, &wait_status, WNOHANG);
            if (waited == process) {
                root_exited = true;
            } else if (waited < 0 && errno != EINTR) {
                const auto message = std::string(std::strerror(errno));
                process_tree.signal_all(SIGKILL);
                (void)::waitpid(process, &wait_status, 0);
                ::close(output_pipe[0]);
                throw std::runtime_error("等待命令进程失败: " + message);
            }
        }

        const auto live_processes = process_tree.refresh();
        if (root_exited && live_processes == 0) {
            break;
        }

        if (request.task_control != nullptr && request.task_control->cancellation_requested()) {
            result.cancelled = true;
            terminate_process();
            break;
        }
        if (request.task_control != nullptr && request.task_control->budget_exhausted()) {
            result.task_timed_out = true;
            terminate_process();
            break;
        }
        if (request.resource_limits.memory_bytes != 0) {
            const auto resident = resident_memory_bytes(process);
            if (resident.has_value() && *resident > request.resource_limits.memory_bytes) {
                result.resource_limited = true;
                result.resource_limit = "memory";
                terminate_process();
                break;
            }
        }
        if (request.resource_limits.max_processes != 0 &&
            live_processes > request.resource_limits.max_processes) {
            result.resource_limited = true;
            result.resource_limit = "processes";
            terminate_process();
            break;
        }
        if (request.resource_limits.workspace_disk_bytes != 0 &&
            std::chrono::steady_clock::now() >= next_workspace_check) {
            next_workspace_check =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            if (workspace_disk_limit_exceeded(request.workspace_root,
                                              request.resource_limits.workspace_disk_bytes)) {
                result.resource_limited = true;
                result.resource_limit = "workspace_disk";
                terminate_process();
                break;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            terminate_process();
            break;
        }

        pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
        (void)::poll(&descriptor, 1, 20);
    }

    drain_output();
    ::close(output_pipe[0]);
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started_at)
                             .count();

    if (!result.cancelled && !result.task_timed_out && !result.timed_out &&
        !result.resource_limited &&
        workspace_disk_limit_exceeded(request.workspace_root,
                                      request.resource_limits.workspace_disk_bytes)) {
        result.resource_limited = true;
        result.resource_limit = "workspace_disk";
    }

    if (!result.cancelled && !result.task_timed_out && !result.timed_out &&
        !result.resource_limited && WIFSIGNALED(wait_status)) {
        const auto signal = WTERMSIG(wait_status);
        if (signal == SIGXCPU) {
            result.resource_limited = true;
            result.resource_limit = "cpu";
        } else if (signal == SIGXFSZ) {
            result.resource_limited = true;
            result.resource_limit = "file_size";
        }
    }

    if (result.cancelled) {
        result.status = "cancelled";
    } else if (result.task_timed_out) {
        result.status = "task_timed_out";
    } else if (result.timed_out) {
        result.status = "timed_out";
    } else if (result.resource_limited) {
        result.status = "resource_limited";
    } else if (WIFEXITED(wait_status)) {
        result.status = "exited";
        result.exit_code = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        result.status = "signaled";
        result.signal = WTERMSIG(wait_status);
    } else {
        result.status = "unknown";
    }
    if ((result.cancelled || result.task_timed_out || result.timed_out) &&
        WIFSIGNALED(wait_status)) {
        result.signal = WTERMSIG(wait_status);
    }
    return result;
}

} // namespace mint::command_detail
