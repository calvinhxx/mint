#include "command_process.hpp"

#include "mint/runtime/task_control.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <unordered_set>

#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

extern char** environ;
#endif

namespace mint::command_detail {
namespace {

#if !defined(_WIN32)

std::vector<std::string> filtered_environment() {
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

    std::vector<std::string> result;
    bool has_path = false;
    for (char** current = environ; current != nullptr && *current != nullptr; ++current) {
        const std::string entry(*current);
        const auto separator = entry.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const auto name = entry.substr(0, separator);
        if (!allowed_names.contains(name)) {
            continue;
        }
        has_path = has_path || name == "PATH";
        result.push_back(entry);
    }
    if (!has_path) {
        result.emplace_back("PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin");
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

#endif

} // namespace

ProcessResult execute_process(ProcessRequest request) {
#if defined(_WIN32)
    (void)request;
    throw std::runtime_error("当前受控命令执行暂未支持 Windows");
#else
    if (request.argv.empty()) {
        throw std::logic_error("内部命令请求缺少 argv");
    }

    auto argv = mutable_pointers(request.argv);
    auto environment_storage = filtered_environment();
    auto environment = mutable_pointers(environment_storage);

    std::array<int, 2> output_pipe{};
    if (::pipe(output_pipe.data()) != 0) {
        throw std::runtime_error("无法创建命令输出管道: " + std::string(std::strerror(errno)));
    }

    const auto read_flags = ::fcntl(output_pipe[0], F_GETFL, 0);
    if (read_flags < 0 || ::fcntl(output_pipe[0], F_SETFL, read_flags | O_NONBLOCK) != 0) {
        const auto message = std::string(std::strerror(errno));
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        throw std::runtime_error("无法配置命令输出管道: " + message);
    }

    const auto started_at = std::chrono::steady_clock::now();
    const auto process = ::fork();
    if (process < 0) {
        const auto message = std::string(std::strerror(errno));
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        throw std::runtime_error("无法启动命令进程: " + message);
    }

    if (process == 0) {
        (void)::setpgid(0, 0);
        if (::chdir(request.cwd.c_str()) != 0 || ::dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
            ::dup2(output_pipe[1], STDERR_FILENO) < 0) {
            constexpr char message[] = "mint: failed to prepare approved command\n";
            (void)::write(output_pipe[1], message, sizeof(message) - 1);
            ::_exit(126);
        }
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        close_inherited_descriptors();
        ::execve(request.executable.c_str(), argv.data(), environment.data());
        constexpr char message[] = "mint: failed to execute approved command\n";
        (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
        ::_exit(127);
    }

    ::close(output_pipe[1]);
    (void)::setpgid(process, process);

    ProcessResult result;
    result.output.reserve(std::min<std::size_t>(request.max_output_bytes, 16 * 1024));
    bool exited = false;
    int wait_status = 0;

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
        (void)::kill(-process, SIGTERM);
        const auto grace_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < grace_deadline) {
            drain_output();
            const auto grace_wait = ::waitpid(process, &wait_status, WNOHANG);
            if (grace_wait == process) {
                exited = true;
                break;
            }
            pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
            (void)::poll(&descriptor, 1, 10);
        }
        if (!exited) {
            (void)::kill(-process, SIGKILL);
            while (::waitpid(process, &wait_status, 0) < 0 && errno == EINTR) {
            }
            exited = true;
        }
    };

    const auto deadline = started_at + std::chrono::seconds(request.timeout_seconds);
    while (!exited) {
        drain_output();
        const auto waited = ::waitpid(process, &wait_status, WNOHANG);
        if (waited == process) {
            exited = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            const auto message = std::string(std::strerror(errno));
            (void)::kill(-process, SIGKILL);
            (void)::waitpid(process, &wait_status, 0);
            ::close(output_pipe[0]);
            throw std::runtime_error("等待命令进程失败: " + message);
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

    if (result.cancelled) {
        result.status = "cancelled";
    } else if (result.task_timed_out) {
        result.status = "task_timed_out";
    } else if (result.timed_out) {
        result.status = "timed_out";
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
#endif
}

} // namespace mint::command_detail
