#include "mint/domain/task_policy.hpp"
#include "mint/task_runtime.hpp"
#include "mint/tools.hpp"

#include "test_executable.hpp"
#include "test_workspace.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

#define MINT_EXPECT(condition, message) EXPECT_TRUE(condition) << (message)

using mint::test::expect_failure;
using mint::test::read_text;
using mint::test::TemporaryDirectory;
using mint::test::write_text;

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

#if defined(__APPLE__)
class ScopedEnvironmentOverride final {
  public:
    ScopedEnvironmentOverride(std::string name, const std::filesystem::path& value)
        : name_(std::move(name)) {
        if (const auto* current = std::getenv(name_.c_str())) {
            previous_ = current;
        }
        if (::setenv(name_.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error("could not override " + name_);
        }
    }

    ~ScopedEnvironmentOverride() {
        if (previous_.has_value()) {
            (void)::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            (void)::unsetenv(name_.c_str());
        }
    }

    ScopedEnvironmentOverride(const ScopedEnvironmentOverride&) = delete;
    ScopedEnvironmentOverride& operator=(const ScopedEnvironmentOverride&) = delete;

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

bool has_command_temp_residue(const std::filesystem::path& workspace) {
    for (const auto& entry : std::filesystem::directory_iterator(workspace)) {
        if (entry.path().filename().string().rfind(".mint-command-tmp-", 0) == 0) {
            return true;
        }
    }
    return false;
}
#endif

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

#if defined(__APPLE__)
std::filesystem::path recorded_command_temp(const std::filesystem::path& marker,
                                            const std::filesystem::path& workspace) {
    const auto path = std::filesystem::path(read_text(marker)).lexically_normal();
    std::error_code error;
    const auto canonical_workspace = std::filesystem::canonical(workspace, error);
    if (error || path.parent_path() != canonical_workspace ||
        path.filename().string().rfind(".mint-command-tmp-", 0) != 0) {
        throw std::runtime_error("command temporary directory marker is invalid");
    }
    return path;
}
#endif

TEST(CommandRunnerTest, ExecutesAuthorizedCommands) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = mint_test_executable_path().generic_string();
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
    struct stat inherited_metadata = {};
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
    const auto program = mint_test_executable_path().generic_string();

#if defined(_WIN32)
    constexpr std::size_t file_size_limit = 0;
#else
    constexpr std::size_t file_size_limit = 1024;
#endif
    mint::ToolRuntimeSettings runtime;
    runtime.command_resources = {
        .cpu_seconds = 10,
        .memory_bytes = MINT_TEST_ADDRESS_SANITIZED ? 0 : std::size_t{128} * 1024 * 1024,
        .max_processes = 128,
        .file_size_bytes = file_size_limit};
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
    const auto program = mint_test_executable_path().generic_string();
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
              {"workspace_snapshot_entries", 50000},
              {"workspace_snapshot_bytes", 536870912},
              {"workspace_snapshot_text_bytes", 67108864},
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
                    policy.tool_limits.workspace_snapshot_entries == 50000 &&
                    policy.tool_limits.workspace_snapshot_bytes == 536870912 &&
                    policy.tool_limits.workspace_snapshot_text_bytes == 67108864 &&
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

TEST(CommandRunnerTest, EnforcesRuntimeControls) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = mint_test_executable_path().generic_string();
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

    auto cancelled_control = std::make_shared<mint::TaskControl>();
    mint::ToolRegistry cancelled_tools(
        workspace,
        mint::ToolRegistryOptions{.allowed_programs = {program},
                                  .task_control = cancelled_control,
                                  .command_approval = [](const mint::CommandApprovalRequest&) {
                                      return mint::ApprovalDecisionKind::run_cancelled;
                                  }});
    const auto cancelled = mint::Json::parse(cancelled_tools.execute(echo_call));
    MINT_EXPECT(cancelled.at("status") == "cancelled" && cancelled.at("cancelled").get<bool>() &&
                    cancelled.at("approval_decision_source") == "run_cancelled" &&
                    cancelled.at("duration_ms") == 0 &&
                    cancelled.at("error").get<std::string>().find("用户拒绝") ==
                        std::string::npos &&
                    cancelled_control->cancellation_requested(),
                "cancelled approval stops synchronously without becoming a user denial");

    mint::ToolRegistry unresolved_tools(
        workspace,
        mint::ToolRegistryOptions{.allowed_programs = {program},
                                  .command_approval = [](const mint::CommandApprovalRequest&) {
                                      return mint::ApprovalDecisionKind::interaction_closed;
                                  }});
    const auto unresolved = mint::Json::parse(unresolved_tools.execute(echo_call));
    MINT_EXPECT(unresolved.at("status") == "failed" &&
                    unresolved.at("approval_decision_source") == "interaction_closed" &&
                    unresolved.at("error").get<std::string>().find("用户拒绝") == std::string::npos,
                "closed approval channel is a protocol failure rather than a user denial");

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
    const auto program = mint_test_executable_path().generic_string();
    const auto protected_secret = workspace / "protected-secret.txt";
    write_text(protected_secret, "sandbox secret\n");
#if defined(__APPLE__)
    const auto host_temp = temporary.path() / "host-temp";
    std::filesystem::create_directories(host_temp);
    ScopedEnvironmentOverride tmpdir("TMPDIR", host_temp);
    ScopedEnvironmentOverride tmp("TMP", host_temp);
    ScopedEnvironmentOverride temp("TEMP", host_temp);
#endif
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

#if defined(__APPLE__)
    const auto lifecycle_directory = workspace / "build";
    std::filesystem::create_directories(lifecycle_directory);
    const auto lifecycle_arguments = [&](const std::filesystem::path& marker,
                                         std::string_view behavior) {
        return mint::Json::array({"--command-helper", "sandbox-runtime", workspace.generic_string(),
                                  marker.generic_string(), std::string(behavior)});
    };
    const auto expect_cleaned = [&](const std::filesystem::path& marker, std::string_view outcome) {
        ASSERT_TRUE(std::filesystem::exists(marker))
            << outcome << " command did not record its temporary directory";
        const auto scratch = recorded_command_temp(marker, workspace);
        MINT_EXPECT(!std::filesystem::exists(scratch),
                    std::string(outcome) + " command left its temporary directory behind");
        MINT_EXPECT(!has_command_temp_residue(workspace),
                    std::string(outcome) + " command left private temporary residue");
    };

    const auto success_marker = lifecycle_directory / "success-scratch.txt";
    const auto runtime = mint::Json::parse(tools.execute(
        {"sandbox-runtime",
         "run_command",
         {{"program", program}, {"args", lifecycle_arguments(success_marker, "complete")}}}));
    MINT_EXPECT(runtime.at("exit_code") == 0,
                "sandbox provides private temporary storage and writable /dev/null: " +
                    runtime.dump());
    expect_cleaned(success_marker, "completed");

    const auto timeout_marker = lifecycle_directory / "timeout-scratch.txt";
    const auto timed_out =
        mint::Json::parse(tools.execute({"sandbox-lifecycle-timeout",
                                         "run_command",
                                         {{"program", program},
                                          {"args", lifecycle_arguments(timeout_marker, "wait")},
                                          {"timeout_seconds", 1}}}));
    MINT_EXPECT(timed_out.at("status") == "timed_out" && timed_out.at("timed_out").get<bool>(),
                "sandbox reports a timed-out command: " + timed_out.dump());
    expect_cleaned(timeout_marker, "timed-out");

    const auto replacement_marker = lifecycle_directory / "replacement-scratch.txt";
    const auto replaced = mint::Json::parse(tools.execute(
        {"sandbox-lifecycle-replace",
         "run_command",
         {{"program", program}, {"args", lifecycle_arguments(replacement_marker, "replace")}}}));
    MINT_EXPECT(!replaced.at("ok").get<bool>() &&
                    replaced.at("error").get<std::string>().find("命令私有临时目录身份已变化") !=
                        std::string::npos,
                "sandbox fails closed when its scratch path is replaced: " + replaced.dump());
    ASSERT_TRUE(std::filesystem::exists(replacement_marker));
    const auto replacement = recorded_command_temp(replacement_marker, workspace);
    const auto moved_original =
        replacement.parent_path() / ("moved-" + replacement.filename().generic_string());
    MINT_EXPECT(std::filesystem::exists(replacement / "replacement-probe.txt") &&
                    read_text(replacement / "replacement-probe.txt") ==
                        "replacement scratch content\n",
                "fail-closed cleanup preserves the replacement object");
    MINT_EXPECT(std::filesystem::exists(moved_original / "original-probe.txt") &&
                    read_text(moved_original / "original-probe.txt") ==
                        "original scratch content\n",
                "fail-closed cleanup does not chase and delete the renamed original directory");
    std::error_code replacement_cleanup_error;
    (void)std::filesystem::remove_all(replacement, replacement_cleanup_error);
    MINT_EXPECT(!replacement_cleanup_error, "test cleanup removes the replacement directory");
    replacement_cleanup_error.clear();
    (void)std::filesystem::remove_all(moved_original, replacement_cleanup_error);
    MINT_EXPECT(!replacement_cleanup_error, "test cleanup removes the renamed original directory");
    MINT_EXPECT(!has_command_temp_residue(workspace),
                "replacement regression leaves no test-owned scratch residue after inspection");
#endif

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

} // namespace

#undef MINT_EXPECT
