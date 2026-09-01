#include "command_helper.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mint::test {

int run_command_helper(int argc, char** argv) {
    if (argc < 3) {
        return 64;
    }

    const std::string mode = argv[2];
    if (mode == "echo") {
        std::cout << "cwd=" << std::filesystem::current_path().generic_string() << '\n';
        for (int index = 3; index < argc; ++index) {
            std::cout << "arg=" << argv[index] << '\n';
        }
        return 0;
    }
    if (mode == "fail") {
        std::cerr << "intentional command failure\n";
        return 7;
    }
    if (mode == "sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::cout << "sleep completed\n";
        return 0;
    }
    if (mode == "spin") {
        volatile std::uint64_t accumulator = 1;
        while (true) {
            for (std::size_t iteration = 0; iteration < 1'000'000; ++iteration) {
                accumulator = accumulator * 1664525U + 1013904223U;
            }
        }
    }
    if (mode == "flood") {
        std::cout << std::string(4096, 'x');
        return 0;
    }
    if (mode == "environment") {
        if (std::getenv("MINT_TEST_SECRET") != nullptr) {
            std::cerr << "secret environment variable leaked\n";
            return 10;
        }
        std::cout << "environment filtered\n";
        return 0;
    }
    if (mode == "stdin-eof") {
        if (std::cin.get() != std::char_traits<char>::eof()) {
            std::cerr << "command inherited caller input\n";
            return 14;
        }
        std::cout << "stdin closed\n";
        return 0;
    }
    if (mode == "verify") {
        if (argc != 5) {
            return 65;
        }
        std::ifstream input(argv[3], std::ios::binary);
        const std::string content{std::istreambuf_iterator<char>(input),
                                  std::istreambuf_iterator<char>()};
        if (!input && !input.eof()) {
            std::cerr << "could not read verification target\n";
            return 8;
        }
        if (content != argv[4]) {
            std::cerr << "verification content mismatch\n";
            return 9;
        }
        std::cout << "verification passed\n";
        return 0;
    }
    if (mode == "write") {
        if (argc != 4) {
            return 65;
        }
        std::ofstream output(argv[3], std::ios::binary);
        if (!output) {
            std::cerr << "write blocked\n";
            return 11;
        }
        output << "sandbox write probe\n";
        return output ? 0 : 12;
    }
    if (mode == "write-with-parent") {
        if (argc != 4) {
            return 65;
        }
        std::error_code error;
        (void)std::filesystem::create_directories(std::filesystem::path(argv[3]).parent_path(),
                                                  error);
        if (error) {
            return 13;
        }
        std::ofstream output(argv[3], std::ios::binary);
        output << "sandbox write probe\n";
        return output ? 0 : 12;
    }
    if (mode == "write-binary") {
        if (argc != 4) {
            return 65;
        }
        std::ofstream output(argv[3], std::ios::binary);
        if (!output) {
            std::cerr << "binary write blocked\n";
            return 11;
        }
        constexpr std::array<unsigned char, 4> payload = {0x00, 0x7f, 0x80, 0xff};
        output.write(reinterpret_cast<const char*>(payload.data()),
                     static_cast<std::streamsize>(payload.size()));
        return output ? 0 : 12;
    }
#if defined(__APPLE__)
    if (mode == "sandbox-runtime") {
        if (argc != 6) {
            return 65;
        }

        std::error_code error;
        const auto workspace = std::filesystem::canonical(argv[3], error);
        std::optional<std::filesystem::path> command_temp;
        for (const auto* name : {"TMPDIR", "TMP", "TEMP"}) {
            const auto* value = std::getenv(name);
            if (value == nullptr || *value == '\0') {
                return 17;
            }
            const auto resolved = std::filesystem::canonical(value, error);
            if (error) {
                return 18;
            }
            if (command_temp.has_value() && resolved != *command_temp) {
                return 19;
            }
            command_temp = resolved;
        }
        if (error || command_temp->parent_path() != workspace ||
            command_temp->filename().string().rfind(".mint-command-tmp-", 0) != 0) {
            return 20;
        }

        {
            std::ofstream probe(*command_temp / "original-probe.txt", std::ios::binary);
            probe << "original scratch content\n";
            if (!probe) {
                return 21;
            }
        }
        const auto null_descriptor = ::open("/dev/null", O_WRONLY);
        if (null_descriptor < 0) {
            return 22;
        }
        constexpr std::string_view null_probe = "sandbox null probe\n";
        const auto written = ::write(null_descriptor, null_probe.data(), null_probe.size());
        const auto close_result = ::close(null_descriptor);
        if (written != static_cast<ssize_t>(null_probe.size()) || close_result != 0) {
            return 23;
        }

        {
            std::ofstream marker(argv[4], std::ios::binary);
            marker << command_temp->generic_string();
            if (!marker) {
                return 24;
            }
        }

        const std::string behavior = argv[5];
        if (behavior == "wait") {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            return 0;
        }
        if (behavior == "replace") {
            const auto moved = workspace / ("moved-" + command_temp->filename().generic_string());
            std::filesystem::rename(*command_temp, moved, error);
            if (error || !std::filesystem::create_directory(*command_temp, error) || error) {
                return 25;
            }
            std::ofstream replacement(*command_temp / "replacement-probe.txt", std::ios::binary);
            replacement << "replacement scratch content\n";
            return replacement ? 0 : 26;
        }
        return behavior == "complete" ? 0 : 65;
    }
#endif
#if !defined(_WIN32)
    if (mode == "write-invalid-name") {
        std::string name = "src/invalid-";
        name.push_back(static_cast<char>(0xff));
        name += ".txt";
        const auto descriptor = ::open(name.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (descriptor < 0) {
            // APFS rejects non-UTF-8 names. Force the same post-command snapshot failure there so
            // the fail-closed checkpoint path remains covered on every POSIX test host.
            std::ofstream fallback("src/invalid-name-unsupported.bin", std::ios::binary);
            fallback << std::string(4096, 'x');
            return fallback ? 0 : 11;
        }
        constexpr std::string_view contents = "invalid name\n";
        const auto written = ::write(descriptor, contents.data(), contents.size());
        const auto close_result = ::close(descriptor);
        return written == static_cast<ssize_t>(contents.size()) && close_result == 0 ? 0 : 12;
    }
    if (mode == "create-symlink") {
        if (argc != 5) {
            return 65;
        }
        std::error_code error;
        std::filesystem::create_symlink(argv[3], argv[4], error);
        return error ? 13 : 0;
    }
#endif
    if (mode == "chmod-executable") {
        if (argc != 4) {
            return 65;
        }
        std::error_code error;
        std::filesystem::permissions(argv[3], std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::add, error);
        return error ? 13 : 0;
    }
    if (mode == "create-directory") {
        if (argc != 4) {
            return 65;
        }
        std::error_code error;
        (void)std::filesystem::create_directories(argv[3], error);
        return error ? 13 : 0;
    }
    if (mode == "delete-directory") {
        if (argc != 4) {
            return 65;
        }
        std::error_code error;
        const bool removed = std::filesystem::remove(argv[3], error);
        return error || !removed ? 13 : 0;
    }
    if (mode == "write-large") {
        if (argc != 4) {
            return 65;
        }
        std::ofstream output(argv[3], std::ios::binary);
        output << std::string(4096, 'x');
        return output ? 0 : 12;
    }
    if (mode == "allocate") {
        if (argc != 4) {
            return 65;
        }
        const auto bytes = static_cast<std::size_t>(std::stoull(argv[3]));
        std::unique_ptr<unsigned char[]> memory(new unsigned char[bytes]);
        auto* const pages = static_cast<volatile unsigned char*>(memory.get());
        for (std::size_t offset = 0; offset < bytes; offset += 4096) {
            pages[offset] = 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return pages[0] == 1 ? 0 : 17;
    }
#if defined(_WIN32)
    if (mode == "network") {
        if (argc != 4) {
            return 65;
        }
        WSADATA winsock{};
        if (::WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
            return 0;
        }
        const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET) {
            (void)::WSACleanup();
            return 0;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(std::stoul(argv[3])));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const auto connected =
            ::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        (void)::closesocket(socket);
        (void)::WSACleanup();
        return connected == SOCKET_ERROR ? 0 : 14;
    }
    if (mode == "handle") {
        if (argc != 7) {
            return 65;
        }
        const auto value = static_cast<std::uintptr_t>(std::stoull(argv[3]));
        const auto handle = reinterpret_cast<HANDLE>(value);
        BY_HANDLE_FILE_INFORMATION metadata{};
        if (!::GetFileInformationByHandle(handle, &metadata)) {
            return 0;
        }
        const bool same_file = metadata.dwVolumeSerialNumber == std::stoul(argv[4]) &&
                               metadata.nFileIndexHigh == std::stoul(argv[5]) &&
                               metadata.nFileIndexLow == std::stoul(argv[6]);
        return same_file ? 15 : 0;
    }
    if (mode == "spawn") {
        std::array<wchar_t, 32768> executable{};
        const auto capacity = static_cast<DWORD>(executable.size());
        const auto length = ::GetModuleFileNameW(nullptr, executable.data(), capacity);
        if (length == 0 || length == capacity) {
            return 18;
        }
        std::wstring line = L"\"";
        line.append(executable.data(), length);
        line += L"\" --command-helper sleep";
        std::vector<wchar_t> mutable_line(line.begin(), line.end());
        mutable_line.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (!::CreateProcessW(executable.data(), mutable_line.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
            return 0;
        }
        (void)::CloseHandle(child.hThread);
        (void)::WaitForSingleObject(child.hProcess, 5000);
        (void)::CloseHandle(child.hProcess);
        return 19;
    }
#else
    if (mode == "spawn") {
        const auto child = ::fork();
        if (child < 0) {
            return 18;
        }
        if (child == 0) {
            ::execl(argv[0], argv[0], "--command-helper", "sleep", nullptr);
            ::_exit(18);
        }
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        return 19;
    }
    if (mode == "limits") {
        const auto print_limit = [](std::string_view name, int resource) {
            struct rlimit limit = {};
            if (::getrlimit(resource, &limit) != 0) {
                return false;
            }
            std::cout << name << '=' << static_cast<unsigned long long>(limit.rlim_cur) << '\n';
            return true;
        };
        const bool common = print_limit("cpu", RLIMIT_CPU) &&
                            print_limit("processes", RLIMIT_NPROC) &&
                            print_limit("file", RLIMIT_FSIZE);
#if defined(RLIMIT_AS)
        return common && print_limit("memory", RLIMIT_AS) ? 0 : 16;
#else
        return common ? 0 : 16;
#endif
    }
    if (mode == "descriptor") {
        if (argc != 6) {
            return 65;
        }
        const auto descriptor = std::stoi(argv[3]);
        const auto expected_device = std::stoull(argv[4]);
        const auto expected_inode = std::stoull(argv[5]);
        struct stat metadata = {};
        errno = 0;
        if (::fstat(descriptor, &metadata) < 0) {
            return errno == EBADF ? 0 : 15;
        }
        const auto same_file =
            static_cast<unsigned long long>(metadata.st_dev) == expected_device &&
            static_cast<unsigned long long>(metadata.st_ino) == expected_inode;
        return same_file ? 15 : 0;
    }
    if (mode == "network") {
        if (argc != 4) {
            return 65;
        }
        const auto descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor < 0) {
            return errno == EPERM || errno == EAFNOSUPPORT ? 0 : 13;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(std::stoul(argv[3])));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const auto connected =
            ::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        ::close(descriptor);
        return connected < 0 ? 0 : 14;
    }
#endif
    return 66;
}

} // namespace mint::test
