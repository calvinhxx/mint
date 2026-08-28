#include "mint/agent.hpp"
#include "mint/config.hpp"
#include "mint/domain/task_policy.hpp"
#include "mint/event_log.hpp"
#include "mint/model_client.hpp"
#include "mint/session_store.hpp"
#include "mint/task_runtime.hpp"
#include "mint/tools.hpp"

#include "agent_support.hpp"
#include "scripted_http_server.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MINT_TEST_ADDRESS_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(MINT_TEST_ADDRESS_SANITIZED)
#define MINT_TEST_ADDRESS_SANITIZED 1
#endif
#if !defined(MINT_TEST_ADDRESS_SANITIZED)
#define MINT_TEST_ADDRESS_SANITIZED 0
#endif

#if defined(_WIN32)
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

#define MINT_EXPECT(condition, message) EXPECT_TRUE(condition) << (message)

using mint::test::ScriptedHttpServer;

std::filesystem::path test_executable;

void set_secret_environment() {
#if defined(_WIN32)
    if (::_putenv_s("MINT_TEST_SECRET", "must-not-reach-child") != 0) {
        throw std::runtime_error("could not set test environment variable");
    }
#else
    if (::setenv("MINT_TEST_SECRET", "must-not-reach-child", 1) != 0) {
        throw std::runtime_error("could not set test environment variable");
    }
#endif
}

void clear_secret_environment() {
#if defined(_WIN32)
    (void)::_putenv_s("MINT_TEST_SECRET", "");
#else
    (void)::unsetenv("MINT_TEST_SECRET");
#endif
}

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
            struct rlimit limit{};
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
        struct stat metadata{};
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

class TemporaryDirectory final {
  public:
    explicit TemporaryDirectory(
        std::filesystem::path base = std::filesystem::temp_directory_path()) {
        static std::atomic<std::uint64_t> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
        const auto process_id = static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
        const auto process_id = static_cast<std::uint64_t>(::getpid());
#endif
        const auto suffix = std::to_string(process_id) + '-' + std::to_string(stamp) + '-' +
                            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        path_ = std::move(base) / ("mint-tests-" + suffix);
        std::filesystem::create_directories(path_ / "workspace" / "src");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

#if defined(_WIN32)
class LoopbackListener final {
  public:
    LoopbackListener() {
        if (::WSAStartup(MAKEWORD(2, 2), &winsock_) != 0) {
            throw std::runtime_error("sandbox test could not initialize Winsock");
        }
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) {
            (void)::WSACleanup();
            throw std::runtime_error("sandbox test could not create loopback listener");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
                SOCKET_ERROR ||
            ::listen(socket_, 1) == SOCKET_ERROR) {
            (void)::closesocket(socket_);
            (void)::WSACleanup();
            throw std::runtime_error("sandbox test could not bind loopback listener");
        }
        int length = sizeof(address);
        if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) ==
            SOCKET_ERROR) {
            (void)::closesocket(socket_);
            (void)::WSACleanup();
            throw std::runtime_error("sandbox test could not inspect loopback listener");
        }
        port_ = ntohs(address.sin_port);
    }

    ~LoopbackListener() {
        if (socket_ != INVALID_SOCKET) {
            (void)::closesocket(socket_);
        }
        (void)::WSACleanup();
    }

    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

  private:
    WSADATA winsock_{};
    SOCKET socket_ = INVALID_SOCKET;
    std::uint16_t port_ = 0;
};
#else
class LoopbackListener final {
  public:
    LoopbackListener() {
        descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor_ < 0) {
            throw std::runtime_error("sandbox test could not create loopback listener");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(descriptor_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
                0 ||
            ::listen(descriptor_, 1) != 0) {
            ::close(descriptor_);
            throw std::runtime_error("sandbox test could not bind loopback listener");
        }
        socklen_t length = sizeof(address);
        if (::getsockname(descriptor_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(descriptor_);
            throw std::runtime_error("sandbox test could not inspect loopback listener");
        }
        port_ = ntohs(address.sin_port);
    }

    ~LoopbackListener() {
        ::close(descriptor_);
    }

    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

  private:
    int descriptor_ = -1;
    std::uint16_t port_ = 0;
};

#endif

void write_text(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("test setup could not create " + path.string());
    }
    output << content;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("test could not read " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

template <typename Callable> void expect_failure(Callable&& callable, const std::string& message) {
    EXPECT_ANY_THROW(callable()) << message;
}

bool has_entry(const mint::Json& entries, const std::string& path) {
    for (const auto& entry : entries) {
        if (entry.value("path", "") == path) {
            return true;
        }
    }
    return false;
}

class ScriptedModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        MINT_EXPECT(tools.size() == 3, "agent exposes exactly three tools in v0.1");
        if (calls_++ == 0) {
            const mint::Json arguments = {{"path", "README.md"}};
            const mint::Json raw_call = {
                {"id", "test-read"},
                {"type", "function"},
                {"function", {{"name", "read_file"}, {"arguments", arguments.dump()}}}};
            return {.assistant_message = {{"role", "assistant"},
                                          {"content", nullptr},
                                          {"tool_calls", mint::Json::array({raw_call})}},
                    .text = {},
                    .tool_calls = {{"test-read", "read_file", arguments}}};
        }

        MINT_EXPECT(messages.back().at("role") == "tool", "tool result is appended to context");
        const auto result = mint::Json::parse(messages.back().at("content").get<std::string>());
        MINT_EXPECT(result.at("ok").get<bool>(), "tool result returned to model is successful");
        return {.assistant_message = {{"role", "assistant"}, {"content", "完成"}},
                .text = "完成",
                .tool_calls = {},
                .usage = {.available = true,
                          .prompt_tokens = 100,
                          .completion_tokens = 10,
                          .total_tokens = 110,
                          .cached_tokens = 80}};
    }

  private:
    int calls_ = 0;
};

class ContextBudgetModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json& messages, const mint::Json&) override {
        MINT_EXPECT(messages.dump().size() <= 16 * 1024,
                    "model context never exceeds configured byte budget");
        if (calls_ > 0) {
            bool found_summary = false;
            for (const auto& message : messages) {
                if (message.value("role", "") == "system" &&
                    message.value("content", "").find("Harness context summary") !=
                        std::string::npos) {
                    found_summary = true;
                }
            }
            MINT_EXPECT(found_summary, "compacted context carries an explicit harness summary");
        }
        if (calls_++ < 3) {
            const auto id = "large-read-" + std::to_string(calls_);
            const mint::Json arguments = {{"path", "large.txt"}};
            const mint::Json raw_call = {
                {"id", id},
                {"type", "function"},
                {"function", {{"name", "read_file"}, {"arguments", arguments.dump()}}}};
            return {.assistant_message = {{"role", "assistant"},
                                          {"content", nullptr},
                                          {"tool_calls", mint::Json::array({raw_call})}},
                    .text = {},
                    .tool_calls = {{id, "read_file", arguments}}};
        }
        return {.assistant_message = {{"role", "assistant"}, {"content", "上下文预算通过"}},
                .text = "上下文预算通过",
                .tool_calls = {}};
    }

  private:
    int calls_ = 0;
};

class WritingModel final : public mint::ModelClient {
  public:
    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        MINT_EXPECT(tools.size() == 6, "write-enabled agent exposes six tools in v1.2");
        MINT_EXPECT(messages.at(0).at("content").get<std::string>().find("apply_patch") !=
                        std::string::npos,
                    "write-enabled system prompt explains apply_patch");

        if (calls_++ == 0) {
            const mint::Json arguments = {{"path", "README.md"},
                                          {"operation", "replace"},
                                          {"old_text", "# Before\n"},
                                          {"new_text", "# After\n"}};
            const mint::Json raw_call = {
                {"id", "test-patch"},
                {"type", "function"},
                {"function", {{"name", "apply_patch"}, {"arguments", arguments.dump()}}}};
            return {.assistant_message = {{"role", "assistant"},
                                          {"content", nullptr},
                                          {"tool_calls", mint::Json::array({raw_call})}},
                    .text = {},
                    .tool_calls = {{"test-patch", "apply_patch", arguments}}};
        }

        MINT_EXPECT(messages.back().at("role") == "tool", "patch result is appended to context");
        const auto result = mint::Json::parse(messages.back().at("content").get<std::string>());
        MINT_EXPECT(result.at("ok").get<bool>(), "patch result returned to model is successful");
        return {.assistant_message = {{"role", "assistant"}, {"content", "修改完成"}},
                .text = "修改完成",
                .tool_calls = {}};
    }

  private:
    int calls_ = 0;
};

class PatchThenVerifyModel final : public mint::ModelClient {
  public:
    explicit PatchThenVerifyModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        MINT_EXPECT(tools.size() == 7, "write-and-command agent exposes seven tools in v1.2");
        const auto system_prompt = messages.at(0).at("content").get<std::string>();
        MINT_EXPECT(system_prompt.find("apply_patch") != std::string::npos,
                    "validation system prompt explains apply_patch");
        MINT_EXPECT(system_prompt.find("run_command") != std::string::npos,
                    "validation system prompt explains run_command");

        if (calls_ == 0) {
            ++calls_;
            return tool_reply("e2e-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Broken\n"},
                               {"new_text", "# Fixed\n"}});
        }

        const auto previous = mint::Json::parse(messages.back().at("content").get<std::string>());
        MINT_EXPECT(previous.at("ok").get<bool>(), "previous e2e tool result succeeded");

        if (calls_ == 1) {
            ++calls_;
            return tool_reply("e2e-verify", "run_command",
                              {{"program", program_},
                               {"args", mint::Json::array({"--command-helper", "verify",
                                                           "README.md", "# Fixed\n"})},
                               {"cwd", "."},
                               {"timeout_seconds", 5}});
        }

        MINT_EXPECT(previous.at("status") == "exited", "verification command exited normally");
        MINT_EXPECT(previous.at("exit_code") == 0, "verification command passed");
        MINT_EXPECT(previous.at("output").get<std::string>().find("verification passed") !=
                        std::string::npos,
                    "verification evidence is returned to the model");
        ++calls_;
        return {.assistant_message = {{"role", "assistant"}, {"content", "修改并验证完成"}},
                .text = "修改并验证完成",
                .tool_calls = {}};
    }

  private:
    static mint::ModelReply tool_reply(std::string id, std::string name, mint::Json arguments) {
        mint::Json raw_call = {{"id", id},
                               {"type", "function"},
                               {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
        mint::ModelReply reply;
        reply.assistant_message = {{"role", "assistant"},
                                   {"content", nullptr},
                                   {"tool_calls", mint::Json::array({raw_call})}};
        reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
        return reply;
    }

    std::string program_;
    int calls_ = 0;
};

class FailureThenRepairModel final : public mint::ModelClient {
  public:
    explicit FailureThenRepairModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json& messages, const mint::Json& tools) override {
        MINT_EXPECT(tools.size() == 7, "verification-gated agent exposes seven tools");
        const auto system_prompt = messages.at(0).at("content").get<std::string>();
        MINT_EXPECT(system_prompt.find("Harness policy requires verification") != std::string::npos,
                    "system prompt explains the required verification gate");

        switch (calls_++) {
        case 0:
            return tool_reply("retry-first-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Broken\n"},
                               {"new_text", "# Almost\n"}});
        case 1:
            MINT_EXPECT(last_tool_result(messages).at("ok").get<bool>(),
                        "first patch succeeded before failed verification");
            return verification_call("retry-first-verify");
        case 2: {
            const auto failed = last_tool_result(messages);
            MINT_EXPECT(failed.at("exit_code") == 9,
                        "first verification exposes the expected failure");
            return final_reply("错误地提前结束");
        }
        case 3:
            MINT_EXPECT(messages.back().at("role") == "user",
                        "harness gate appends a continuation requirement");
            MINT_EXPECT(messages.back().at("content").get<std::string>().find(
                            "unverified changes") != std::string::npos,
                        "continuation requirement explains unverified changes");
            gate_seen_ = true;
            return tool_reply("retry-second-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Almost\n"},
                               {"new_text", "# Fixed\n"}});
        case 4:
            MINT_EXPECT(last_tool_result(messages).at("ok").get<bool>(),
                        "second patch succeeded after the gate");
            return verification_call("retry-second-verify");
        default: {
            const auto passed = last_tool_result(messages);
            MINT_EXPECT(passed.at("exit_code") == 0, "second verification passes after the repair");
            return final_reply("失败后继续修复并验证完成");
        }
        }
    }

    [[nodiscard]] bool gate_seen() const noexcept {
        return gate_seen_;
    }

  private:
    static mint::Json last_tool_result(const mint::Json& messages) {
        MINT_EXPECT(messages.back().at("role") == "tool",
                    "script expects a tool result as the latest message");
        return mint::Json::parse(messages.back().at("content").get<std::string>());
    }

    static mint::ModelReply tool_reply(std::string id, std::string name, mint::Json arguments) {
        mint::Json raw_call = {{"id", id},
                               {"type", "function"},
                               {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
        mint::ModelReply reply;
        reply.assistant_message = {{"role", "assistant"},
                                   {"content", nullptr},
                                   {"tool_calls", mint::Json::array({raw_call})}};
        reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
        return reply;
    }

    mint::ModelReply verification_call(std::string id) const {
        return tool_reply(
            std::move(id), "run_command",
            {{"program", program_},
             {"args", mint::Json::array({"--command-helper", "verify", "README.md", "# Fixed\n"})},
             {"cwd", "."},
             {"timeout_seconds", 5}});
    }

    static mint::ModelReply final_reply(std::string text) {
        return {.assistant_message = {{"role", "assistant"}, {"content", text}},
                .text = std::move(text),
                .tool_calls = {}};
    }

    std::string program_;
    int calls_ = 0;
    bool gate_seen_ = false;
};

class PassThenFailModel final : public mint::ModelClient {
  public:
    explicit PassThenFailModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json& messages, const mint::Json&) override {
        switch (calls_++) {
        case 0:
            return tool_reply("regression-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Broken\n"},
                               {"new_text", "# Fixed\n"}});
        case 1:
            return tool_reply("regression-pass", "run_command",
                              {{"program", program_},
                               {"args", mint::Json::array({"--command-helper", "verify",
                                                           "README.md", "# Fixed\n"})},
                               {"cwd", "."},
                               {"timeout_seconds", 5}});
        case 2:
            MINT_EXPECT(last_tool_result(messages).at("exit_code") == 0,
                        "initial verification passes before the later failure");
            return tool_reply("regression-fail", "run_command",
                              {{"program", program_},
                               {"args", mint::Json::array({"--command-helper", "fail"})},
                               {"cwd", "."},
                               {"timeout_seconds", 5}});
        default:
            MINT_EXPECT(last_tool_result(messages).at("exit_code") == 7,
                        "later command exposes the regression failure");
            return {.assistant_message = {{"role", "assistant"}, {"content", "错误地忽略后续失败"}},
                    .text = "错误地忽略后续失败",
                    .tool_calls = {}};
        }
    }

  private:
    static mint::Json last_tool_result(const mint::Json& messages) {
        MINT_EXPECT(messages.back().at("role") == "tool",
                    "regression script expects the latest tool result");
        return mint::Json::parse(messages.back().at("content").get<std::string>());
    }

    static mint::ModelReply tool_reply(std::string id, std::string name, mint::Json arguments) {
        mint::Json raw_call = {{"id", id},
                               {"type", "function"},
                               {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
        mint::ModelReply reply;
        reply.assistant_message = {{"role", "assistant"},
                                   {"content", nullptr},
                                   {"tool_calls", mint::Json::array({raw_call})}};
        reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
        return reply;
    }

    std::string program_;
    int calls_ = 0;
};

mint::ModelReply model_tool_reply(std::string id, std::string name, mint::Json arguments) {
    mint::Json raw_call = {{"id", id},
                           {"type", "function"},
                           {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
    mint::ModelReply reply;
    reply.assistant_message = {
        {"role", "assistant"}, {"content", nullptr}, {"tool_calls", mint::Json::array({raw_call})}};
    reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
    return reply;
}

class StoppingSessionPatchModel final : public mint::ModelClient {
  public:
    explicit StoppingSessionPatchModel(std::shared_ptr<mint::TaskControl> control = {})
        : control_(std::move(control)) {}

    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        if (control_ != nullptr) {
            control_->request_cancel();
        }
        return model_tool_reply("session-patch", "apply_patch",
                                {{"path", "README.md"},
                                 {"operation", "replace"},
                                 {"old_text", "# Broken\n"},
                                 {"new_text", "# Fixed\n"}});
    }

  private:
    std::shared_ptr<mint::TaskControl> control_;
};

class ResumeVerificationModel final : public mint::ModelClient {
  public:
    explicit ResumeVerificationModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json& messages, const mint::Json&) override {
        MINT_EXPECT(messages.back().at("role") == "tool",
                    "resumed model receives the restored pending tool result");
        const auto result = mint::Json::parse(messages.back().at("content").get<std::string>());
        if (calls_++ == 0) {
            MINT_EXPECT(result.at("ok").get<bool>(), "restored patch succeeds before verification");
            return model_tool_reply("session-verify", "run_command",
                                    {{"program", program_},
                                     {"args", mint::Json::array({"--command-helper", "verify",
                                                                 "README.md", "# Fixed\n"})},
                                     {"cwd", "."},
                                     {"timeout_seconds", 5}});
        }
        MINT_EXPECT(result.at("exit_code") == 0, "resumed verification command passes");
        return {.assistant_message = {{"role", "assistant"}, {"content", "恢复后验证完成"}},
                .text = "恢复后验证完成",
                .tool_calls = {}};
    }

  private:
    std::string program_;
    int calls_ = 0;
};

class LongCommandModel final : public mint::ModelClient {
  public:
    explicit LongCommandModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        if (calls_++ != 0) {
            throw std::runtime_error("cancelled agent must not call the model again");
        }
        return model_tool_reply("cancel-command", "run_command",
                                {{"program", program_},
                                 {"args", mint::Json::array({"--command-helper", "sleep"})},
                                 {"cwd", "."},
                                 {"timeout_seconds", 5}});
    }

  private:
    std::string program_;
    int calls_ = 0;
};

class DeniedVerificationModel final : public mint::ModelClient {
  public:
    explicit DeniedVerificationModel(std::string program) : program_(std::move(program)) {}

    mint::ModelReply complete(const mint::Json&, const mint::Json&) override {
        switch (calls_++) {
        case 0:
            return model_tool_reply("denied-patch", "apply_patch",
                                    {{"path", "README.md"},
                                     {"operation", "replace"},
                                     {"old_text", "# Broken\n"},
                                     {"new_text", "# Fixed\n"}});
        case 1:
            return model_tool_reply("denied-verify", "run_command",
                                    {{"program", program_},
                                     {"args", mint::Json::array({"--command-helper", "verify",
                                                                 "README.md", "# Fixed\n"})},
                                     {"cwd", "."},
                                     {"timeout_seconds", 5}});
        default:
            return {
                .assistant_message = {{"role", "assistant"}, {"content", "错误地把拒绝当成通过"}},
                .text = "错误地把拒绝当成通过",
                .tool_calls = {}};
        }
    }

  private:
    std::string program_;
    int calls_ = 0;
};

TEST(ToolRegistryTest, ReadOnlyTools) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    std::filesystem::create_directories(workspace / ".git");
    write_text(workspace / "README.md", "# Demo Agent\nA tiny project.\n");
    write_text(workspace / "src" / "main.cpp", "int main() { return 0; }\n");
    write_text(workspace / "large.txt", std::string(24 * 1024, 'x'));
    write_text(workspace / "config.json", R"({"api_key":"must-not-leak"})");
    write_text(workspace / ".git" / "config", "token=must-not-leak\n");
    write_text(temporary.path() / "outside.txt", "secret\n");

    mint::ToolRegistry tools(
        workspace, mint::ToolRegistryOptions{.protected_paths = {workspace / "config.json"}});

    const auto listed =
        mint::Json::parse(tools.execute({"list", "list_files", {{"path", "."}, {"max_depth", 2}}}));
    MINT_EXPECT(listed.at("ok").get<bool>(), "list_files succeeds");
    MINT_EXPECT(has_entry(listed.at("entries"), "README.md"), "list_files sees README.md");
    MINT_EXPECT(has_entry(listed.at("entries"), "src/main.cpp"), "list_files respects depth");
    MINT_EXPECT(!has_entry(listed.at("entries"), "config.json"),
                "list_files hides the protected config file");
    MINT_EXPECT(!has_entry(listed.at("entries"), ".git"),
                "list_files hides ignored metadata directories");

    const auto direct_git_list =
        mint::Json::parse(tools.execute({"git-list", "list_files", {{"path", ".git"}}}));
    MINT_EXPECT(!direct_git_list.at("ok").get<bool>(),
                "list_files rejects a directly requested ignored directory");

    const auto read =
        mint::Json::parse(tools.execute({"read", "read_file", {{"path", "README.md"}}}));
    MINT_EXPECT(read.at("ok").get<bool>(), "read_file succeeds");
    MINT_EXPECT(read.at("content").get<std::string>().find("Demo Agent") != std::string::npos,
                "read_file returns text");

    const auto first_chunk =
        mint::Json::parse(tools.execute({"large-first", "read_file", {{"path", "large.txt"}}}));
    MINT_EXPECT(first_chunk.at("ok").get<bool>() &&
                    first_chunk.at("content").get<std::string>().size() == 16 * 1024 &&
                    first_chunk.at("truncated").get<bool>(),
                "read_file defaults to a bounded 16 KiB chunk");
    const auto second_chunk =
        mint::Json::parse(tools.execute({"large-second",
                                         "read_file",
                                         {{"path", "large.txt"},
                                          {"offset", first_chunk.at("next_offset")},
                                          {"max_bytes", 16 * 1024}}}));
    MINT_EXPECT(second_chunk.at("ok").get<bool>() && !second_chunk.at("truncated").get<bool>() &&
                    second_chunk.at("content").get<std::string>().size() == 8 * 1024,
                "read_file continues from next_offset without resending the first chunk");

    const auto protected_config =
        mint::Json::parse(tools.execute({"config", "read_file", {{"path", "config.json"}}}));
    MINT_EXPECT(!protected_config.at("ok").get<bool>(), "read_file rejects protected config");

    const auto git_config =
        mint::Json::parse(tools.execute({"git-config", "read_file", {{"path", ".git/config"}}}));
    MINT_EXPECT(!git_config.at("ok").get<bool>(),
                "read_file rejects a direct path inside ignored metadata");

    const auto searched = mint::Json::parse(tools.execute(
        {"search", "search_text", {{"path", "."}, {"query", "agent"}, {"case_sensitive", false}}}));
    MINT_EXPECT(searched.at("ok").get<bool>(), "search_text succeeds");
    MINT_EXPECT(searched.at("hits").size() == 1, "search_text finds one case-insensitive hit");
    MINT_EXPECT(searched.at("hits").at(0).at("line") == 1, "search_text reports line number");

    const auto secret_search = mint::Json::parse(
        tools.execute({"secret-search",
                       "search_text",
                       {{"path", "."}, {"query", "must-not-leak"}, {"case_sensitive", true}}}));
    MINT_EXPECT(secret_search.at("hits").empty(), "search_text skips protected config");

    const auto git_search = mint::Json::parse(tools.execute(
        {"git-search", "search_text", {{"path", ".git"}, {"query", "must-not-leak"}}}));
    MINT_EXPECT(!git_search.at("ok").get<bool>(),
                "search_text rejects a direct ignored metadata path");

    const auto escaped =
        mint::Json::parse(tools.execute({"escape", "read_file", {{"path", "../outside.txt"}}}));
    MINT_EXPECT(!escaped.at("ok").get<bool>(), "path traversal is rejected");

    std::error_code symlink_error;
    std::filesystem::create_symlink(temporary.path() / "outside.txt", workspace / "outside-link",
                                    symlink_error);
    if (!symlink_error) {
        const auto followed_symlink =
            mint::Json::parse(tools.execute({"symlink", "read_file", {{"path", "outside-link"}}}));
        MINT_EXPECT(!followed_symlink.at("ok").get<bool>(), "escaping symlink is rejected");
    }
}

TEST(ToolRegistryTest, ConfigurableRuntimeLimits) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "large.txt", std::string(4096, 'x'));
    write_text(workspace / "one.txt", "needle one\nneedle two\n");
    write_text(workspace / "two.txt", "needle three\n");

    mint::ToolRuntimeSettings limits;
    limits.read_file_bytes = 1024;
    limits.list_max_entries = 2;
    limits.search_file_bytes = 4096;
    limits.search_max_hits = 1;
    limits.search_max_files = 2;
    limits.command_output_bytes = 512;
    mint::ToolRegistry tools(workspace, mint::ToolRegistryOptions{.runtime = limits});

    const auto read =
        mint::Json::parse(tools.execute({"read", "read_file", {{"path", "large.txt"}}}));
    MINT_EXPECT(read.at("content").get<std::string>().size() == limits.read_file_bytes &&
                    read.at("truncated").get<bool>(),
                "policy-configured read_file chunk size is applied");

    const auto listed =
        mint::Json::parse(tools.execute({"list", "list_files", {{"path", "."}, {"max_depth", 1}}}));
    MINT_EXPECT(listed.at("entries").size() == limits.list_max_entries &&
                    listed.at("truncated").get<bool>(),
                "policy-configured list entry budget is applied");

    const auto searched = mint::Json::parse(
        tools.execute({"search", "search_text", {{"path", "."}, {"query", "NEEDLE"}}}));
    MINT_EXPECT(searched.at("hits").size() == limits.search_max_hits &&
                    searched.at("truncated").get<bool>(),
                "policy-configured search budget is applied with allocation-free ASCII matching");

    std::filesystem::create_directories(workspace / "oversized");
    write_text(workspace / "oversized" / "one.txt", std::string(2048, 'a'));
    write_text(workspace / "oversized" / "two.txt", std::string(2048, 'b'));
    write_text(workspace / "oversized" / "three.txt", std::string(2048, 'c'));
    auto scan_limits = limits;
    scan_limits.search_file_bytes = 1024;
    scan_limits.search_max_hits = 10;
    mint::ToolRegistry bounded_search(workspace, mint::ToolRegistryOptions{.runtime = scan_limits});
    const auto bounded = mint::Json::parse(bounded_search.execute(
        {"bounded-search", "search_text", {{"path", "oversized"}, {"query", "missing"}}}));
    MINT_EXPECT(bounded.at("scanned_files") == scan_limits.search_max_files &&
                    bounded.at("truncated").get<bool>(),
                "search file budget also caps oversized candidates before opening them");

    const auto definitions = tools.definitions().dump();
    MINT_EXPECT(definitions.find("Defaults to 1024") != std::string::npos,
                "tool schema reports the configured read chunk default");

    mint::ToolRegistry legacy_limit(workspace,
                                    mint::ToolRegistryOptions{.max_command_output_bytes = 512});
    MINT_EXPECT(legacy_limit.runtime_settings().command_output_bytes == 512,
                "legacy command output option maps into the unified runtime settings");
    expect_failure(
        [&] {
            auto conflicting = limits;
            conflicting.command_output_bytes = 1024;
            (void)mint::ToolRegistry(
                workspace,
                mint::ToolRegistryOptions{.max_command_output_bytes = 512, .runtime = conflicting});
        },
        "conflicting legacy and unified command output limits are rejected");

    auto invalid = limits;
    invalid.search_max_hits = 0;
    expect_failure(
        [&] { (void)mint::ToolRegistry(workspace, mint::ToolRegistryOptions{.runtime = invalid}); },
        "programmatic tool limits are validated before use");
}

TEST(ToolRegistryTest, ApplyPatch) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    std::filesystem::create_directories(workspace / ".git");
    write_text(workspace / "README.md", "alpha\nbeta\n");
    write_text(workspace / "duplicate.txt", "repeat\nrepeat\n");
    std::string invalid_utf8 = "bad";
    invalid_utf8.push_back(static_cast<char>(0xC3));
    invalid_utf8.push_back('(');
    write_text(workspace / "invalid.txt", invalid_utf8);
    write_text(workspace / "config.json", R"({"api_key":"must-stay-secret"})");
    write_text(workspace / ".git" / "config", "token=must-stay-secret\n");
    write_text(temporary.path() / "outside.txt", "outside\n");

    const mint::ToolCall replace_readme{"patch-readme",
                                        "apply_patch",
                                        {{"path", "README.md"},
                                         {"operation", "replace"},
                                         {"old_text", "beta\n"},
                                         {"new_text", "gamma\n"}}};

    mint::ToolRegistry read_only_tools(workspace);
    MINT_EXPECT(read_only_tools.definitions().size() == 3,
                "write tool is hidden unless explicitly enabled");
    const auto disabled = mint::Json::parse(read_only_tools.execute(replace_readme));
    MINT_EXPECT(!disabled.at("ok").get<bool>(), "apply_patch rejects missing write authorization");
    MINT_EXPECT(read_text(workspace / "README.md") == "alpha\nbeta\n",
                "disabled apply_patch leaves the file unchanged");
    const auto disabled_changes = mint::Json::parse(
        read_only_tools.execute({"disabled-changes", "workspace_changes", mint::Json::object()}));
    MINT_EXPECT(!disabled_changes.at("ok").get<bool>(),
                "workspace_changes is unavailable without write authorization");

    mint::ToolRegistry tools(
        workspace, mint::ToolRegistryOptions{.protected_paths = {workspace / "config.json"},
                                             .allow_write = true});
    MINT_EXPECT(tools.definitions().size() == 6,
                "write-enabled registry exposes patch, changeset and workspace changes");

    const auto replaced = mint::Json::parse(tools.execute(replace_readme));
    MINT_EXPECT(replaced.at("ok").get<bool>(), "apply_patch replaces one exact block");
    MINT_EXPECT(read_text(workspace / "README.md") == "alpha\ngamma\n",
                "replace writes the expected contents");

    const auto created =
        mint::Json::parse(tools.execute({"patch-create",
                                         "apply_patch",
                                         {{"path", "src/generated.cpp"},
                                          {"operation", "create"},
                                          {"new_text", "int generated() { return 42; }\n"}}}));
    MINT_EXPECT(created.at("ok").get<bool>(), "apply_patch creates a new text file");
    MINT_EXPECT(read_text(workspace / "src" / "generated.cpp") ==
                    "int generated() { return 42; }\n",
                "create writes the expected contents");

    const auto changes =
        mint::Json::parse(tools.execute({"changes", "workspace_changes", mint::Json::object()}));
    MINT_EXPECT(changes.at("ok").get<bool>(), "workspace_changes succeeds");
    MINT_EXPECT(changes.at("changed_files").size() == 2,
                "workspace_changes reports modified and created files");
    const auto diff = changes.at("diff").get<std::string>();
    MINT_EXPECT(diff.find("--- a/README.md") != std::string::npos,
                "workspace_changes emits a modified-file header");
    MINT_EXPECT(diff.find("-beta\n+gamma\n") != std::string::npos,
                "workspace_changes emits the exact text replacement");
    MINT_EXPECT(diff.find("--- /dev/null\n+++ b/src/generated.cpp") != std::string::npos,
                "workspace_changes emits a created-file header");
    MINT_EXPECT(!changes.at("diff_truncated").get<bool>(), "small workspace diff is not truncated");

    const auto overwrite = mint::Json::parse(tools.execute(
        {"patch-overwrite",
         "apply_patch",
         {{"path", "README.md"}, {"operation", "create"}, {"new_text", "overwritten\n"}}}));
    MINT_EXPECT(!overwrite.at("ok").get<bool>(), "create never overwrites an existing file");

    const auto ambiguous = mint::Json::parse(tools.execute({"patch-ambiguous",
                                                            "apply_patch",
                                                            {{"path", "duplicate.txt"},
                                                             {"operation", "replace"},
                                                             {"old_text", "repeat"},
                                                             {"new_text", "changed"}}}));
    MINT_EXPECT(!ambiguous.at("ok").get<bool>(), "replace rejects an ambiguous old_text");
    MINT_EXPECT(read_text(workspace / "duplicate.txt") == "repeat\nrepeat\n",
                "ambiguous replacement leaves the file unchanged");

    const auto stale = mint::Json::parse(tools.execute({"patch-stale",
                                                        "apply_patch",
                                                        {{"path", "README.md"},
                                                         {"operation", "replace"},
                                                         {"old_text", "not present"},
                                                         {"new_text", "changed"}}}));
    MINT_EXPECT(!stale.at("ok").get<bool>(), "replace detects stale file context");

    const auto invalid_encoding = mint::Json::parse(tools.execute({"patch-invalid-utf8",
                                                                   "apply_patch",
                                                                   {{"path", "invalid.txt"},
                                                                    {"operation", "replace"},
                                                                    {"old_text", "bad"},
                                                                    {"new_text", "good"}}}));
    MINT_EXPECT(!invalid_encoding.at("ok").get<bool>(),
                "apply_patch rejects a non-UTF-8 source before writing");
    MINT_EXPECT(read_text(workspace / "invalid.txt") == invalid_utf8,
                "encoding rejection leaves the original bytes unchanged");

    const auto protected_config =
        mint::Json::parse(tools.execute({"patch-config",
                                         "apply_patch",
                                         {{"path", "config.json"},
                                          {"operation", "replace"},
                                          {"old_text", "must-stay-secret"},
                                          {"new_text", "leaked"}}}));
    MINT_EXPECT(!protected_config.at("ok").get<bool>(),
                "apply_patch rejects the protected config file");
    MINT_EXPECT(read_text(workspace / "config.json").find("must-stay-secret") != std::string::npos,
                "protected config remains unchanged");

    const auto git_config = mint::Json::parse(tools.execute({"patch-git-config",
                                                             "apply_patch",
                                                             {{"path", ".git/config"},
                                                              {"operation", "replace"},
                                                              {"old_text", "must-stay-secret"},
                                                              {"new_text", "leaked"}}}));
    MINT_EXPECT(!git_config.at("ok").get<bool>(),
                "apply_patch rejects ignored repository metadata");
    MINT_EXPECT(read_text(workspace / ".git" / "config").find("must-stay-secret") !=
                    std::string::npos,
                "ignored repository metadata remains unchanged");

    const auto escaped = mint::Json::parse(tools.execute({"patch-escape",
                                                          "apply_patch",
                                                          {{"path", "../outside.txt"},
                                                           {"operation", "replace"},
                                                           {"old_text", "outside"},
                                                           {"new_text", "escaped"}}}));
    MINT_EXPECT(!escaped.at("ok").get<bool>(), "apply_patch rejects path traversal");
    MINT_EXPECT(read_text(temporary.path() / "outside.txt") == "outside\n",
                "path traversal leaves outside files unchanged");

    const auto unsupported = mint::Json::parse(
        tools.execute({"patch-delete",
                       "apply_patch",
                       {{"path", "README.md"}, {"operation", "delete"}, {"new_text", ""}}}));
    MINT_EXPECT(!unsupported.at("ok").get<bool>(), "v0.2 does not allow file deletion");

    std::error_code symlink_error;
    std::filesystem::create_symlink(temporary.path() / "outside.txt", workspace / "write-link",
                                    symlink_error);
    if (!symlink_error) {
        const auto symlink = mint::Json::parse(tools.execute({"patch-symlink",
                                                              "apply_patch",
                                                              {{"path", "write-link"},
                                                               {"operation", "replace"},
                                                               {"old_text", "outside"},
                                                               {"new_text", "escaped"}}}));
        MINT_EXPECT(!symlink.at("ok").get<bool>(), "apply_patch rejects symbolic links");
        MINT_EXPECT(read_text(temporary.path() / "outside.txt") == "outside\n",
                    "symbolic link rejection leaves outside files unchanged");
    }

    const auto journal_state = tools.workspace_change_state();
    mint::ToolRegistry restored_tools(workspace, mint::ToolRegistryOptions{.allow_write = true});
    restored_tools.restore_workspace_change_state(journal_state);
    MINT_EXPECT(restored_tools.workspace_change_snapshot().at("changed_files").size() == 2,
                "change journal restores both stable file entries");

    write_text(workspace / "README.md", "externally changed\n");
    bool rejected_stale_session = false;
    try {
        mint::ToolRegistry stale_tools(workspace, mint::ToolRegistryOptions{.allow_write = true});
        stale_tools.restore_workspace_change_state(journal_state);
    } catch (const std::invalid_argument& error) {
        rejected_stale_session = std::string(error.what()).find("外部修改") != std::string::npos;
    }
    MINT_EXPECT(rejected_stale_session,
                "change journal restore rejects files modified after the checkpoint");
}

TEST(ToolRegistryTest, ApplyChangeSet) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    std::filesystem::create_directories(workspace / "src");
    std::filesystem::create_directories(workspace / ".git");
    write_text(workspace / "src" / "alpha.txt", "alpha\n");
    write_text(workspace / "src" / "delete.txt", "delete me\n");
    write_text(workspace / "src" / "move.txt", "move me\n");
    write_text(workspace / ".git" / "config", "protected\n");

    bool approval_seen = false;
    mint::ToolRegistry tools(
        workspace, mint::ToolRegistryOptions{
                       .allow_write = true,
                       .allowed_write_paths = {"src"},
                       .change_set_approval = [&](const mint::ChangeSetApprovalRequest& request) {
                           approval_seen =
                               request.paths.size() == 5 &&
                               request.unified_diff.find("src/alpha.txt") != std::string::npos &&
                               request.unified_diff.find("src/moved.txt") != std::string::npos;
                           return true;
                       }});

    const auto rejected_extra_field = mint::Json::parse(tools.execute(
        {"changeset-extra-field",
         "apply_changeset",
         {{"changes", mint::Json::array({{{"operation", "create"},
                                          {"path", "src/extra.txt"},
                                          {"new_text", "new\n"},
                                          {"old_text", "not valid for create"}}})}}}));
    MINT_EXPECT(!rejected_extra_field.at("ok").get<bool>() &&
                    !std::filesystem::exists(workspace / "src" / "extra.txt"),
                "changeset rejects operation fields outside the exact operation contract");

    const auto committed = mint::Json::parse(tools.execute(
        {"changeset-commit",
         "apply_changeset",
         {{"changes",
           mint::Json::array(
               {{{"operation", "replace"},
                 {"path", "src/alpha.txt"},
                 {"old_text", "alpha\n"},
                 {"new_text", "beta\n"}},
                {{"operation", "create"}, {"path", "src/new.txt"}, {"new_text", "new\n"}},
                {{"operation", "delete"}, {"path", "src/delete.txt"}, {"old_text", "delete me\n"}},
                {{"operation", "move"},
                 {"path", "src/move.txt"},
                 {"destination", "src/moved.txt"},
                 {"old_text", "move me\n"}}})}}}));
    MINT_EXPECT(committed.at("ok").get<bool>() && committed.at("status") == "committed" &&
                    committed.at("operation_count") == 4,
                "apply_changeset commits four validated operations together");
    MINT_EXPECT(approval_seen, "changeset approval receives a bounded five-file diff preview");
    MINT_EXPECT(read_text(workspace / "src" / "alpha.txt") == "beta\n" &&
                    read_text(workspace / "src" / "new.txt") == "new\n",
                "changeset replace and create write exact content");
    MINT_EXPECT(!std::filesystem::exists(workspace / "src" / "delete.txt") &&
                    !std::filesystem::exists(workspace / "src" / "move.txt") &&
                    read_text(workspace / "src" / "moved.txt") == "move me\n",
                "changeset delete and move produce exact final paths");

    const auto snapshot = tools.workspace_change_snapshot();
    MINT_EXPECT(snapshot.at("changed_files").size() == 5,
                "change journal represents a move as one deletion and one creation");
    MINT_EXPECT(snapshot.at("diff").get<std::string>().find(
                    "--- a/src/delete.txt\n+++ /dev/null") != std::string::npos,
                "change journal emits deleted-file unified diff headers");
    MINT_EXPECT(tools.workspace_change_state().at("schema_version") == 2,
                "v1.2 change journal persists file existence state");

    mint::ToolRegistry restored(
        workspace, mint::ToolRegistryOptions{.allow_write = true, .allowed_write_paths = {"src"}});
    restored.restore_workspace_change_state(tools.workspace_change_state());
    MINT_EXPECT(restored.workspace_change_snapshot().at("changed_files").size() == 5,
                "change journal restores created, modified and deleted paths");

    const auto before_failed_validation = read_text(workspace / "src" / "alpha.txt");
    const auto rejected = mint::Json::parse(
        tools.execute({"changeset-prevalidation",
                       "apply_changeset",
                       {{"changes", mint::Json::array({{{"operation", "replace"},
                                                        {"path", "src/alpha.txt"},
                                                        {"old_text", "beta\n"},
                                                        {"new_text", "gamma\n"}},
                                                       {{"operation", "delete"},
                                                        {"path", "src/moved.txt"},
                                                        {"old_text", "stale\n"}}})}}}));
    MINT_EXPECT(!rejected.at("ok").get<bool>() &&
                    read_text(workspace / "src" / "alpha.txt") == before_failed_validation,
                "changeset validates every operation before writing the first file");

    bool denied_seen = false;
    mint::ToolRegistry denied_tools(
        workspace, mint::ToolRegistryOptions{
                       .allow_write = true,
                       .allowed_write_paths = {"src"},
                       .change_set_approval = [&](const mint::ChangeSetApprovalRequest&) {
                           denied_seen = true;
                           return false;
                       }});
    const auto denied = mint::Json::parse(
        denied_tools.execute({"changeset-denied",
                              "apply_changeset",
                              {{"changes", mint::Json::array({{{"operation", "create"},
                                                               {"path", "src/denied.txt"},
                                                               {"new_text", "denied\n"}}})}}}));
    MINT_EXPECT(denied_seen && denied.at("status") == "denied" &&
                    !std::filesystem::exists(workspace / "src" / "denied.txt"),
                "changeset approval denial performs no writes");

    const auto ignored = mint::Json::parse(
        tools.execute({"changeset-ignored",
                       "apply_changeset",
                       {{"changes", mint::Json::array({{{"operation", "replace"},
                                                        {"path", ".git/config"},
                                                        {"old_text", "protected\n"},
                                                        {"new_text", "changed\n"}}})}}}));
    MINT_EXPECT(!ignored.at("ok").get<bool>() &&
                    read_text(workspace / ".git" / "config") == "protected\n",
                "changeset cannot write ignored repository metadata");

#if !defined(_WIN32)
    const auto locked = workspace / "zlocked";
    std::filesystem::create_directories(locked);
    std::filesystem::permissions(
        locked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);
    mint::ToolRegistry rollback_tools(
        workspace,
        mint::ToolRegistryOptions{.allow_write = true, .allowed_write_paths = {"src", "zlocked"}});
    const auto rolled_back = mint::Json::parse(rollback_tools.execute(
        {"changeset-rollback",
         "apply_changeset",
         {{"changes", mint::Json::array({{{"operation", "replace"},
                                          {"path", "src/alpha.txt"},
                                          {"old_text", "beta\n"},
                                          {"new_text", "gamma\n"}},
                                         {{"operation", "create"},
                                          {"path", "zlocked/new.txt"},
                                          {"new_text", "cannot write\n"}}})}}}));
    std::filesystem::permissions(locked, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    MINT_EXPECT(!rolled_back.at("ok").get<bool>() &&
                    read_text(workspace / "src" / "alpha.txt") == "beta\n" &&
                    !std::filesystem::exists(locked / "new.txt"),
                "a mid-commit filesystem failure restores every earlier file");
#endif
}

TEST(CommandRunnerTest, ExecutesAuthorizedCommands) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = test_executable.generic_string();
    const mint::ToolCall echo_call{
        "command-echo",
        "run_command",
        {{"program", program},
         {"args", mint::Json::array({"--command-helper", "echo", "hello"})},
         {"cwd", "src"},
         {"timeout_seconds", 2}}};

    mint::ToolRegistry disabled_tools(workspace);
    const auto disabled = mint::Json::parse(disabled_tools.execute(echo_call));
    MINT_EXPECT(!disabled.at("ok").get<bool>(), "run_command rejects missing user authorization");

    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.allowed_programs = {program},
                                                       .default_command_timeout_seconds = 2,
                                                       .max_command_timeout_seconds = 5,
                                                       .max_command_output_bytes = 256});
    MINT_EXPECT(tools.can_run_commands(), "command runner reports enabled capability");
    MINT_EXPECT(tools.definitions().size() == 4,
                "command-only registry exposes run_command as the fourth tool");

    const auto echoed = mint::Json::parse(tools.execute(echo_call));
    MINT_EXPECT(echoed.at("ok").get<bool>(), "approved command starts successfully");
    MINT_EXPECT(echoed.at("status") == "exited", "approved command exits normally");
    MINT_EXPECT(echoed.at("exit_code") == 0, "approved command returns exit code zero");
    MINT_EXPECT(echoed.at("cwd") == "src", "command result reports relative cwd");
    MINT_EXPECT(echoed.at("output").get<std::string>().find("arg=hello") != std::string::npos,
                "combined command output is captured");

    const auto quoted = mint::Json::parse(
        tools.execute({"command-quoting",
                       "run_command",
                       {{"program", program},
                        {"args", mint::Json::array({"--command-helper", "echo", "with spaces",
                                                    R"(quote"inside)", R"(trailing\\)"})}}}));
    const auto quoted_output = quoted.at("output").get<std::string>();
    MINT_EXPECT(quoted.at("exit_code") == 0 &&
                    quoted_output.find("arg=with spaces") != std::string::npos &&
                    quoted_output.find("arg=quote\"inside") != std::string::npos &&
                    quoted_output.find("arg=trailing\\\\") != std::string::npos,
                "argv preserves spaces, quotes and trailing backslashes without a shell");

    const auto failed = mint::Json::parse(tools.execute(
        {"command-fail",
         "run_command",
         {{"program", program}, {"args", mint::Json::array({"--command-helper", "fail"})}}}));
    MINT_EXPECT(failed.at("ok").get<bool>(),
                "a non-zero child exit is still a successfully executed tool");
    MINT_EXPECT(failed.at("status") == "exited", "failed command exits normally");
    MINT_EXPECT(failed.at("exit_code") == 7, "non-zero exit code is preserved");
    MINT_EXPECT(failed.at("output").get<std::string>().find("intentional command failure") !=
                    std::string::npos,
                "stderr is captured with stdout");

    const auto truncated = mint::Json::parse(tools.execute(
        {"command-flood",
         "run_command",
         {{"program", program}, {"args", mint::Json::array({"--command-helper", "flood"})}}}));
    MINT_EXPECT(truncated.at("exit_code") == 0, "large-output command still completes");
    MINT_EXPECT(truncated.at("output_truncated").get<bool>(), "command output reports truncation");
    MINT_EXPECT(truncated.at("output").get<std::string>().size() == 256,
                "captured output respects the byte limit");

    set_secret_environment();
    const auto filtered_environment = mint::Json::parse(
        tools.execute({"command-environment",
                       "run_command",
                       {{"program", program},
                        {"args", mint::Json::array({"--command-helper", "environment"})}}}));
    clear_secret_environment();
    MINT_EXPECT(filtered_environment.at("exit_code") == 0,
                "command child receives the filtered environment");
    MINT_EXPECT(filtered_environment.at("output").get<std::string>().find("environment filtered") !=
                    std::string::npos,
                "unapproved environment values are not inherited");

    const auto inherited_path = workspace / "inherited.txt";
    write_text(inherited_path, "must not be inherited\n");
#if defined(_WIN32)
    SECURITY_ATTRIBUTES inherited_attributes{};
    inherited_attributes.nLength = sizeof(inherited_attributes);
    inherited_attributes.bInheritHandle = TRUE;
    const auto inherited_handle =
        ::CreateFileW(inherited_path.c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherited_attributes,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(inherited_handle, INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION inherited_metadata{};
    ASSERT_TRUE(::GetFileInformationByHandle(inherited_handle, &inherited_metadata));
    const auto filtered_descriptor = mint::Json::parse(tools.execute(
        {"command-handle",
         "run_command",
         {{"program", program},
          {"args",
           mint::Json::array({"--command-helper", "handle",
                              std::to_string(reinterpret_cast<std::uintptr_t>(inherited_handle)),
                              std::to_string(inherited_metadata.dwVolumeSerialNumber),
                              std::to_string(inherited_metadata.nFileIndexHigh),
                              std::to_string(inherited_metadata.nFileIndexLow)})}}}));
    (void)::CloseHandle(inherited_handle);
#else
    const auto inherited_descriptor = ::open(inherited_path.c_str(), O_RDONLY);
    ASSERT_GE(inherited_descriptor, 3);
    struct stat inherited_metadata{};
    ASSERT_EQ(::fstat(inherited_descriptor, &inherited_metadata), 0);
    const auto filtered_descriptor = mint::Json::parse(tools.execute(
        {"command-descriptor",
         "run_command",
         {{"program", program},
          {"args", mint::Json::array({"--command-helper", "descriptor",
                                      std::to_string(inherited_descriptor),
                                      std::to_string(inherited_metadata.st_dev),
                                      std::to_string(inherited_metadata.st_ino)})}}}));
    (void)::close(inherited_descriptor);
#endif
    MINT_EXPECT(filtered_descriptor.at("exit_code") == 0,
                "command child cannot inherit unrelated mint handles");

    const auto timed_out = mint::Json::parse(
        tools.execute({"command-timeout",
                       "run_command",
                       {{"program", program},
                        {"args", mint::Json::array({"--command-helper", "sleep"})},
                        {"timeout_seconds", 1}}}));
    MINT_EXPECT(timed_out.at("ok").get<bool>(), "timeout returns a structured command result");
    MINT_EXPECT(timed_out.at("timed_out").get<bool>(), "timeout is reported explicitly");
    MINT_EXPECT(timed_out.at("status") == "timed_out", "timeout has a distinct status");
    MINT_EXPECT(timed_out.at("exit_code").is_null(), "timed-out command has no exit code");

    const auto escaped_cwd = mint::Json::parse(
        tools.execute({"command-escape", "run_command", {{"program", program}, {"cwd", ".."}}}));
    MINT_EXPECT(!escaped_cwd.at("ok").get<bool>(), "run_command rejects cwd outside the workspace");

    const auto unapproved = mint::Json::parse(tools.execute(
        {"command-unapproved",
         "run_command",
         {{"program", "/bin/echo"}, {"args", mint::Json::array({"should-not-run"})}}}));
    MINT_EXPECT(!unapproved.at("ok").get<bool>(),
                "run_command rejects programs not approved by the user");

#if defined(_WIN32)
    const std::array blocked_launchers = {"cmd.exe", "powershell.exe"};
#else
    const std::array blocked_launchers = {"sh", "bwrap"};
#endif
    for (const std::string launcher : blocked_launchers) {
        bool blocked_launcher = false;
        try {
            mint::ToolRegistry blocked(workspace,
                                       mint::ToolRegistryOptions{.allowed_programs = {launcher}});
        } catch (const std::invalid_argument& error) {
            blocked_launcher = std::string(error.what()).find("不允许授权") != std::string::npos;
        }
        MINT_EXPECT(blocked_launcher,
                    "the command policy refuses shells and general-purpose launchers");
    }
}

TEST(CommandRunnerTest, EnforcesResourceLimits) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = test_executable.generic_string();

    mint::ToolRuntimeSettings runtime;
    runtime.command_resources = {
        .cpu_seconds = 10,
        .memory_bytes = MINT_TEST_ADDRESS_SANITIZED ? 0 : std::size_t{128} * 1024 * 1024,
        .max_processes = 128,
#if defined(_WIN32)
        .file_size_bytes = 0};
#else
        .file_size_bytes = 1024};
#endif
    mint::ToolRegistry tools(
        workspace, mint::ToolRegistryOptions{.allowed_programs = {program}, .runtime = runtime});

    const auto reported = mint::Json::parse(
        tools.execute({"command-limits",
                       "run_command",
                       {{"program", program},
                        {"args", mint::Json::array({"--command-helper", "echo", "limits"})}}}));
    EXPECT_EQ(reported.at("exit_code"), 0) << reported.dump(2);
    EXPECT_EQ(reported.at("resource_limits"),
              mint::command_resource_limits_to_json(runtime.command_resources));

#if !defined(_WIN32)
    const auto inspected = mint::Json::parse(tools.execute(
        {"command-limit-inspection",
         "run_command",
         {{"program", program}, {"args", mint::Json::array({"--command-helper", "limits"})}}}));
    EXPECT_EQ(inspected.at("exit_code"), 0) << inspected.dump(2);
    const auto output = inspected.at("output").get<std::string>();
    EXPECT_NE(output.find("cpu=10\n"), std::string::npos);
    EXPECT_NE(output.find("file=1024\n"), std::string::npos);
#if defined(__linux__)
    if (runtime.command_resources.memory_bytes != 0) {
        EXPECT_NE(output.find("memory=134217728\n"), std::string::npos);
    }

    mint::ToolRuntimeSettings workspace_runtime;
    workspace_runtime.command_resources.workspace_disk_bytes = 1024;
    mint::ToolRegistry workspace_tools(
        workspace,
        mint::ToolRegistryOptions{.allowed_programs = {program}, .runtime = workspace_runtime});
    const auto workspace_limited = mint::Json::parse(workspace_tools.execute(
        {"command-workspace-limit",
         "run_command",
         {{"program", program},
          {"args",
           mint::Json::array({"--command-helper", "write-large", "workspace-limit.bin"})}}}));
    EXPECT_EQ(workspace_limited.at("status"), "resource_limited") << workspace_limited.dump(2);
    EXPECT_EQ(workspace_limited.at("resource_limit"), "workspace_disk");
    const auto preflight_limited = mint::Json::parse(workspace_tools.execute(
        {"command-workspace-preflight",
         "run_command",
         {{"program", program},
          {"args", mint::Json::array({"--command-helper", "echo", "must-not-run"})}}}));
    EXPECT_EQ(preflight_limited.at("status"), "resource_limited") << preflight_limited.dump(2);
    EXPECT_TRUE(preflight_limited.at("output").get<std::string>().empty());
#endif

    const auto limited_path = workspace / "limited.bin";
    const auto limited = mint::Json::parse(
        tools.execute({"command-file-limit",
                       "run_command",
                       {{"program", program},
                        {"args", mint::Json::array({"--command-helper", "write-large",
                                                    limited_path.filename().string()})}}}));
    std::error_code error;
    const auto written = std::filesystem::file_size(limited_path, error);
    EXPECT_FALSE(error);
    EXPECT_LE(written, runtime.command_resources.file_size_bytes);
    EXPECT_EQ(limited.at("status"), "resource_limited");
    EXPECT_EQ(limited.at("resource_limit"), "file_size");
#endif

    if (runtime.command_resources.memory_bytes != 0) {
        const auto memory_limited = mint::Json::parse(
            tools.execute({"command-memory-limit",
                           "run_command",
                           {{"program", program},
                            {"args", mint::Json::array({"--command-helper", "allocate",
                                                        std::to_string(256 * 1024 * 1024)})},
                            {"timeout_seconds", 5}}}));
        EXPECT_FALSE(memory_limited.at("timed_out").get<bool>());
#if defined(__APPLE__) || defined(_WIN32)
        EXPECT_TRUE(memory_limited.at("resource_limited").get<bool>()) << memory_limited.dump(2);
        EXPECT_EQ(memory_limited.at("resource_limit"), "memory");
#else
        EXPECT_TRUE(memory_limited.at("resource_limited").get<bool>() ||
                    memory_limited.at("status") == "signaled" ||
                    memory_limited.value("exit_code", 0) != 0);
#endif
    }

    mint::ToolRuntimeSettings cpu_runtime;
    cpu_runtime.command_resources.cpu_seconds = 1;
    mint::ToolRegistry cpu_tools(workspace, mint::ToolRegistryOptions{.allowed_programs = {program},
                                                                      .runtime = cpu_runtime});
    const auto cpu_limited = mint::Json::parse(
        cpu_tools.execute({"command-cpu-limit",
                           "run_command",
                           {{"program", program},
                            {"args", mint::Json::array({"--command-helper", "spin"})},
                            {"timeout_seconds", 15}}}));
    EXPECT_EQ(cpu_limited.at("status"), "resource_limited");
    EXPECT_EQ(cpu_limited.at("resource_limit"), "cpu");

    mint::ToolRuntimeSettings process_runtime;
    process_runtime.command_resources.max_processes = 1;
    mint::ToolRegistry process_tools(
        workspace,
        mint::ToolRegistryOptions{.allowed_programs = {program}, .runtime = process_runtime});
    const auto process_limited = mint::Json::parse(
        process_tools.execute({"command-process-limit",
                               "run_command",
                               {{"program", program},
                                {"args", mint::Json::array({"--command-helper", "spawn"})},
                                {"timeout_seconds", 5}}}));
    EXPECT_EQ(process_limited.at("status"), "resource_limited") << process_limited.dump(2);
    EXPECT_EQ(process_limited.at("resource_limit"), "processes");

#if defined(_WIN32)
    auto unsupported_runtime = mint::ToolRuntimeSettings{};
    unsupported_runtime.command_resources.file_size_bytes = 1024;
    EXPECT_THROW((void)mint::ToolRegistry(
                     workspace, mint::ToolRegistryOptions{.allowed_programs = {program},
                                                          .runtime = unsupported_runtime}),
                 std::invalid_argument);
#endif
}

TEST(CommandRunnerTest, EnforcesTaskPolicyAndRecipes) {
    const auto legacy_defaults = mint::parse_task_policy({{"schema_version", 1}});
    MINT_EXPECT(legacy_defaults.fingerprint == "1fbaf1adbf33a12d",
                "adding optional runtime settings preserves legacy policy fingerprints");
    const auto explicit_defaults = mint::parse_task_policy(
        {{"schema_version", 1}, {"tool_limits", mint::tool_runtime_settings_to_json({})}});
    MINT_EXPECT(explicit_defaults.fingerprint == legacy_defaults.fingerprint,
                "explicit default tool limits have the same semantic policy fingerprint");
    expect_failure(
        [] {
            (void)mint::parse_task_policy(
                {{"schema_version", 1}, {"tool_limits", {{"search_max_hits", 0}}}});
        },
        "task policy rejects unsafe tool budget values");
#if defined(_WIN32)
    const std::filesystem::path external_policy_path = R"(C:\toolchain)";
#else
    const std::filesystem::path external_policy_path = "/opt/toolchain";
#endif
    const auto external_policy = mint::parse_task_policy(
        {{"schema_version", 1},
         {"command_read_paths", mint::Json::array({external_policy_path.generic_string()})}});
    MINT_EXPECT(external_policy.command_read_paths ==
                    std::vector<std::filesystem::path>{external_policy_path.lexically_normal()},
                "task policy preserves explicit external command read paths");
    MINT_EXPECT(external_policy.fingerprint != legacy_defaults.fingerprint,
                "external command read paths participate in the policy fingerprint");
    expect_failure(
        [] {
            (void)mint::parse_task_policy(
                {{"schema_version", 1}, {"command_read_paths", mint::Json::array({"relative"})}});
        },
        "task policy rejects relative external command read paths");

    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto policy_path = temporary.path() / "policy.json";
    const auto program = test_executable.generic_string();
#if defined(_WIN32)
    constexpr std::size_t policy_file_size_bytes = 0;
#else
    constexpr std::size_t policy_file_size_bytes = 1048576;
#endif
    write_text(
        policy_path,
        mint::Json{
            {"schema_version", 1},
            {"write_paths", mint::Json::array({"src"})},
            {"recipes", mint::Json::array(
                            {{{"name", "verify"},
                              {"description", "Run the deterministic verification helper"},
                              {"program", program},
                              {"args", mint::Json::array({"--command-helper", "echo", "recipe"})},
                              {"cwd", "src"},
                              {"timeout_seconds", 5},
                              {"verification", true}}})},
            {"require_verification", true},
            {"max_turns", 24},
            {"max_context_bytes", 131072},
            {"max_seconds", 900},
            {"tool_limits",
             {{"read_file_bytes", 4096},
              {"list_max_entries", 50},
              {"search_file_bytes", 524288},
              {"search_max_hits", 25},
              {"search_max_files", 500},
              {"command_output_bytes", 4096},
              {"command_resources",
               {{"cpu_seconds", 30},
                {"memory_bytes", 0},
                {"max_processes", 256},
                {"file_size_bytes", policy_file_size_bytes}}}}}}
            .dump(2));

    const auto policy = mint::load_task_policy(policy_path);
    MINT_EXPECT(policy.write_paths.size() == 1 && policy.recipes.size() == 1 &&
                    policy.recipes.front().verification && policy.require_verification,
                "task policy loads write paths, immutable recipes and verification contract");
    MINT_EXPECT(policy.max_turns == 24 && policy.max_context_bytes == 131072 &&
                    policy.max_seconds == 900 && !policy.fingerprint.empty(),
                "task policy loads bounded task budgets and a stable fingerprint");
    MINT_EXPECT(policy.tool_limits.read_file_bytes == 4096 &&
                    policy.tool_limits.list_max_entries == 50 &&
                    policy.tool_limits.search_file_bytes == 524288 &&
                    policy.tool_limits.search_max_hits == 25 &&
                    policy.tool_limits.search_max_files == 500 &&
                    policy.tool_limits.command_output_bytes == 4096 &&
                    policy.tool_limits.command_resources.cpu_seconds == 30 &&
                    policy.tool_limits.command_resources.memory_bytes == 0 &&
                    policy.tool_limits.command_resources.max_processes == 256 &&
                    policy.tool_limits.command_resources.file_size_bytes == policy_file_size_bytes,
                "task policy loads configurable tool performance budgets");
    const auto second_load = mint::load_task_policy(policy_path);
    MINT_EXPECT(second_load.fingerprint == policy.fingerprint,
                "equivalent policy content has a deterministic fingerprint");

    bool approval_shape_valid = false;
    mint::ToolRegistry tools(
        workspace, mint::ToolRegistryOptions{
                       .protected_paths = {policy.source_path},
                       .allow_write = true,
                       .allowed_write_paths = policy.write_paths,
                       .command_recipes = policy.recipes,
                       .policy_fingerprint = policy.fingerprint,
                       .command_approval =
                           [&](const mint::CommandApprovalRequest& request) {
                               approval_shape_valid =
                                   request.program == program && request.cwd == "src" &&
                                   request.timeout_seconds == 5 &&
                                   request.args == std::vector<std::string>{"--command-helper",
                                                                            "echo", "recipe"};
                               return true;
                           },
                       .runtime = policy.tool_limits});
    MINT_EXPECT(tools.uses_command_recipes() &&
                    tools.command_recipe_names() == std::vector<std::string>{"verify"} &&
                    tools.policy_fingerprint() == policy.fingerprint,
                "tool registry exposes recipe capability without raw command arguments");
    const auto definitions = tools.definitions();
    bool has_recipe = false;
    bool has_raw_command = false;
    for (const auto& definition : definitions) {
        const auto name = definition.at("function").value("name", "");
        has_recipe = has_recipe || name == "run_recipe";
        has_raw_command = has_raw_command || name == "run_command";
    }
    MINT_EXPECT(has_recipe && !has_raw_command,
                "policy mode exposes run_recipe instead of model-controlled argv");

    const auto executed =
        mint::Json::parse(tools.execute({"recipe-run", "run_recipe", {{"recipe", "verify"}}}));
    MINT_EXPECT(
        approval_shape_valid && executed.at("exit_code") == 0 &&
            executed.at("recipe") == "verify" && executed.at("verification_eligible").get<bool>() &&
            executed.at("output").get<std::string>().find("arg=recipe") != std::string::npos,
        "recipe execution uses exactly the policy argv, cwd, timeout and verification marker");

    const auto override_attempt = mint::Json::parse(
        tools.execute({"recipe-override",
                       "run_recipe",
                       {{"recipe", "verify"}, {"args", mint::Json::array({"override"})}}}));
    MINT_EXPECT(!override_attempt.at("ok").get<bool>(),
                "run_recipe rejects model attempts to override fixed arguments");

    const auto invalid_policy = temporary.path() / "invalid-policy.json";
    write_text(invalid_policy, R"({"schema_version":1,"unknown_capability":true})");
    bool rejected_unknown = false;
    try {
        (void)mint::load_task_policy(invalid_policy);
    } catch (const std::invalid_argument& error) {
        rejected_unknown = std::string(error.what()).find("未知字段") != std::string::npos;
    }
    MINT_EXPECT(rejected_unknown, "task policy rejects unknown capability fields");

    const auto unverifiable_policy = temporary.path() / "unverifiable-policy.json";
    write_text(unverifiable_policy, mint::Json{{"schema_version", 1},
                                               {"write_paths", mint::Json::array({"src"})},
                                               {"recipes", mint::Json::array()},
                                               {"require_verification", true}}
                                        .dump());
    bool rejected_unverifiable = false;
    try {
        (void)mint::load_task_policy(unverifiable_policy);
    } catch (const std::invalid_argument& error) {
        rejected_unverifiable =
            std::string(error.what()).find("verification=true") != std::string::npos;
    }
    MINT_EXPECT(rejected_unverifiable,
                "verification policy requires a verification-eligible recipe");
}

TEST(ToolRegistryTest, EnforcesWritePathAllowlist) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "keep\n");

    mint::ToolRegistry tools(
        workspace, mint::ToolRegistryOptions{.allow_write = true,
                                             .allowed_write_paths = {"src", "FIX_REPORT.md"}});
    MINT_EXPECT(tools.allowed_write_paths() == std::vector<std::string>({"src", "FIX_REPORT.md"}),
                "write allowlist exposes stable relative policy labels");

    const auto denied = mint::Json::parse(tools.execute({"scope-denied",
                                                         "apply_patch",
                                                         {{"path", "README.md"},
                                                          {"operation", "replace"},
                                                          {"old_text", "keep\n"},
                                                          {"new_text", "changed\n"}}}));
    MINT_EXPECT(!denied.at("ok").get<bool>() && read_text(workspace / "README.md") == "keep\n",
                "write allowlist rejects an otherwise valid edit outside its scope");

    const auto allowed_file = mint::Json::parse(tools.execute(
        {"scope-report",
         "apply_patch",
         {{"path", "FIX_REPORT.md"}, {"operation", "create"}, {"new_text", "verified\n"}}}));
    MINT_EXPECT(allowed_file.at("ok").get<bool>(),
                "write allowlist permits an exact not-yet-created file");

    const auto allowed_directory =
        mint::Json::parse(tools.execute({"scope-source",
                                         "apply_patch",
                                         {{"path", "src/generated.cpp"},
                                          {"operation", "create"},
                                          {"new_text", "int generated = 1;\n"}}}));
    MINT_EXPECT(allowed_directory.at("ok").get<bool>(),
                "write allowlist permits descendants of an authorized existing directory");

    bool rejected_escape = false;
    try {
        mint::ToolRegistry invalid(
            workspace, mint::ToolRegistryOptions{.allow_write = true,
                                                 .allowed_write_paths = {"../outside.txt"}});
    } catch (const std::invalid_argument&) {
        rejected_escape = true;
    }
    MINT_EXPECT(rejected_escape, "write allowlist rejects paths outside the workspace");
}

TEST(CommandRunnerTest, EnforcesRuntimeControls) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = test_executable.generic_string();
    const mint::ToolCall echo_call{
        "approval-echo",
        "run_command",
        {{"program", program},
         {"args", mint::Json::array({"--command-helper", "echo", "approved"})},
         {"cwd", "src"},
         {"timeout_seconds", 2}}};

    bool approval_seen = false;
    bool approval_shape_valid = false;
    mint::ToolRegistry denied_tools(
        workspace, mint::ToolRegistryOptions{
                       .allowed_programs = {program},
                       .command_approval = [&](const mint::CommandApprovalRequest& request) {
                           approval_seen = true;
                           approval_shape_valid =
                               request.program == program && request.cwd == "src" &&
                               request.timeout_seconds == 2 && request.args.size() == 3 &&
                               request.args.back() == "approved";
                           return false;
                       }});
    const auto denied = mint::Json::parse(denied_tools.execute(echo_call));
    MINT_EXPECT(approval_seen && approval_shape_valid,
                "per-command approval receives the exact argv, cwd and timeout");
    MINT_EXPECT(!denied.at("ok").get<bool>() && denied.at("status") == "denied",
                "denied command returns a structured result without starting");

    mint::ToolRegistry approved_tools(
        workspace,
        mint::ToolRegistryOptions{
            .allowed_programs = {program},
            .command_approval = [](const mint::CommandApprovalRequest&) { return true; }});
    const auto approved = mint::Json::parse(approved_tools.execute(echo_call));
    MINT_EXPECT(approved.at("exit_code") == 0, "approved per-command request starts normally");

    auto budget = std::make_shared<mint::TaskControl>(std::chrono::milliseconds(100));
    mint::ToolRegistry budget_tools(workspace,
                                    mint::ToolRegistryOptions{.allowed_programs = {program},
                                                              .default_command_timeout_seconds = 5,
                                                              .max_command_timeout_seconds = 5,
                                                              .task_control = budget});
    const auto started = std::chrono::steady_clock::now();
    const auto timed_out = mint::Json::parse(
        budget_tools.execute({"task-budget",
                              "run_command",
                              {{"program", program},
                               {"args", mint::Json::array({"--command-helper", "sleep"})},
                               {"timeout_seconds", 5}}}));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    MINT_EXPECT(timed_out.at("status") == "task_timed_out" &&
                    timed_out.at("task_timed_out").get<bool>(),
                "total task budget has a distinct command outcome");
    MINT_EXPECT(elapsed < 1500, "total task budget terminates the running process group promptly");
}

TEST(CommandRunnerTest, EnforcesOperatingSystemSandbox) {
#if defined(_WIN32)
    TemporaryDirectory temporary;
#else
    TemporaryDirectory temporary(std::filesystem::path{"/var/tmp"});
#endif
    const auto workspace = temporary.path() / "workspace";
    const auto program = test_executable.generic_string();
    const auto protected_secret = workspace / "protected-secret.txt";
    write_text(protected_secret, "sandbox secret\n");
    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.protected_paths = {protected_secret},
                                                       .allowed_programs = {program},
                                                       .require_command_sandbox = true});
    MINT_EXPECT(tools.commands_are_os_sandboxed(),
                "command registry reports OS sandbox enforcement");
#if defined(__APPLE__)
    constexpr std::string_view expected_backend = "macos-seatbelt";
#elif defined(_WIN32)
    constexpr std::string_view expected_backend = "windows-appcontainer";
#else
    constexpr std::string_view expected_backend = "linux-bubblewrap";
#endif
    MINT_EXPECT(tools.command_sandbox_backend() == expected_backend,
                "the native command sandbox backend is named in policy state");

    std::error_code path_error;
    const auto inside = std::filesystem::weakly_canonical(workspace / "inside.txt", path_error);
    MINT_EXPECT(!path_error, "sandbox test resolves inside path");
    const auto allowed = mint::Json::parse(tools.execute(
        {"sandbox-inside",
         "run_command",
         {{"program", program},
          {"args", mint::Json::array({"--command-helper", "write", inside.generic_string()})}}}));
    MINT_EXPECT(allowed.at("sandboxed").get<bool>() &&
                    allowed.at("sandbox_backend").get<std::string>() == expected_backend,
                "command result carries auditable sandbox metadata");
    MINT_EXPECT(allowed.at("exit_code") == 0 && std::filesystem::exists(inside),
                "sandbox permits writes inside the workspace: " + allowed.dump());

    const auto outside =
        std::filesystem::weakly_canonical(temporary.path() / "outside.txt", path_error);
    MINT_EXPECT(!path_error, "sandbox test resolves outside path");
    const auto blocked_write = mint::Json::parse(tools.execute(
        {"sandbox-outside",
         "run_command",
         {{"program", program},
          {"args", mint::Json::array({"--command-helper", "write", outside.generic_string()})}}}));
    const bool wrote_to_host = std::filesystem::exists(outside);
    std::error_code cleanup_error;
    (void)std::filesystem::remove(outside, cleanup_error);
    MINT_EXPECT(blocked_write.at("exit_code") != 0 && !wrote_to_host,
                "sandbox blocks writes to host paths outside the workspace: " +
                    blocked_write.dump());

    const auto blocked_read = mint::Json::parse(tools.execute(
        {"sandbox-protected-read",
         "run_command",
         {{"program", program},
          {"args", mint::Json::array({"--command-helper", "verify",
                                      protected_secret.generic_string(), "sandbox secret\n"})}}}));
    MINT_EXPECT(blocked_read.at("exit_code") != 0,
                "sandbox blocks command reads of protected runtime files");

#if defined(_WIN32)
    const auto external_directory = temporary.path() / "external-toolchain";
#else
    const char* previous_home_value = std::getenv("HOME");
    const std::optional<std::string> previous_home =
        previous_home_value == nullptr ? std::nullopt
                                       : std::optional(std::string(previous_home_value));
    const auto isolated_home = temporary.path() / "home";
    const auto external_directory = isolated_home / "external-toolchain";
    ASSERT_EQ(::setenv("HOME", isolated_home.c_str(), 1), 0);
#endif
    const auto external_file = external_directory / "metadata.txt";
    std::filesystem::create_directories(external_directory);
    write_text(external_file, "toolchain metadata\n");
    EXPECT_THROW(
        (void)mint::ToolRegistry(
            workspace, mint::ToolRegistryOptions{.command_read_paths = {external_directory},
                                                 .allowed_programs = {program}}),
        std::invalid_argument);
    EXPECT_THROW((void)mint::ToolRegistry(
                     workspace, mint::ToolRegistryOptions{.command_read_paths = {workspace},
                                                          .allowed_programs = {program},
                                                          .require_command_sandbox = true}),
                 std::invalid_argument);
    EXPECT_THROW(
        (void)mint::ToolRegistry(
            workspace, mint::ToolRegistryOptions{.protected_paths = {external_file},
                                                 .command_read_paths = {external_directory},
                                                 .allowed_programs = {program},
                                                 .require_command_sandbox = true}),
        std::invalid_argument);
    mint::ToolRegistry external_tools(
        workspace, mint::ToolRegistryOptions{.command_read_paths = {external_directory},
                                             .allowed_programs = {program},
                                             .require_command_sandbox = true});
    const auto external_read = mint::Json::parse(external_tools.execute(
        {"sandbox-external-read",
         "run_command",
         {{"program", program},
          {"args", mint::Json::array({"--command-helper", "verify", external_file.generic_string(),
                                      "toolchain metadata\n"})}}}));
    MINT_EXPECT(external_read.at("exit_code") == 0,
                "sandbox exposes explicitly authorized external paths for reads: " +
                    external_read.dump());
    const auto external_write = mint::Json::parse(external_tools.execute(
        {"sandbox-external-write",
         "run_command",
         {{"program", program},
          {"args",
           mint::Json::array({"--command-helper", "write", external_file.generic_string()})}}}));
    MINT_EXPECT(external_write.at("exit_code") != 0 &&
                    read_text(external_file) == "toolchain metadata\n",
                "external command paths remain read-only: " + external_write.dump());
#if !defined(_WIN32)
    if (previous_home.has_value()) {
        ASSERT_EQ(::setenv("HOME", previous_home->c_str(), 1), 0);
    } else {
        ASSERT_EQ(::unsetenv("HOME"), 0);
    }
#endif

    LoopbackListener host_listener;
    const auto blocked_network = mint::Json::parse(
        tools.execute({"sandbox-network",
                       "run_command",
                       {{"program", program},
                        {"args", mint::Json::array({"--command-helper", "network",
                                                    std::to_string(host_listener.port())})}}}));
    MINT_EXPECT(blocked_network.at("exit_code") == 0,
                "sandbox prevents the command from reaching the host network: " +
                    blocked_network.dump());

#if defined(_WIN32)
    mint::ToolRegistry cmake_tools(
        workspace,
        mint::ToolRegistryOptions{.allowed_programs = {"cmake"}, .require_command_sandbox = true});
    const auto cmake_result = mint::Json::parse(
        cmake_tools.execute({"cmake-version",
                             "run_command",
                             {{"program", "cmake"}, {"args", mint::Json::array({"--version"})}}}));
    MINT_EXPECT(cmake_result.at("sandbox_backend") == "windows-appcontainer",
                "CMake runs through the Windows AppContainer backend");
    const auto cmake_output = cmake_result.at("output").get<std::string>();
    MINT_EXPECT(cmake_result.at("exit_code") == 0 &&
                    cmake_output.find("cmake version") != std::string::npos,
                "installed CMake remains usable in the AppContainer: " + cmake_result.dump());
#endif
}

TEST(AgentLoopTest, CompletesReadOnlyTask) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Loop test\n");

    mint::ToolRegistry tools(workspace);
    ScriptedModel model;
    std::ostringstream log;
    mint::Agent agent(model, tools, log);

    const auto result = agent.run("读取 README 后回答");
    MINT_EXPECT(result.completed, "agent reaches a final answer");
    MINT_EXPECT(result.answer == "完成", "agent returns model final answer");
    MINT_EXPECT(result.turns == 2, "agent performs tool turn then final turn");
    MINT_EXPECT(result.execution.tool_calls == 1, "agent summary counts the read tool call");
    MINT_EXPECT(result.execution.successful_tool_calls == 1,
                "agent summary counts the successful read tool");
    MINT_EXPECT(result.model.calls == 2 && result.model.attempts == 2 &&
                    result.model.usage_reports == 1 && result.model.total_tokens == 110 &&
                    result.model.cached_tokens == 80,
                "agent aggregates model attempts and token usage across turns");
    MINT_EXPECT(log.str().find("read_file") != std::string::npos, "agent log shows tool call");
    MINT_EXPECT(log.str().find("缓存 80，命中 80%") != std::string::npos,
                "agent log shows observable prompt cache usage");
}

TEST(AgentLoopTest, EnforcesContextBudget) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "large.txt", std::string(128 * 1024, 'x'));

    mint::ToolRegistry tools(workspace);
    ContextBudgetModel model;
    std::ostringstream output;
    mint::Agent agent(model, tools, output,
                      mint::AgentOptions{.max_turns = 6, .max_context_bytes = 16 * 1024});
    const auto result = agent.run("重复读取大文件并验证上下文压缩");
    MINT_EXPECT(result.completed && result.answer == "上下文预算通过",
                "agent completes after multiple compacted large tool results");
    MINT_EXPECT(result.execution.tool_calls == 3,
                "context compaction does not alter executed tool history");
}

TEST(AgentLoopTest, ContextCompactionPreservesFailureEvidence) {
    const mint::Json arguments = {{"path", "large.txt"}};
    const mint::Json tool_call = {
        {"id", "failed-read"},
        {"type", "function"},
        {"function", {{"name", "read_file"}, {"arguments", arguments.dump()}}}};
    mint::Json large_diagnostics = mint::Json::array();
    for (int index = 0; index < 2000; ++index) {
        large_diagnostics.push_back(index);
    }
    const mint::Json failed_result = {{"ok", false},
                                      {"status", "failed"},
                                      {"error", "compile failed"},
                                      {"resource_limited", true},
                                      {"resource_limit", "memory"},
                                      {"resource_limits", {{"memory_bytes", 134217728}}},
                                      {"diagnostics", std::move(large_diagnostics)}};
    const mint::Json messages = mint::Json::array(
        {{{"role", "system"}, {"content", "system"}},
         {{"role", "user"}, {"content", "task"}},
         {{"role", "assistant"}, {"content", nullptr}, {"tool_calls", {tool_call}}},
         {{"role", "tool"}, {"tool_call_id", "failed-read"}, {"content", failed_result.dump()}}});

    const auto compacted = mint::agent_detail::compact_context(messages, 2048);
    MINT_EXPECT(compacted.payloads_compacted,
                "oversized latest tool group uses payload compaction");

    const auto tool_message =
        std::find_if(compacted.messages.begin(), compacted.messages.end(),
                     [](const mint::Json& message) { return message.value("role", "") == "tool"; });
    MINT_EXPECT(tool_message != compacted.messages.end(),
                "compaction retains the latest assistant/tool call pair");
    const auto retained = mint::Json::parse(tool_message->at("content").get<std::string>());
    MINT_EXPECT(retained.at("context_compacted").get<bool>() && !retained.at("ok").get<bool>() &&
                    retained.at("status") == "failed" && retained.at("error") == "compile failed" &&
                    retained.at("resource_limited").get<bool>() &&
                    retained.at("resource_limit") == "memory" &&
                    retained.at("resource_limits").at("memory_bytes") == 134217728 &&
                    !retained.contains("diagnostics"),
                "compaction omits bulk payload without changing failed evidence into success");
}

TEST(AgentLoopTest, EmitsEventLogAndMachineResult) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto event_path = temporary.path() / "events.jsonl";
    write_text(workspace / "README.md", "# Event secret body\n");

    mint::ToolRegistry tools(workspace);
    mint::DemoModelClient model;
    mint::EventLog events(event_path);
    std::ostringstream log;
    mint::Agent agent(model, tools, log, mint::AgentOptions{.event_log = &events});
    const auto result = agent.run("读取 README 后回答");
    const auto machine = mint::agent_result_to_json(result);
    MINT_EXPECT(machine.at("schema_version") == 1 && machine.at("status") == "completed" &&
                    machine.at("completed").get<bool>(),
                "machine result has a versioned completion contract");
    MINT_EXPECT(machine.at("execution").at("tool_calls") == 3,
                "machine result exposes the execution summary");
    MINT_EXPECT(machine.at("verification_status") == "not_required",
                "machine result exposes explicit verification state");

    std::ifstream input(event_path, std::ios::binary);
    std::string raw{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    MINT_EXPECT(raw.find("Event secret body") == std::string::npos &&
                    raw.find("\"query\":\"Agent\"") == std::string::npos,
                "JSONL trace omits file contents, search text and raw tool output");
    std::istringstream lines(raw);
    std::string line;
    std::size_t expected_sequence = 1;
    std::vector<std::string> types;
    while (std::getline(lines, line)) {
        const auto event = mint::Json::parse(line);
        MINT_EXPECT(event.at("schema_version") == 1,
                    "every JSONL event carries its schema version");
        MINT_EXPECT(event.at("seq") == expected_sequence++, "JSONL event sequence is monotonic");
        types.push_back(event.at("type").get<std::string>());
    }
    MINT_EXPECT(!types.empty() && types.front() == "task_started" &&
                    types.back() == "task_finished",
                "event trace brackets the complete task lifecycle");
    MINT_EXPECT(std::find(types.begin(), types.end(), "tool_started") != types.end() &&
                    std::find(types.begin(), types.end(), "tool_completed") != types.end(),
                "event trace records sanitized tool lifecycle events");
}

TEST(RuntimeFilesTest, ProtectsRuntimeFiles) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "existing.txt";
    write_text(target, "must not be overwritten\n");

    const auto append_path = temporary.path() / "append-events.jsonl";
    {
        mint::EventLog first(append_path);
        first.emit("first");
    }
    {
        mint::EventLog second(append_path, true);
        second.emit("second");
    }
    std::ifstream appended_input(append_path, std::ios::binary);
    std::string first_line;
    std::string second_line;
    std::getline(appended_input, first_line);
    std::getline(appended_input, second_line);
    MINT_EXPECT(mint::Json::parse(first_line).at("seq") == 1 &&
                    mint::Json::parse(second_line).at("seq") == 2,
                "resumed JSONL logging continues the prior sequence");

    const auto partial_path = temporary.path() / "partial-events.jsonl";
    write_text(partial_path, R"({"schema_version":1,"seq":4,"type":"stable","data":{}})"
                             "\n{\"schema_version\":1,\"seq\":5");
    {
        mint::EventLog recovered(partial_path, true);
        recovered.emit("after_crash");
    }
    std::ifstream recovered_input(partial_path, std::ios::binary);
    std::string recovered_line;
    std::vector<std::string> recovered_lines;
    while (std::getline(recovered_input, recovered_line)) {
        recovered_lines.push_back(recovered_line);
    }
    MINT_EXPECT(recovered_lines.size() == 3 &&
                    mint::Json::parse(recovered_lines.back()).at("seq") == 5,
                "JSONL recovery separates a crash-truncated final line from new events");

    std::error_code symlink_error;
    const auto event_link = temporary.path() / "events-link.jsonl";
    std::filesystem::create_symlink(target, event_link, symlink_error);
    if (!symlink_error) {
        bool event_rejected = false;
        bool session_rejected = false;
        try {
            mint::EventLog events(event_link);
        } catch (const std::invalid_argument&) {
            event_rejected = true;
        }
        const auto session_link = temporary.path() / "session-link.json";
        std::filesystem::create_symlink(target, session_link, symlink_error);
        try {
            mint::SessionStore session(session_link);
        } catch (const std::invalid_argument&) {
            session_rejected = true;
        }
        MINT_EXPECT(event_rejected && session_rejected,
                    "runtime files reject symbolic-link targets");
        MINT_EXPECT(read_text(target) == "must not be overwritten\n",
                    "runtime symlink rejection leaves the target unchanged");
    }

    std::error_code hard_link_error;
    const auto hard_link = temporary.path() / "events-hardlink.jsonl";
    std::filesystem::create_hard_link(target, hard_link, hard_link_error);
    if (!hard_link_error) {
        bool rejected = false;
        try {
            mint::EventLog events(hard_link);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        MINT_EXPECT(rejected, "event log rejects an existing file with multiple hard links");
        MINT_EXPECT(read_text(target) == "must not be overwritten\n",
                    "hard-link rejection leaves the shared inode unchanged");
    }

    const auto existing_directory = temporary.path() / "runtime-directory";
    std::filesystem::create_directory(existing_directory);
    bool event_directory_rejected = false;
    bool session_directory_rejected = false;
    try {
        mint::EventLog events(existing_directory);
    } catch (const std::invalid_argument&) {
        event_directory_rejected = true;
    }
    try {
        mint::SessionStore session(existing_directory);
    } catch (const std::invalid_argument&) {
        session_directory_rejected = true;
    }
    MINT_EXPECT(event_directory_rejected && session_directory_rejected,
                "runtime output paths reject existing non-regular files");
}

TEST(AgentLoopTest, CompletesWriteTask) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Before\n");

    mint::ToolRegistry tools(workspace, mint::ToolRegistryOptions{.allow_write = true});
    WritingModel model;
    std::ostringstream log;
    mint::Agent agent(model, tools, log);

    const auto result = agent.run("修改 README 标题");
    MINT_EXPECT(result.completed, "write-enabled agent reaches a final answer");
    MINT_EXPECT(result.answer == "修改完成", "write-enabled agent returns final answer");
    MINT_EXPECT(read_text(workspace / "README.md") == "# After\n",
                "agent loop executes apply_patch");
    MINT_EXPECT(result.execution.file_changes == 1,
                "write agent summary counts the file modification");
    MINT_EXPECT(result.changes.files == std::vector<std::string>{"README.md"},
                "write agent result exposes the changed file list");
    MINT_EXPECT(result.changes.unified_diff.find("-# Before\n+# After\n") != std::string::npos,
                "write agent result exposes the unified diff");
    MINT_EXPECT(result.verification_status == "not_run",
                "unverified write is marked not_run when the gate is disabled");
    const auto write_log = log.str();
    MINT_EXPECT(write_log.find("README.md") != std::string::npos,
                "patch log includes the target path");
    const auto before_final = write_log.substr(0, write_log.find("[最终回答]"));
    MINT_EXPECT(before_final.find("# Before") == std::string::npos,
                "patch log does not dump old file contents");
    MINT_EXPECT(before_final.find("# After") == std::string::npos,
                "patch log does not dump new file contents");
}

TEST(AgentLoopTest, PatchesThenVerifies) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Broken\n");
    const auto program = test_executable.generic_string();

    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.allow_write = true,
                                                       .allowed_programs = {program},
                                                       .default_command_timeout_seconds = 5,
                                                       .max_command_timeout_seconds = 5});
    PatchThenVerifyModel model(program);
    std::ostringstream log;
    mint::Agent agent(model, tools, log,
                      mint::AgentOptions{.require_verification_after_write = true});

    const auto result = agent.run("修复 README 并执行验证");
    MINT_EXPECT(result.completed, "patch-then-verify agent reaches a final answer");
    MINT_EXPECT(result.answer == "修改并验证完成",
                "patch-then-verify agent returns the verified answer");
    MINT_EXPECT(result.turns == 3, "agent performs patch, verification command, then final answer");
    MINT_EXPECT(result.execution.tool_calls == 2,
                "end-to-end summary counts patch and command tools");
    MINT_EXPECT(result.execution.file_changes == 1, "end-to-end summary counts the patch");
    MINT_EXPECT(result.execution.command_calls == 1,
                "end-to-end summary counts the verification command");
    MINT_EXPECT(result.execution.commands_passed == 1,
                "end-to-end summary records the passing verification");
    MINT_EXPECT(result.execution.commands_failed == 0, "end-to-end summary has no failed commands");
    MINT_EXPECT(result.verification_status == "passed",
                "end-to-end result records passed verification");
    MINT_EXPECT(result.changes.files == std::vector<std::string>{"README.md"},
                "end-to-end result lists the modified file");
    MINT_EXPECT(read_text(workspace / "README.md") == "# Fixed\n",
                "end-to-end loop leaves the requested file change");
    MINT_EXPECT(log.str().find("apply_patch") != std::string::npos,
                "end-to-end log records the patch tool");
    MINT_EXPECT(log.str().find("run_command") != std::string::npos,
                "end-to-end log records the verification command");
    const auto e2e_log = log.str();
    const auto e2e_before_final = e2e_log.substr(0, e2e_log.find("[最终回答]"));
    MINT_EXPECT(e2e_before_final.find("# Fixed") == std::string::npos,
                "end-to-end tool-call logs do not dump patch or command arguments");
    MINT_EXPECT(e2e_log.find("+# Fixed") != std::string::npos,
                "end-to-end final state prints the audited diff");
}

TEST(AgentLoopTest, FailedVerificationRequiresRepair) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Broken\n");
    const auto program = test_executable.generic_string();

    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.allow_write = true,
                                                       .allowed_programs = {program},
                                                       .default_command_timeout_seconds = 5,
                                                       .max_command_timeout_seconds = 5});
    FailureThenRepairModel model(program);
    std::ostringstream log;
    mint::Agent agent(model, tools, log,
                      mint::AgentOptions{.max_turns = 8, .require_verification_after_write = true});

    const auto result = agent.run("修复 README；验证失败时继续修复");
    MINT_EXPECT(result.completed, "verification-gated agent eventually completes");
    MINT_EXPECT(result.answer == "失败后继续修复并验证完成",
                "premature final answer is not accepted");
    MINT_EXPECT(result.turns == 6,
                "agent uses patch, failed verify, rejected final, patch, pass, final");
    MINT_EXPECT(model.gate_seen(), "scripted model receives the harness continuation requirement");
    MINT_EXPECT(result.execution.file_changes == 2,
                "execution summary counts both repair attempts");
    MINT_EXPECT(result.execution.command_calls == 2,
                "execution summary counts both verification attempts");
    MINT_EXPECT(result.execution.commands_failed == 1,
                "execution summary records the first failed verification");
    MINT_EXPECT(result.execution.commands_passed == 1,
                "execution summary records the final passing verification");
    MINT_EXPECT(result.verification_status == "passed", "final verification status is passed");
    MINT_EXPECT(read_text(workspace / "README.md") == "# Fixed\n",
                "second repair leaves the verified contents");
    MINT_EXPECT(result.changes.files == std::vector<std::string>{"README.md"},
                "change journal collapses repeated edits into one file");
    MINT_EXPECT(result.changes.unified_diff.find("-# Broken\n+# Fixed\n") != std::string::npos,
                "final diff compares the original baseline with verified contents");
    MINT_EXPECT(result.changes.unified_diff.find("Almost") == std::string::npos,
                "intermediate failed contents do not pollute the final diff");
    MINT_EXPECT(log.str().find("[继续]") != std::string::npos,
                "console trace records the rejected premature completion");
}

TEST(AgentLoopTest, LatestCommandControlsVerificationStatus) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Broken\n");
    const auto program = test_executable.generic_string();

    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.allow_write = true,
                                                       .allowed_programs = {program},
                                                       .default_command_timeout_seconds = 5,
                                                       .max_command_timeout_seconds = 5});
    PassThenFailModel model(program);
    std::ostringstream log;
    mint::Agent agent(model, tools, log,
                      mint::AgentOptions{.max_turns = 4, .require_verification_after_write = true});

    const auto result = agent.run("修改后先验证成功，再模拟一次回归失败");
    MINT_EXPECT(!result.completed,
                "a later failed command prevents completion despite an earlier pass");
    MINT_EXPECT(result.verification_status == "failed",
                "the latest post-write command controls verification status");
    MINT_EXPECT(result.execution.commands_passed == 1,
                "regression scenario records the initial pass");
    MINT_EXPECT(result.execution.commands_failed == 1,
                "regression scenario records the later failure");
    MINT_EXPECT(log.str().find("[继续]") != std::string::npos,
                "verification gate rejects completion after the later failure");
}

TEST(AgentLoopTest, DeniedCommandCannotSatisfyVerification) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = test_executable.generic_string();
    write_text(workspace / "README.md", "# Broken\n");

    mint::ToolRegistry tools(
        workspace,
        mint::ToolRegistryOptions{
            .allow_write = true,
            .allowed_programs = {program},
            .default_command_timeout_seconds = 5,
            .max_command_timeout_seconds = 5,
            .command_approval = [](const mint::CommandApprovalRequest&) { return false; }});
    DeniedVerificationModel model(program);
    std::ostringstream log;
    mint::Agent agent(model, tools, log,
                      mint::AgentOptions{.max_turns = 3, .require_verification_after_write = true});
    const auto result = agent.run("修改并申请运行验证");
    MINT_EXPECT(!result.completed && result.status == "max_turns",
                "approval denial prevents the model from ending the task");
    MINT_EXPECT(result.verification_status == "denied",
                "verification state distinguishes approval denial from test failure");
    MINT_EXPECT(result.execution.commands_denied == 1,
                "execution summary counts the denied command separately");
    MINT_EXPECT(log.str().find("[继续]") != std::string::npos,
                "verification gate requests continuation after denial");
}

TEST(AgentLoopTest, CancellationStopsRunningCommand) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = test_executable.generic_string();
    auto control = std::make_shared<mint::TaskControl>();

    mint::ToolRegistry tools(workspace,
                             mint::ToolRegistryOptions{.allowed_programs = {program},
                                                       .default_command_timeout_seconds = 5,
                                                       .max_command_timeout_seconds = 5,
                                                       .task_control = control});
    LongCommandModel model(program);
    std::ostringstream log;
    mint::Agent agent(model, tools, log,
                      mint::AgentOptions{.max_turns = 4, .task_control = control});

    std::thread canceller([control]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        control->request_cancel();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto result = agent.run("运行一个会被取消的命令");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    canceller.join();

    MINT_EXPECT(!result.completed && result.status == "cancelled",
                "agent exposes an explicit cancelled terminal state");
    MINT_EXPECT(result.stop_reason == "user_cancelled", "cancelled result records the stop reason");
    MINT_EXPECT(result.execution.commands_cancelled == 1,
                "execution summary counts the cancelled child command");
    MINT_EXPECT(elapsed < 1500, "agent cancellation terminates the child process group promptly");
    const auto machine = mint::agent_result_to_json(result);
    MINT_EXPECT(machine.at("status") == "cancelled" && !machine.at("completed").get<bool>(),
                "machine result preserves cancellation without pretending completion");
}

TEST(SessionTest, CheckpointsAndResumes) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto session_path = temporary.path() / "session.json";
    const auto program = test_executable.generic_string();
    write_text(workspace / "README.md", "# Broken\n");
    mint::SessionStore session(session_path);

    auto stop_after_model_reply = std::make_shared<mint::TaskControl>();
    mint::ToolRegistry first_tools(
        workspace, mint::ToolRegistryOptions{.protected_paths = {session_path},
                                             .allow_write = true,
                                             .allowed_programs = {program},
                                             .default_command_timeout_seconds = 5,
                                             .max_command_timeout_seconds = 5,
                                             .task_control = stop_after_model_reply});
    StoppingSessionPatchModel first_model(stop_after_model_reply);
    std::ostringstream first_log;
    mint::Agent first_agent(first_model, first_tools, first_log,
                            mint::AgentOptions{.max_turns = 1,
                                               .require_verification_after_write = true,
                                               .task_control = stop_after_model_reply,
                                               .session_store = &session});
    const auto interrupted = first_agent.run("修复 README 并验证");
    MINT_EXPECT(interrupted.status == "cancelled" && !interrupted.completed,
                "task cancellation stops the first invocation explicitly");
    MINT_EXPECT(read_text(workspace / "README.md") == "# Broken\n",
                "pending tool is checkpointed before execution when cancellation arrives");
    const auto checkpoint = session.load();
    MINT_EXPECT(checkpoint.at("schema_version") == 4 && checkpoint.at("status") == "cancelled" &&
                    checkpoint.at("pending_tool_calls").size() == 1 &&
                    checkpoint.at("in_flight_tool_call").is_null(),
                "session stores the pending call at a stable resume point");

    const auto incomplete_path = temporary.path() / "incomplete-v4-session.json";
    mint::SessionStore incomplete_session(incomplete_path);
    auto incomplete_checkpoint = checkpoint;
    incomplete_checkpoint.erase("in_flight_tool_call");
    incomplete_session.save(incomplete_checkpoint);
    bool rejected_incomplete_v4 = false;
    try {
        mint::ToolRegistry incomplete_tools(
            workspace, mint::ToolRegistryOptions{.protected_paths = {incomplete_path},
                                                 .allow_write = true,
                                                 .allowed_programs = {program},
                                                 .default_command_timeout_seconds = 5,
                                                 .max_command_timeout_seconds = 5});
        StoppingSessionPatchModel unused_model;
        std::ostringstream unused_log;
        mint::Agent incomplete_agent(unused_model, incomplete_tools, unused_log,
                                     mint::AgentOptions{.require_verification_after_write = true,
                                                        .session_store = &incomplete_session,
                                                        .resume_session = true});
        (void)incomplete_agent.run("");
    } catch (const std::invalid_argument& error) {
        rejected_incomplete_v4 =
            std::string(error.what()).find("缺少必需状态") != std::string::npos;
    }
    MINT_EXPECT(rejected_incomplete_v4,
                "schema v4 rejects a checkpoint missing its durable in-flight marker");

    bool rejected_tool_limit_change = false;
    try {
        mint::ToolRegistry mismatched_tools(
            workspace, mint::ToolRegistryOptions{
                           .protected_paths = {session_path},
                           .allow_write = true,
                           .allowed_programs = {program},
                           .default_command_timeout_seconds = 5,
                           .max_command_timeout_seconds = 5,
                           .runtime = mint::ToolRuntimeSettings{.read_file_bytes = 2048}});
        StoppingSessionPatchModel unused_model;
        std::ostringstream unused_log;
        mint::Agent mismatched_agent(unused_model, mismatched_tools, unused_log,
                                     mint::AgentOptions{.require_verification_after_write = true,
                                                        .session_store = &session,
                                                        .resume_session = true});
        (void)mismatched_agent.run("");
    } catch (const std::invalid_argument& error) {
        rejected_tool_limit_change =
            std::string(error.what()).find("能力授权") != std::string::npos;
    }
    MINT_EXPECT(rejected_tool_limit_change,
                "resume rejects tool budget changes that could alter pending work");

    const auto inflight_path = temporary.path() / "inflight-session.json";
    mint::SessionStore inflight_session(inflight_path);
    auto inflight_checkpoint = checkpoint;
    inflight_checkpoint["status"] = "running";
    inflight_checkpoint["in_flight_tool_call"] = inflight_checkpoint.at("pending_tool_calls").at(0);
    inflight_session.save(inflight_checkpoint);

    bool blocked_inflight_replay = false;
    try {
        mint::ToolRegistry blocked_tools(
            workspace, mint::ToolRegistryOptions{.protected_paths = {inflight_path},
                                                 .allow_write = true,
                                                 .allowed_programs = {program},
                                                 .default_command_timeout_seconds = 5,
                                                 .max_command_timeout_seconds = 5});
        ResumeVerificationModel unused_model(program);
        std::ostringstream unused_log;
        mint::Agent blocked_agent(unused_model, blocked_tools, unused_log,
                                  mint::AgentOptions{.max_turns = 2,
                                                     .require_verification_after_write = true,
                                                     .session_store = &inflight_session,
                                                     .resume_session = true});
        (void)blocked_agent.run("");
    } catch (const std::runtime_error& error) {
        blocked_inflight_replay =
            std::string(error.what()).find("--retry-inflight") != std::string::npos;
    }
    MINT_EXPECT(blocked_inflight_replay,
                "resume blocks an ambiguous side-effecting in-flight tool by default");

    mint::ToolRegistry retry_tools(workspace,
                                   mint::ToolRegistryOptions{.protected_paths = {inflight_path},
                                                             .allow_write = true,
                                                             .allowed_programs = {program},
                                                             .default_command_timeout_seconds = 5,
                                                             .max_command_timeout_seconds = 5});
    ResumeVerificationModel retry_model(program);
    std::ostringstream retry_log;
    mint::Agent retry_agent(retry_model, retry_tools, retry_log,
                            mint::AgentOptions{.max_turns = 2,
                                               .require_verification_after_write = true,
                                               .session_store = &inflight_session,
                                               .resume_session = true,
                                               .retry_in_flight_tool = true});
    const auto retry_result = retry_agent.run("");
    MINT_EXPECT(
        retry_result.completed && read_text(workspace / "README.md") == "# Fixed\n",
        "explicit retry authorization replays the in-flight tool and completes verification");
    write_text(workspace / "README.md", "# Broken\n");

    const auto legacy_path = temporary.path() / "legacy-v2-session.json";
    mint::SessionStore legacy_session(legacy_path);
    auto legacy_checkpoint = checkpoint;
    legacy_checkpoint["schema_version"] = 2;
    legacy_checkpoint.erase("in_flight_tool_call");
    legacy_checkpoint.erase("model");
    legacy_checkpoint["capabilities"].erase("command_recipes");
    legacy_checkpoint["capabilities"].erase("policy_fingerprint");
    legacy_checkpoint["capabilities"].erase("approve_each_changeset");
    legacy_checkpoint["capabilities"].erase("tool_limits");
    legacy_checkpoint["execution"].erase("recipe_calls");
    legacy_checkpoint["execution"].erase("verification_commands");
    legacy_checkpoint["execution"].erase("last_command_verification_eligible");
    legacy_checkpoint["change_journal"] = {{"schema_version", 1}, {"entries", mint::Json::array()}};
    legacy_session.save(legacy_checkpoint);
    mint::ToolRegistry legacy_tools(workspace,
                                    mint::ToolRegistryOptions{.protected_paths = {legacy_path},
                                                              .allow_write = true,
                                                              .allowed_programs = {program},
                                                              .default_command_timeout_seconds = 5,
                                                              .max_command_timeout_seconds = 5});
    ResumeVerificationModel legacy_model(program);
    std::ostringstream legacy_log;
    mint::Agent legacy_agent(legacy_model, legacy_tools, legacy_log,
                             mint::AgentOptions{.max_turns = 2,
                                                .require_verification_after_write = true,
                                                .session_store = &legacy_session,
                                                .resume_session = true});
    const auto migrated = legacy_agent.run("");
    MINT_EXPECT(migrated.completed && legacy_session.load().at("schema_version") == 4,
                "current mint restores a v2 checkpoint and rewrites it as schema v4");
    write_text(workspace / "README.md", "# Broken\n");

    bool rejected_policy_downgrade = false;
    try {
        mint::ToolRegistry mismatched_tools(
            workspace,
            mint::ToolRegistryOptions{
                .protected_paths = {session_path},
                .allow_write = true,
                .allowed_programs = {program},
                .command_approval = [](const mint::CommandApprovalRequest&) { return true; }});
        ScriptedModel unused_model;
        std::ostringstream unused_log;
        mint::Agent mismatched_agent(unused_model, mismatched_tools, unused_log,
                                     mint::AgentOptions{.require_verification_after_write = true,
                                                        .session_store = &session,
                                                        .resume_session = true});
        (void)mismatched_agent.run("");
    } catch (const std::invalid_argument& error) {
        rejected_policy_downgrade = std::string(error.what()).find("能力授权") != std::string::npos;
    }
    MINT_EXPECT(rejected_policy_downgrade,
                "resume rejects a different per-command approval policy");

    mint::ToolRegistry resumed_tools(workspace,
                                     mint::ToolRegistryOptions{.protected_paths = {session_path},
                                                               .allow_write = true,
                                                               .allowed_programs = {program},
                                                               .default_command_timeout_seconds = 5,
                                                               .max_command_timeout_seconds = 5});
    ResumeVerificationModel resumed_model(program);
    std::ostringstream resumed_log;
    mint::Agent resumed_agent(resumed_model, resumed_tools, resumed_log,
                              mint::AgentOptions{.max_turns = 2,
                                                 .require_verification_after_write = true,
                                                 .session_store = &session,
                                                 .resume_session = true});
    const auto resumed = resumed_agent.run("");
    MINT_EXPECT(resumed.completed && resumed.status == "completed",
                "resumed session executes the pending patch and reaches completion");
    MINT_EXPECT(resumed.answer == "恢复后验证完成" && resumed.turns == 3,
                "resume preserves prior turns and continues the original conversation");
    MINT_EXPECT(resumed.execution.file_changes == 1 && resumed.execution.commands_passed == 1,
                "resume preserves and extends the execution summary");
    MINT_EXPECT(resumed.verification_status == "passed",
                "resumed task still satisfies the verification gate");
    MINT_EXPECT(read_text(workspace / "README.md") == "# Fixed\n",
                "restored pending patch changes the workspace exactly once");
    MINT_EXPECT(resumed.changes.unified_diff.find("-# Broken\n+# Fixed\n") != std::string::npos,
                "restored change journal preserves the original-to-final diff");
    MINT_EXPECT(session.load().at("status") == "completed",
                "final stable checkpoint records terminal completion");
}

TEST(ModelConfigTest, LoadsAndValidatesJson) {
    TemporaryDirectory temporary;
    const auto valid_path = temporary.path() / "valid.json";
    write_text(valid_path, R"({"api_url":"https://example.test/chat/completions",)"
                           R"("api_key":"secret","model":"example-model",)"
                           R"("connect_timeout_seconds":7,"request_timeout_seconds":42,)"
                           R"("max_retries":4,"retry_initial_delay_ms":123,)"
                           R"("max_completion_tokens":777})");

    const auto config = mint::load_chat_completions_config(valid_path);
    MINT_EXPECT(config.api_url == "https://example.test/chat/completions",
                "JSON config loads api_url");
    MINT_EXPECT(config.api_key == "secret", "JSON config loads api_key");
    MINT_EXPECT(config.model == "example-model", "JSON config loads model");
    MINT_EXPECT(config.connect_timeout_seconds == 7, "JSON config loads connect timeout");
    MINT_EXPECT(config.request_timeout_seconds == 42, "JSON config loads request timeout");
    MINT_EXPECT(config.max_retries == 4, "JSON config loads retry count");
    MINT_EXPECT(config.retry_initial_delay_ms == 123, "JSON config loads retry delay");
    MINT_EXPECT(config.max_completion_tokens == 777,
                "JSON config loads the completion token ceiling");
    MINT_EXPECT(config.adapter == mint::ModelAdapter::chat_completions && !config.stream,
                "legacy JSON config keeps the Chat Completions non-streaming defaults");

    const auto invalid_path = temporary.path() / "invalid.json";
    write_text(invalid_path, R"({"api_url":"https://example.test","api_key":"x"})");
    bool rejected_missing_model = false;
    try {
        (void)mint::load_chat_completions_config(invalid_path);
    } catch (const std::runtime_error& error) {
        rejected_missing_model = std::string(error.what()).find("model") != std::string::npos;
    }
    MINT_EXPECT(rejected_missing_model, "JSON config explains a missing model field");

    bool rejected_zero_timeout = false;
    try {
        mint::ChatCompletionsClient invalid_client(
            {.api_url = "https://example.test/chat/completions",
             .model = "example-model",
             .connect_timeout_seconds = 0,
             .request_timeout_seconds = 1});
    } catch (const std::invalid_argument&) {
        rejected_zero_timeout = true;
    }
    MINT_EXPECT(rejected_zero_timeout,
                "model client rejects non-positive timeout policy even without config loading");
}

TEST(ModelClientTest, RetriesWithServerDirectedBackoff) {
    const std::string transient_error = R"({"error":{"message":"transient test failure"}})";
    const std::string success =
        R"({"choices":[{"message":{"role":"assistant","content":"retry passed"}}],)"
        R"("usage":{"prompt_tokens":120,"completion_tokens":8,"total_tokens":128,)"
        R"("prompt_tokens_details":{"cached_tokens":96}}})";
    ScriptedHttpServer server(
        {{.status = 429,
          .headers = {{"Retry-After", "0.02"}, {"X-RateLimit-Reset-Tokens", "20ms"}},
          .body = transient_error},
         {.status = 503, .body = transient_error},
         {.body = success}});
    std::vector<mint::ModelProgress> progress;
    mint::ChatCompletionsClient client(
        {.api_url = server.url("/v1/chat/completions"),
         .model = "retry-test-model",
         .connect_timeout_seconds = 2,
         .request_timeout_seconds = 2,
         .max_retries = 2,
         .retry_initial_delay_ms = 1,
         .max_completion_tokens = 321,
         .progress = [&](const mint::ModelProgress& event) { progress.push_back(event); }});
    const auto started = std::chrono::steady_clock::now();
    const auto reply =
        client.complete(mint::Json::array({{{"role", "system"}, {"content", "test"}},
                                           {{"role", "user"}, {"content", "test retry"}}}),
                        mint::Json::array());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    server.wait();
    MINT_EXPECT(reply.text == "retry passed",
                "model client returns the successful response after transient failures");
    MINT_EXPECT(server.request_count() == 3,
                "model client retries exactly the configured transient failures");
    MINT_EXPECT(elapsed.count() >= 50,
                "model client honors the server-provided token reset delay for HTTP 429");
    for (std::size_t index = 0; index < server.request_count(); ++index) {
        MINT_EXPECT(server.request(index).find(R"("max_completion_tokens":321)") !=
                        std::string::npos,
                    "every model retry carries the configured completion token ceiling");
    }
    MINT_EXPECT(reply.usage.available && reply.usage.prompt_tokens == 120 &&
                    reply.usage.completion_tokens == 8 && reply.usage.cached_tokens == 96,
                "model client exposes prompt, completion and cached token usage");
    MINT_EXPECT(reply.metadata.adapter == "chat_completions" &&
                    reply.metadata.model == "retry-test-model" && reply.metadata.attempts == 3 &&
                    reply.metadata.retries == 2 && reply.metadata.http_status == 200 &&
                    reply.metadata.duration_ms >= 50,
                "model client exposes adapter, retry and latency metadata");
    MINT_EXPECT(progress.size() == 6 &&
                    progress.at(0).kind == mint::ModelProgressKind::attempt_started &&
                    progress.at(1).kind == mint::ModelProgressKind::retry_scheduled &&
                    progress.at(1).http_status == 429 && progress.at(1).delay_ms >= 50 &&
                    progress.at(2).kind == mint::ModelProgressKind::attempt_started &&
                    progress.at(3).kind == mint::ModelProgressKind::retry_scheduled &&
                    progress.at(3).http_status == 503 &&
                    progress.at(4).kind == mint::ModelProgressKind::attempt_started &&
                    progress.at(5).kind == mint::ModelProgressKind::request_succeeded &&
                    progress.at(5).http_status == 200 && progress.at(5).attempt == 3 &&
                    progress.at(5).max_attempts == 3,
                "model progress reports each attempt, retry delay and final response");
    const auto progress_json = mint::model_progress_to_json(progress.at(1));
    MINT_EXPECT(progress_json.at("kind") == "retry_scheduled" && progress_json.at("attempt") == 1 &&
                    progress_json.at("http_status") == 429,
                "model progress has a stable event-log JSON contract");
}

} // namespace

const std::filesystem::path& mint_test_executable_path() {
    return test_executable;
}

int run_change_transaction_lock_helper(int argc, char** argv);

#undef MINT_EXPECT

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--transaction-lock-helper") {
        return run_change_transaction_lock_helper(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--command-helper") {
        return run_command_helper(argc, argv);
    }

    std::error_code executable_error;
    test_executable = std::filesystem::weakly_canonical(argv[0], executable_error);
    if (executable_error || test_executable.empty()) {
        std::cerr << "could not resolve test executable path\n";
        return 1;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
