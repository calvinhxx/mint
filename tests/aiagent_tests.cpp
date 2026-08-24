#include "aiagent/agent.hpp"
#include "aiagent/config.hpp"
#include "aiagent/domain/task_policy.hpp"
#include "aiagent/event_log.hpp"
#include "aiagent/model_client.hpp"
#include "aiagent/session_store.hpp"
#include "aiagent/task_runtime.hpp"
#include "aiagent/tools.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

std::filesystem::path test_executable;

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
    if (mode == "flood") {
        std::cout << std::string(4096, 'x');
        return 0;
    }
    if (mode == "environment") {
        if (std::getenv("AIAGENT_TEST_SECRET") != nullptr) {
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
#if defined(__APPLE__)
    if (mode == "network") {
        const auto descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor < 0) {
            return errno == EPERM ? 0 : 13;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(9);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const auto connected =
            ::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        const auto result_errno = errno;
        ::close(descriptor);
        return connected < 0 && result_errno == EPERM ? 0 : 14;
    }
#endif
    return 66;
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("aiagent-tests-" + std::to_string(stamp));
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

#if !defined(_WIN32)
class RetryHttpServer final {
  public:
    RetryHttpServer() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) {
            throw std::runtime_error("retry test could not create socket");
        }
        int reuse = 1;
        (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 4) != 0) {
            ::close(listener_);
            throw std::runtime_error("retry test could not bind loopback server");
        }
        socklen_t length = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(listener_);
            throw std::runtime_error("retry test could not inspect loopback port");
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~RetryHttpServer() {
        if (listener_ >= 0) {
            ::shutdown(listener_, SHUT_RDWR);
            ::close(listener_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    RetryHttpServer(const RetryHttpServer&) = delete;
    RetryHttpServer& operator=(const RetryHttpServer&) = delete;

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1/chat/completions";
    }

    [[nodiscard]] int requests() const noexcept {
        return requests_.load();
    }

    [[nodiscard]] bool saw_completion_limit() const noexcept {
        return saw_completion_limit_.load();
    }

  private:
    static void send_all(int descriptor, const std::string& response) {
        std::size_t offset = 0;
        while (offset < response.size()) {
            const auto sent =
                ::send(descriptor, response.data() + offset, response.size() - offset, 0);
            if (sent <= 0) {
                return;
            }
            offset += static_cast<std::size_t>(sent);
        }
    }

    static std::size_t request_content_length(const std::string& request) {
        const std::string header = "Content-Length:";
        const auto position = request.find(header);
        if (position == std::string::npos) {
            return 0;
        }
        const auto value_start = request.find_first_not_of(" \t", position + header.size());
        const auto value_end = request.find("\r\n", value_start);
        return static_cast<std::size_t>(
            std::stoul(request.substr(value_start, value_end - value_start)));
    }

    void serve() {
        for (int index = 0; index < 3; ++index) {
            pollfd ready{listener_, POLLIN, 0};
            if (::poll(&ready, 1, 5000) <= 0) {
                return;
            }
            const auto connection = ::accept(listener_, nullptr, nullptr);
            if (connection < 0) {
                return;
            }
            std::string request;
            std::array<char, 4096> buffer{};
            while (request.find("\r\n\r\n") == std::string::npos) {
                const auto received = ::recv(connection, buffer.data(), buffer.size(), 0);
                if (received <= 0) {
                    break;
                }
                request.append(buffer.data(), static_cast<std::size_t>(received));
            }
            const auto header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const auto expected_size = header_end + 4 + request_content_length(request);
                while (request.size() < expected_size) {
                    const auto received = ::recv(connection, buffer.data(), buffer.size(), 0);
                    if (received <= 0) {
                        break;
                    }
                    request.append(buffer.data(), static_cast<std::size_t>(received));
                }
            }
            if (request.find(R"("max_completion_tokens":321)") != std::string::npos) {
                saw_completion_limit_ = true;
            }
            ++requests_;
            const bool succeed = index == 2;
            const std::string body =
                succeed
                    ? R"({"choices":[{"message":{"role":"assistant","content":"retry passed"}}],)"
                      R"("usage":{"prompt_tokens":120,"completion_tokens":8,"total_tokens":128,)"
                      R"("prompt_tokens_details":{"cached_tokens":96}}})"
                    : R"({"error":{"message":"transient test failure"}})";
            const std::string status = index == 0
                                           ? "429 Too Many Requests"
                                           : (succeed ? "200 OK" : "503 Service Unavailable");
            const std::string rate_limit_headers =
                index == 0 ? "Retry-After: 0.02\r\nX-RateLimit-Reset-Tokens: 20ms\r\n" : "";
            const auto response =
                std::string("HTTP/1.1 ") + status + "\r\n" + rate_limit_headers +
                "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            send_all(connection, response);
            ::shutdown(connection, SHUT_RDWR);
            ::close(connection);
        }
    }

    int listener_ = -1;
    unsigned short port_ = 0;
    std::atomic<int> requests_{0};
    std::atomic<bool> saw_completion_limit_{false};
    std::thread thread_;
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

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("FAILED: " + message);
    }
}

bool has_entry(const aiagent::Json& entries, const std::string& path) {
    for (const auto& entry : entries) {
        if (entry.value("path", "") == path) {
            return true;
        }
    }
    return false;
}

class ScriptedModel final : public aiagent::ModelClient {
  public:
    aiagent::ModelReply complete(const aiagent::Json& messages,
                                 const aiagent::Json& tools) override {
        expect(tools.size() == 3, "agent exposes exactly three tools in v0.1");
        if (calls_++ == 0) {
            const aiagent::Json arguments = {{"path", "README.md"}};
            const aiagent::Json raw_call = {
                {"id", "test-read"},
                {"type", "function"},
                {"function", {{"name", "read_file"}, {"arguments", arguments.dump()}}}};
            return {.assistant_message = {{"role", "assistant"},
                                          {"content", nullptr},
                                          {"tool_calls", aiagent::Json::array({raw_call})}},
                    .text = {},
                    .tool_calls = {{"test-read", "read_file", arguments}}};
        }

        expect(messages.back().at("role") == "tool", "tool result is appended to context");
        const auto result = aiagent::Json::parse(messages.back().at("content").get<std::string>());
        expect(result.at("ok").get<bool>(), "tool result returned to model is successful");
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

class ContextBudgetModel final : public aiagent::ModelClient {
  public:
    aiagent::ModelReply complete(const aiagent::Json& messages, const aiagent::Json&) override {
        expect(messages.dump().size() <= 16 * 1024,
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
            expect(found_summary, "compacted context carries an explicit harness summary");
        }
        if (calls_++ < 3) {
            const auto id = "large-read-" + std::to_string(calls_);
            const aiagent::Json arguments = {{"path", "large.txt"}};
            const aiagent::Json raw_call = {
                {"id", id},
                {"type", "function"},
                {"function", {{"name", "read_file"}, {"arguments", arguments.dump()}}}};
            return {.assistant_message = {{"role", "assistant"},
                                          {"content", nullptr},
                                          {"tool_calls", aiagent::Json::array({raw_call})}},
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

class WritingModel final : public aiagent::ModelClient {
  public:
    aiagent::ModelReply complete(const aiagent::Json& messages,
                                 const aiagent::Json& tools) override {
        expect(tools.size() == 6, "write-enabled agent exposes six tools in v1.2");
        expect(messages.at(0).at("content").get<std::string>().find("apply_patch") !=
                   std::string::npos,
               "write-enabled system prompt explains apply_patch");

        if (calls_++ == 0) {
            const aiagent::Json arguments = {{"path", "README.md"},
                                             {"operation", "replace"},
                                             {"old_text", "# Before\n"},
                                             {"new_text", "# After\n"}};
            const aiagent::Json raw_call = {
                {"id", "test-patch"},
                {"type", "function"},
                {"function", {{"name", "apply_patch"}, {"arguments", arguments.dump()}}}};
            return {.assistant_message = {{"role", "assistant"},
                                          {"content", nullptr},
                                          {"tool_calls", aiagent::Json::array({raw_call})}},
                    .text = {},
                    .tool_calls = {{"test-patch", "apply_patch", arguments}}};
        }

        expect(messages.back().at("role") == "tool", "patch result is appended to context");
        const auto result = aiagent::Json::parse(messages.back().at("content").get<std::string>());
        expect(result.at("ok").get<bool>(), "patch result returned to model is successful");
        return {.assistant_message = {{"role", "assistant"}, {"content", "修改完成"}},
                .text = "修改完成",
                .tool_calls = {}};
    }

  private:
    int calls_ = 0;
};

class PatchThenVerifyModel final : public aiagent::ModelClient {
  public:
    explicit PatchThenVerifyModel(std::string program) : program_(std::move(program)) {}

    aiagent::ModelReply complete(const aiagent::Json& messages,
                                 const aiagent::Json& tools) override {
        expect(tools.size() == 7, "write-and-command agent exposes seven tools in v1.2");
        const auto system_prompt = messages.at(0).at("content").get<std::string>();
        expect(system_prompt.find("apply_patch") != std::string::npos,
               "validation system prompt explains apply_patch");
        expect(system_prompt.find("run_command") != std::string::npos,
               "validation system prompt explains run_command");

        if (calls_ == 0) {
            ++calls_;
            return tool_reply("e2e-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Broken\n"},
                               {"new_text", "# Fixed\n"}});
        }

        const auto previous =
            aiagent::Json::parse(messages.back().at("content").get<std::string>());
        expect(previous.at("ok").get<bool>(), "previous e2e tool result succeeded");

        if (calls_ == 1) {
            ++calls_;
            return tool_reply("e2e-verify", "run_command",
                              {{"program", program_},
                               {"args", aiagent::Json::array({"--command-helper", "verify",
                                                              "README.md", "# Fixed\n"})},
                               {"cwd", "."},
                               {"timeout_seconds", 5}});
        }

        expect(previous.at("status") == "exited", "verification command exited normally");
        expect(previous.at("exit_code") == 0, "verification command passed");
        expect(previous.at("output").get<std::string>().find("verification passed") !=
                   std::string::npos,
               "verification evidence is returned to the model");
        ++calls_;
        return {.assistant_message = {{"role", "assistant"}, {"content", "修改并验证完成"}},
                .text = "修改并验证完成",
                .tool_calls = {}};
    }

  private:
    static aiagent::ModelReply tool_reply(std::string id, std::string name,
                                          aiagent::Json arguments) {
        aiagent::Json raw_call = {{"id", id},
                                  {"type", "function"},
                                  {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
        aiagent::ModelReply reply;
        reply.assistant_message = {{"role", "assistant"},
                                   {"content", nullptr},
                                   {"tool_calls", aiagent::Json::array({raw_call})}};
        reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
        return reply;
    }

    std::string program_;
    int calls_ = 0;
};

class FailureThenRepairModel final : public aiagent::ModelClient {
  public:
    explicit FailureThenRepairModel(std::string program) : program_(std::move(program)) {}

    aiagent::ModelReply complete(const aiagent::Json& messages,
                                 const aiagent::Json& tools) override {
        expect(tools.size() == 7, "verification-gated agent exposes seven tools");
        const auto system_prompt = messages.at(0).at("content").get<std::string>();
        expect(system_prompt.find("Harness policy requires verification") != std::string::npos,
               "system prompt explains the required verification gate");

        switch (calls_++) {
        case 0:
            return tool_reply("retry-first-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Broken\n"},
                               {"new_text", "# Almost\n"}});
        case 1:
            expect(last_tool_result(messages).at("ok").get<bool>(),
                   "first patch succeeded before failed verification");
            return verification_call("retry-first-verify");
        case 2: {
            const auto failed = last_tool_result(messages);
            expect(failed.at("exit_code") == 9, "first verification exposes the expected failure");
            return final_reply("错误地提前结束");
        }
        case 3:
            expect(messages.back().at("role") == "user",
                   "harness gate appends a continuation requirement");
            expect(messages.back().at("content").get<std::string>().find("unverified changes") !=
                       std::string::npos,
                   "continuation requirement explains unverified changes");
            gate_seen_ = true;
            return tool_reply("retry-second-patch", "apply_patch",
                              {{"path", "README.md"},
                               {"operation", "replace"},
                               {"old_text", "# Almost\n"},
                               {"new_text", "# Fixed\n"}});
        case 4:
            expect(last_tool_result(messages).at("ok").get<bool>(),
                   "second patch succeeded after the gate");
            return verification_call("retry-second-verify");
        default: {
            const auto passed = last_tool_result(messages);
            expect(passed.at("exit_code") == 0, "second verification passes after the repair");
            return final_reply("失败后继续修复并验证完成");
        }
        }
    }

    [[nodiscard]] bool gate_seen() const noexcept {
        return gate_seen_;
    }

  private:
    static aiagent::Json last_tool_result(const aiagent::Json& messages) {
        expect(messages.back().at("role") == "tool",
               "script expects a tool result as the latest message");
        return aiagent::Json::parse(messages.back().at("content").get<std::string>());
    }

    static aiagent::ModelReply tool_reply(std::string id, std::string name,
                                          aiagent::Json arguments) {
        aiagent::Json raw_call = {{"id", id},
                                  {"type", "function"},
                                  {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
        aiagent::ModelReply reply;
        reply.assistant_message = {{"role", "assistant"},
                                   {"content", nullptr},
                                   {"tool_calls", aiagent::Json::array({raw_call})}};
        reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
        return reply;
    }

    aiagent::ModelReply verification_call(std::string id) const {
        return tool_reply(std::move(id), "run_command",
                          {{"program", program_},
                           {"args", aiagent::Json::array(
                                        {"--command-helper", "verify", "README.md", "# Fixed\n"})},
                           {"cwd", "."},
                           {"timeout_seconds", 5}});
    }

    static aiagent::ModelReply final_reply(std::string text) {
        return {.assistant_message = {{"role", "assistant"}, {"content", text}},
                .text = std::move(text),
                .tool_calls = {}};
    }

    std::string program_;
    int calls_ = 0;
    bool gate_seen_ = false;
};

class PassThenFailModel final : public aiagent::ModelClient {
  public:
    explicit PassThenFailModel(std::string program) : program_(std::move(program)) {}

    aiagent::ModelReply complete(const aiagent::Json& messages, const aiagent::Json&) override {
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
                               {"args", aiagent::Json::array({"--command-helper", "verify",
                                                              "README.md", "# Fixed\n"})},
                               {"cwd", "."},
                               {"timeout_seconds", 5}});
        case 2:
            expect(last_tool_result(messages).at("exit_code") == 0,
                   "initial verification passes before the later failure");
            return tool_reply("regression-fail", "run_command",
                              {{"program", program_},
                               {"args", aiagent::Json::array({"--command-helper", "fail"})},
                               {"cwd", "."},
                               {"timeout_seconds", 5}});
        default:
            expect(last_tool_result(messages).at("exit_code") == 7,
                   "later command exposes the regression failure");
            return {.assistant_message = {{"role", "assistant"}, {"content", "错误地忽略后续失败"}},
                    .text = "错误地忽略后续失败",
                    .tool_calls = {}};
        }
    }

  private:
    static aiagent::Json last_tool_result(const aiagent::Json& messages) {
        expect(messages.back().at("role") == "tool",
               "regression script expects the latest tool result");
        return aiagent::Json::parse(messages.back().at("content").get<std::string>());
    }

    static aiagent::ModelReply tool_reply(std::string id, std::string name,
                                          aiagent::Json arguments) {
        aiagent::Json raw_call = {{"id", id},
                                  {"type", "function"},
                                  {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
        aiagent::ModelReply reply;
        reply.assistant_message = {{"role", "assistant"},
                                   {"content", nullptr},
                                   {"tool_calls", aiagent::Json::array({raw_call})}};
        reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
        return reply;
    }

    std::string program_;
    int calls_ = 0;
};

aiagent::ModelReply model_tool_reply(std::string id, std::string name, aiagent::Json arguments) {
    aiagent::Json raw_call = {{"id", id},
                              {"type", "function"},
                              {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
    aiagent::ModelReply reply;
    reply.assistant_message = {{"role", "assistant"},
                               {"content", nullptr},
                               {"tool_calls", aiagent::Json::array({raw_call})}};
    reply.tool_calls.push_back({std::move(id), std::move(name), std::move(arguments)});
    return reply;
}

class SlowSessionPatchModel final : public aiagent::ModelClient {
  public:
    aiagent::ModelReply complete(const aiagent::Json&, const aiagent::Json&) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return model_tool_reply("session-patch", "apply_patch",
                                {{"path", "README.md"},
                                 {"operation", "replace"},
                                 {"old_text", "# Broken\n"},
                                 {"new_text", "# Fixed\n"}});
    }
};

class ResumeVerificationModel final : public aiagent::ModelClient {
  public:
    explicit ResumeVerificationModel(std::string program) : program_(std::move(program)) {}

    aiagent::ModelReply complete(const aiagent::Json& messages, const aiagent::Json&) override {
        expect(messages.back().at("role") == "tool",
               "resumed model receives the restored pending tool result");
        const auto result = aiagent::Json::parse(messages.back().at("content").get<std::string>());
        if (calls_++ == 0) {
            expect(result.at("ok").get<bool>(), "restored patch succeeds before verification");
            return model_tool_reply("session-verify", "run_command",
                                    {{"program", program_},
                                     {"args", aiagent::Json::array({"--command-helper", "verify",
                                                                    "README.md", "# Fixed\n"})},
                                     {"cwd", "."},
                                     {"timeout_seconds", 5}});
        }
        expect(result.at("exit_code") == 0, "resumed verification command passes");
        return {.assistant_message = {{"role", "assistant"}, {"content", "恢复后验证完成"}},
                .text = "恢复后验证完成",
                .tool_calls = {}};
    }

  private:
    std::string program_;
    int calls_ = 0;
};

class LongCommandModel final : public aiagent::ModelClient {
  public:
    explicit LongCommandModel(std::string program) : program_(std::move(program)) {}

    aiagent::ModelReply complete(const aiagent::Json&, const aiagent::Json&) override {
        if (calls_++ != 0) {
            throw std::runtime_error("cancelled agent must not call the model again");
        }
        return model_tool_reply("cancel-command", "run_command",
                                {{"program", program_},
                                 {"args", aiagent::Json::array({"--command-helper", "sleep"})},
                                 {"cwd", "."},
                                 {"timeout_seconds", 5}});
    }

  private:
    std::string program_;
    int calls_ = 0;
};

class DeniedVerificationModel final : public aiagent::ModelClient {
  public:
    explicit DeniedVerificationModel(std::string program) : program_(std::move(program)) {}

    aiagent::ModelReply complete(const aiagent::Json&, const aiagent::Json&) override {
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
                                     {"args", aiagent::Json::array({"--command-helper", "verify",
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

void test_read_only_tools() {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    std::filesystem::create_directories(workspace / ".git");
    write_text(workspace / "README.md", "# Demo Agent\nA tiny project.\n");
    write_text(workspace / "src" / "main.cpp", "int main() { return 0; }\n");
    write_text(workspace / "large.txt", std::string(24 * 1024, 'x'));
    write_text(workspace / "config.json", R"({"api_key":"must-not-leak"})");
    write_text(workspace / ".git" / "config", "token=must-not-leak\n");
    write_text(temporary.path() / "outside.txt", "secret\n");

    aiagent::ToolRegistry tools(
        workspace, aiagent::ToolRegistryOptions{.protected_paths = {workspace / "config.json"}});

    const auto listed = aiagent::Json::parse(
        tools.execute({"list", "list_files", {{"path", "."}, {"max_depth", 2}}}));
    expect(listed.at("ok").get<bool>(), "list_files succeeds");
    expect(has_entry(listed.at("entries"), "README.md"), "list_files sees README.md");
    expect(has_entry(listed.at("entries"), "src/main.cpp"), "list_files respects depth");
    expect(!has_entry(listed.at("entries"), "config.json"),
           "list_files hides the protected config file");
    expect(!has_entry(listed.at("entries"), ".git"),
           "list_files hides ignored metadata directories");

    const auto direct_git_list =
        aiagent::Json::parse(tools.execute({"git-list", "list_files", {{"path", ".git"}}}));
    expect(!direct_git_list.at("ok").get<bool>(),
           "list_files rejects a directly requested ignored directory");

    const auto read =
        aiagent::Json::parse(tools.execute({"read", "read_file", {{"path", "README.md"}}}));
    expect(read.at("ok").get<bool>(), "read_file succeeds");
    expect(read.at("content").get<std::string>().find("Demo Agent") != std::string::npos,
           "read_file returns text");

    const auto first_chunk =
        aiagent::Json::parse(tools.execute({"large-first", "read_file", {{"path", "large.txt"}}}));
    expect(first_chunk.at("ok").get<bool>() &&
               first_chunk.at("content").get<std::string>().size() == 16 * 1024 &&
               first_chunk.at("truncated").get<bool>(),
           "read_file defaults to a bounded 16 KiB chunk");
    const auto second_chunk =
        aiagent::Json::parse(tools.execute({"large-second",
                                            "read_file",
                                            {{"path", "large.txt"},
                                             {"offset", first_chunk.at("next_offset")},
                                             {"max_bytes", 16 * 1024}}}));
    expect(second_chunk.at("ok").get<bool>() && !second_chunk.at("truncated").get<bool>() &&
               second_chunk.at("content").get<std::string>().size() == 8 * 1024,
           "read_file continues from next_offset without resending the first chunk");

    const auto protected_config =
        aiagent::Json::parse(tools.execute({"config", "read_file", {{"path", "config.json"}}}));
    expect(!protected_config.at("ok").get<bool>(), "read_file rejects protected config");

    const auto git_config =
        aiagent::Json::parse(tools.execute({"git-config", "read_file", {{"path", ".git/config"}}}));
    expect(!git_config.at("ok").get<bool>(),
           "read_file rejects a direct path inside ignored metadata");

    const auto searched = aiagent::Json::parse(tools.execute(
        {"search", "search_text", {{"path", "."}, {"query", "agent"}, {"case_sensitive", false}}}));
    expect(searched.at("ok").get<bool>(), "search_text succeeds");
    expect(searched.at("hits").size() == 1, "search_text finds one case-insensitive hit");
    expect(searched.at("hits").at(0).at("line") == 1, "search_text reports line number");

    const auto secret_search = aiagent::Json::parse(
        tools.execute({"secret-search",
                       "search_text",
                       {{"path", "."}, {"query", "must-not-leak"}, {"case_sensitive", true}}}));
    expect(secret_search.at("hits").empty(), "search_text skips protected config");

    const auto git_search = aiagent::Json::parse(tools.execute(
        {"git-search", "search_text", {{"path", ".git"}, {"query", "must-not-leak"}}}));
    expect(!git_search.at("ok").get<bool>(), "search_text rejects a direct ignored metadata path");

    const auto escaped =
        aiagent::Json::parse(tools.execute({"escape", "read_file", {{"path", "../outside.txt"}}}));
    expect(!escaped.at("ok").get<bool>(), "path traversal is rejected");

    std::error_code symlink_error;
    std::filesystem::create_symlink(temporary.path() / "outside.txt", workspace / "outside-link",
                                    symlink_error);
    if (!symlink_error) {
        const auto followed_symlink = aiagent::Json::parse(
            tools.execute({"symlink", "read_file", {{"path", "outside-link"}}}));
        expect(!followed_symlink.at("ok").get<bool>(), "escaping symlink is rejected");
    }
}

void test_apply_patch_tool() {
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

    const aiagent::ToolCall replace_readme{"patch-readme",
                                           "apply_patch",
                                           {{"path", "README.md"},
                                            {"operation", "replace"},
                                            {"old_text", "beta\n"},
                                            {"new_text", "gamma\n"}}};

    aiagent::ToolRegistry read_only_tools(workspace);
    expect(read_only_tools.definitions().size() == 3,
           "write tool is hidden unless explicitly enabled");
    const auto disabled = aiagent::Json::parse(read_only_tools.execute(replace_readme));
    expect(!disabled.at("ok").get<bool>(), "apply_patch rejects missing write authorization");
    expect(read_text(workspace / "README.md") == "alpha\nbeta\n",
           "disabled apply_patch leaves the file unchanged");
    const auto disabled_changes = aiagent::Json::parse(read_only_tools.execute(
        {"disabled-changes", "workspace_changes", aiagent::Json::object()}));
    expect(!disabled_changes.at("ok").get<bool>(),
           "workspace_changes is unavailable without write authorization");

    aiagent::ToolRegistry tools(
        workspace, aiagent::ToolRegistryOptions{.protected_paths = {workspace / "config.json"},
                                                .allow_write = true});
    expect(tools.definitions().size() == 6,
           "write-enabled registry exposes patch, changeset and workspace changes");

    const auto replaced = aiagent::Json::parse(tools.execute(replace_readme));
    expect(replaced.at("ok").get<bool>(), "apply_patch replaces one exact block");
    expect(read_text(workspace / "README.md") == "alpha\ngamma\n",
           "replace writes the expected contents");

    const auto created =
        aiagent::Json::parse(tools.execute({"patch-create",
                                            "apply_patch",
                                            {{"path", "src/generated.cpp"},
                                             {"operation", "create"},
                                             {"new_text", "int generated() { return 42; }\n"}}}));
    expect(created.at("ok").get<bool>(), "apply_patch creates a new text file");
    expect(read_text(workspace / "src" / "generated.cpp") == "int generated() { return 42; }\n",
           "create writes the expected contents");

    const auto changes = aiagent::Json::parse(
        tools.execute({"changes", "workspace_changes", aiagent::Json::object()}));
    expect(changes.at("ok").get<bool>(), "workspace_changes succeeds");
    expect(changes.at("changed_files").size() == 2,
           "workspace_changes reports modified and created files");
    const auto diff = changes.at("diff").get<std::string>();
    expect(diff.find("--- a/README.md") != std::string::npos,
           "workspace_changes emits a modified-file header");
    expect(diff.find("-beta\n+gamma\n") != std::string::npos,
           "workspace_changes emits the exact text replacement");
    expect(diff.find("--- /dev/null\n+++ b/src/generated.cpp") != std::string::npos,
           "workspace_changes emits a created-file header");
    expect(!changes.at("diff_truncated").get<bool>(), "small workspace diff is not truncated");

    const auto overwrite = aiagent::Json::parse(tools.execute(
        {"patch-overwrite",
         "apply_patch",
         {{"path", "README.md"}, {"operation", "create"}, {"new_text", "overwritten\n"}}}));
    expect(!overwrite.at("ok").get<bool>(), "create never overwrites an existing file");

    const auto ambiguous = aiagent::Json::parse(tools.execute({"patch-ambiguous",
                                                               "apply_patch",
                                                               {{"path", "duplicate.txt"},
                                                                {"operation", "replace"},
                                                                {"old_text", "repeat"},
                                                                {"new_text", "changed"}}}));
    expect(!ambiguous.at("ok").get<bool>(), "replace rejects an ambiguous old_text");
    expect(read_text(workspace / "duplicate.txt") == "repeat\nrepeat\n",
           "ambiguous replacement leaves the file unchanged");

    const auto stale = aiagent::Json::parse(tools.execute({"patch-stale",
                                                           "apply_patch",
                                                           {{"path", "README.md"},
                                                            {"operation", "replace"},
                                                            {"old_text", "not present"},
                                                            {"new_text", "changed"}}}));
    expect(!stale.at("ok").get<bool>(), "replace detects stale file context");

    const auto invalid_encoding = aiagent::Json::parse(tools.execute({"patch-invalid-utf8",
                                                                      "apply_patch",
                                                                      {{"path", "invalid.txt"},
                                                                       {"operation", "replace"},
                                                                       {"old_text", "bad"},
                                                                       {"new_text", "good"}}}));
    expect(!invalid_encoding.at("ok").get<bool>(),
           "apply_patch rejects a non-UTF-8 source before writing");
    expect(read_text(workspace / "invalid.txt") == invalid_utf8,
           "encoding rejection leaves the original bytes unchanged");

    const auto protected_config =
        aiagent::Json::parse(tools.execute({"patch-config",
                                            "apply_patch",
                                            {{"path", "config.json"},
                                             {"operation", "replace"},
                                             {"old_text", "must-stay-secret"},
                                             {"new_text", "leaked"}}}));
    expect(!protected_config.at("ok").get<bool>(), "apply_patch rejects the protected config file");
    expect(read_text(workspace / "config.json").find("must-stay-secret") != std::string::npos,
           "protected config remains unchanged");

    const auto git_config = aiagent::Json::parse(tools.execute({"patch-git-config",
                                                                "apply_patch",
                                                                {{"path", ".git/config"},
                                                                 {"operation", "replace"},
                                                                 {"old_text", "must-stay-secret"},
                                                                 {"new_text", "leaked"}}}));
    expect(!git_config.at("ok").get<bool>(), "apply_patch rejects ignored repository metadata");
    expect(read_text(workspace / ".git" / "config").find("must-stay-secret") != std::string::npos,
           "ignored repository metadata remains unchanged");

    const auto escaped = aiagent::Json::parse(tools.execute({"patch-escape",
                                                             "apply_patch",
                                                             {{"path", "../outside.txt"},
                                                              {"operation", "replace"},
                                                              {"old_text", "outside"},
                                                              {"new_text", "escaped"}}}));
    expect(!escaped.at("ok").get<bool>(), "apply_patch rejects path traversal");
    expect(read_text(temporary.path() / "outside.txt") == "outside\n",
           "path traversal leaves outside files unchanged");

    const auto unsupported = aiagent::Json::parse(
        tools.execute({"patch-delete",
                       "apply_patch",
                       {{"path", "README.md"}, {"operation", "delete"}, {"new_text", ""}}}));
    expect(!unsupported.at("ok").get<bool>(), "v0.2 does not allow file deletion");

    std::error_code symlink_error;
    std::filesystem::create_symlink(temporary.path() / "outside.txt", workspace / "write-link",
                                    symlink_error);
    if (!symlink_error) {
        const auto symlink = aiagent::Json::parse(tools.execute({"patch-symlink",
                                                                 "apply_patch",
                                                                 {{"path", "write-link"},
                                                                  {"operation", "replace"},
                                                                  {"old_text", "outside"},
                                                                  {"new_text", "escaped"}}}));
        expect(!symlink.at("ok").get<bool>(), "apply_patch rejects symbolic links");
        expect(read_text(temporary.path() / "outside.txt") == "outside\n",
               "symbolic link rejection leaves outside files unchanged");
    }

    const auto journal_state = tools.workspace_change_state();
    aiagent::ToolRegistry restored_tools(workspace,
                                         aiagent::ToolRegistryOptions{.allow_write = true});
    restored_tools.restore_workspace_change_state(journal_state);
    expect(restored_tools.workspace_change_snapshot().at("changed_files").size() == 2,
           "change journal restores both stable file entries");

    write_text(workspace / "README.md", "externally changed\n");
    bool rejected_stale_session = false;
    try {
        aiagent::ToolRegistry stale_tools(workspace,
                                          aiagent::ToolRegistryOptions{.allow_write = true});
        stale_tools.restore_workspace_change_state(journal_state);
    } catch (const std::invalid_argument& error) {
        rejected_stale_session = std::string(error.what()).find("外部修改") != std::string::npos;
    }
    expect(rejected_stale_session,
           "change journal restore rejects files modified after the checkpoint");
}

void test_apply_changeset_tool() {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    std::filesystem::create_directories(workspace / "src");
    std::filesystem::create_directories(workspace / ".git");
    write_text(workspace / "src" / "alpha.txt", "alpha\n");
    write_text(workspace / "src" / "delete.txt", "delete me\n");
    write_text(workspace / "src" / "move.txt", "move me\n");
    write_text(workspace / ".git" / "config", "protected\n");

    bool approval_seen = false;
    aiagent::ToolRegistry tools(
        workspace,
        aiagent::ToolRegistryOptions{
            .allow_write = true,
            .allowed_write_paths = {"src"},
            .change_set_approval = [&](const aiagent::ChangeSetApprovalRequest& request) {
                approval_seen = request.paths.size() == 5 &&
                                request.unified_diff.find("src/alpha.txt") != std::string::npos &&
                                request.unified_diff.find("src/moved.txt") != std::string::npos;
                return true;
            }});

    const auto rejected_extra_field = aiagent::Json::parse(tools.execute(
        {"changeset-extra-field",
         "apply_changeset",
         {{"changes", aiagent::Json::array({{{"operation", "create"},
                                             {"path", "src/extra.txt"},
                                             {"new_text", "new\n"},
                                             {"old_text", "not valid for create"}}})}}}));
    expect(!rejected_extra_field.at("ok").get<bool>() &&
               !std::filesystem::exists(workspace / "src" / "extra.txt"),
           "changeset rejects operation fields outside the exact operation contract");

    const auto committed = aiagent::Json::parse(tools.execute(
        {"changeset-commit",
         "apply_changeset",
         {{"changes",
           aiagent::Json::array(
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
    expect(committed.at("ok").get<bool>() && committed.at("status") == "committed" &&
               committed.at("operation_count") == 4,
           "apply_changeset commits four validated operations together");
    expect(approval_seen, "changeset approval receives a bounded five-file diff preview");
    expect(read_text(workspace / "src" / "alpha.txt") == "beta\n" &&
               read_text(workspace / "src" / "new.txt") == "new\n",
           "changeset replace and create write exact content");
    expect(!std::filesystem::exists(workspace / "src" / "delete.txt") &&
               !std::filesystem::exists(workspace / "src" / "move.txt") &&
               read_text(workspace / "src" / "moved.txt") == "move me\n",
           "changeset delete and move produce exact final paths");

    const auto snapshot = tools.workspace_change_snapshot();
    expect(snapshot.at("changed_files").size() == 5,
           "change journal represents a move as one deletion and one creation");
    expect(snapshot.at("diff").get<std::string>().find("--- a/src/delete.txt\n+++ /dev/null") !=
               std::string::npos,
           "change journal emits deleted-file unified diff headers");
    expect(tools.workspace_change_state().at("schema_version") == 2,
           "v1.2 change journal persists file existence state");

    aiagent::ToolRegistry restored(
        workspace,
        aiagent::ToolRegistryOptions{.allow_write = true, .allowed_write_paths = {"src"}});
    restored.restore_workspace_change_state(tools.workspace_change_state());
    expect(restored.workspace_change_snapshot().at("changed_files").size() == 5,
           "change journal restores created, modified and deleted paths");

    const auto before_failed_validation = read_text(workspace / "src" / "alpha.txt");
    const auto rejected = aiagent::Json::parse(
        tools.execute({"changeset-prevalidation",
                       "apply_changeset",
                       {{"changes", aiagent::Json::array({{{"operation", "replace"},
                                                           {"path", "src/alpha.txt"},
                                                           {"old_text", "beta\n"},
                                                           {"new_text", "gamma\n"}},
                                                          {{"operation", "delete"},
                                                           {"path", "src/moved.txt"},
                                                           {"old_text", "stale\n"}}})}}}));
    expect(!rejected.at("ok").get<bool>() &&
               read_text(workspace / "src" / "alpha.txt") == before_failed_validation,
           "changeset validates every operation before writing the first file");

    bool denied_seen = false;
    aiagent::ToolRegistry denied_tools(
        workspace, aiagent::ToolRegistryOptions{
                       .allow_write = true,
                       .allowed_write_paths = {"src"},
                       .change_set_approval = [&](const aiagent::ChangeSetApprovalRequest&) {
                           denied_seen = true;
                           return false;
                       }});
    const auto denied = aiagent::Json::parse(
        denied_tools.execute({"changeset-denied",
                              "apply_changeset",
                              {{"changes", aiagent::Json::array({{{"operation", "create"},
                                                                  {"path", "src/denied.txt"},
                                                                  {"new_text", "denied\n"}}})}}}));
    expect(denied_seen && denied.at("status") == "denied" &&
               !std::filesystem::exists(workspace / "src" / "denied.txt"),
           "changeset approval denial performs no writes");

    const auto ignored = aiagent::Json::parse(
        tools.execute({"changeset-ignored",
                       "apply_changeset",
                       {{"changes", aiagent::Json::array({{{"operation", "replace"},
                                                           {"path", ".git/config"},
                                                           {"old_text", "protected\n"},
                                                           {"new_text", "changed\n"}}})}}}));
    expect(!ignored.at("ok").get<bool>() &&
               read_text(workspace / ".git" / "config") == "protected\n",
           "changeset cannot write ignored repository metadata");

#if !defined(_WIN32)
    const auto locked = workspace / "zlocked";
    std::filesystem::create_directories(locked);
    std::filesystem::permissions(
        locked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);
    aiagent::ToolRegistry rollback_tools(
        workspace, aiagent::ToolRegistryOptions{.allow_write = true,
                                                .allowed_write_paths = {"src", "zlocked"}});
    const auto rolled_back = aiagent::Json::parse(rollback_tools.execute(
        {"changeset-rollback",
         "apply_changeset",
         {{"changes", aiagent::Json::array({{{"operation", "replace"},
                                             {"path", "src/alpha.txt"},
                                             {"old_text", "beta\n"},
                                             {"new_text", "gamma\n"}},
                                            {{"operation", "create"},
                                             {"path", "zlocked/new.txt"},
                                             {"new_text", "cannot write\n"}}})}}}));
    std::filesystem::permissions(locked, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    expect(!rolled_back.at("ok").get<bool>() &&
               read_text(workspace / "src" / "alpha.txt") == "beta\n" &&
               !std::filesystem::exists(locked / "new.txt"),
           "a mid-commit filesystem failure restores every earlier file");
#endif
}

void test_command_runner() {
#if defined(_WIN32)
    return;
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = test_executable.generic_string();
    const aiagent::ToolCall echo_call{
        "command-echo",
        "run_command",
        {{"program", program},
         {"args", aiagent::Json::array({"--command-helper", "echo", "hello"})},
         {"cwd", "src"},
         {"timeout_seconds", 2}}};

    aiagent::ToolRegistry disabled_tools(workspace);
    const auto disabled = aiagent::Json::parse(disabled_tools.execute(echo_call));
    expect(!disabled.at("ok").get<bool>(), "run_command rejects missing user authorization");

    aiagent::ToolRegistry tools(workspace,
                                aiagent::ToolRegistryOptions{.allowed_programs = {program},
                                                             .default_command_timeout_seconds = 2,
                                                             .max_command_timeout_seconds = 5,
                                                             .max_command_output_bytes = 256});
    expect(tools.can_run_commands(), "command runner reports enabled capability");
    expect(tools.definitions().size() == 4,
           "command-only registry exposes run_command as the fourth tool");

    const auto echoed = aiagent::Json::parse(tools.execute(echo_call));
    expect(echoed.at("ok").get<bool>(), "approved command starts successfully");
    expect(echoed.at("status") == "exited", "approved command exits normally");
    expect(echoed.at("exit_code") == 0, "approved command returns exit code zero");
    expect(echoed.at("cwd") == "src", "command result reports relative cwd");
    expect(echoed.at("output").get<std::string>().find("arg=hello") != std::string::npos,
           "combined command output is captured");

    const auto failed = aiagent::Json::parse(tools.execute(
        {"command-fail",
         "run_command",
         {{"program", program}, {"args", aiagent::Json::array({"--command-helper", "fail"})}}}));
    expect(failed.at("ok").get<bool>(),
           "a non-zero child exit is still a successfully executed tool");
    expect(failed.at("status") == "exited", "failed command exits normally");
    expect(failed.at("exit_code") == 7, "non-zero exit code is preserved");
    expect(failed.at("output").get<std::string>().find("intentional command failure") !=
               std::string::npos,
           "stderr is captured with stdout");

    const auto truncated = aiagent::Json::parse(tools.execute(
        {"command-flood",
         "run_command",
         {{"program", program}, {"args", aiagent::Json::array({"--command-helper", "flood"})}}}));
    expect(truncated.at("exit_code") == 0, "large-output command still completes");
    expect(truncated.at("output_truncated").get<bool>(), "command output reports truncation");
    expect(truncated.at("output").get<std::string>().size() == 256,
           "captured output respects the byte limit");

    (void)::setenv("AIAGENT_TEST_SECRET", "must-not-reach-child", 1);
    const auto filtered_environment = aiagent::Json::parse(
        tools.execute({"command-environment",
                       "run_command",
                       {{"program", program},
                        {"args", aiagent::Json::array({"--command-helper", "environment"})}}}));
    (void)::unsetenv("AIAGENT_TEST_SECRET");
    expect(filtered_environment.at("exit_code") == 0,
           "command child receives the filtered environment");
    expect(filtered_environment.at("output").get<std::string>().find("environment filtered") !=
               std::string::npos,
           "unapproved environment values are not inherited");

    const auto timed_out = aiagent::Json::parse(
        tools.execute({"command-timeout",
                       "run_command",
                       {{"program", program},
                        {"args", aiagent::Json::array({"--command-helper", "sleep"})},
                        {"timeout_seconds", 1}}}));
    expect(timed_out.at("ok").get<bool>(), "timeout returns a structured command result");
    expect(timed_out.at("timed_out").get<bool>(), "timeout is reported explicitly");
    expect(timed_out.at("status") == "timed_out", "timeout has a distinct status");
    expect(timed_out.at("exit_code").is_null(), "timed-out command has no exit code");

    const auto escaped_cwd = aiagent::Json::parse(
        tools.execute({"command-escape", "run_command", {{"program", program}, {"cwd", ".."}}}));
    expect(!escaped_cwd.at("ok").get<bool>(), "run_command rejects cwd outside the workspace");

    const auto unapproved = aiagent::Json::parse(tools.execute(
        {"command-unapproved",
         "run_command",
         {{"program", "/bin/echo"}, {"args", aiagent::Json::array({"should-not-run"})}}}));
    expect(!unapproved.at("ok").get<bool>(),
           "run_command rejects programs not approved by the user");

    bool blocked_launcher = false;
    try {
        aiagent::ToolRegistry blocked(workspace,
                                      aiagent::ToolRegistryOptions{.allowed_programs = {"sh"}});
    } catch (const std::invalid_argument& error) {
        blocked_launcher = std::string(error.what()).find("不允许授权") != std::string::npos;
    }
    expect(blocked_launcher,
           "the command policy refuses common shells and general-purpose launchers");
#endif
}

void test_task_policy_and_command_recipes() {
#if defined(_WIN32)
    return;
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto policy_path = temporary.path() / "policy.json";
    const auto program = test_executable.generic_string();
    write_text(
        policy_path,
        aiagent::Json{{"schema_version", 1},
                      {"write_paths", aiagent::Json::array({"src"})},
                      {"recipes",
                       aiagent::Json::array(
                           {{{"name", "verify"},
                             {"description", "Run the deterministic verification helper"},
                             {"program", program},
                             {"args", aiagent::Json::array({"--command-helper", "echo", "recipe"})},
                             {"cwd", "src"},
                             {"timeout_seconds", 5},
                             {"verification", true}}})},
                      {"require_verification", true},
                      {"max_turns", 24},
                      {"max_context_bytes", 131072},
                      {"max_seconds", 900}}
            .dump(2));

    const auto policy = aiagent::load_task_policy(policy_path);
    expect(policy.write_paths.size() == 1 && policy.recipes.size() == 1 &&
               policy.recipes.front().verification && policy.require_verification,
           "task policy loads write paths, immutable recipes and verification contract");
    expect(policy.max_turns == 24 && policy.max_context_bytes == 131072 &&
               policy.max_seconds == 900 && !policy.fingerprint.empty(),
           "task policy loads bounded task budgets and a stable fingerprint");
    const auto second_load = aiagent::load_task_policy(policy_path);
    expect(second_load.fingerprint == policy.fingerprint,
           "equivalent policy content has a deterministic fingerprint");

    bool approval_shape_valid = false;
    aiagent::ToolRegistry tools(
        workspace, aiagent::ToolRegistryOptions{
                       .protected_paths = {policy.source_path},
                       .allow_write = true,
                       .allowed_write_paths = policy.write_paths,
                       .command_recipes = policy.recipes,
                       .policy_fingerprint = policy.fingerprint,
                       .command_approval = [&](const aiagent::CommandApprovalRequest& request) {
                           approval_shape_valid =
                               request.program == program && request.cwd == "src" &&
                               request.timeout_seconds == 5 &&
                               request.args ==
                                   std::vector<std::string>{"--command-helper", "echo", "recipe"};
                           return true;
                       }});
    expect(tools.uses_command_recipes() &&
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
    expect(has_recipe && !has_raw_command,
           "policy mode exposes run_recipe instead of model-controlled argv");

    const auto executed =
        aiagent::Json::parse(tools.execute({"recipe-run", "run_recipe", {{"recipe", "verify"}}}));
    expect(approval_shape_valid && executed.at("exit_code") == 0 &&
               executed.at("recipe") == "verify" &&
               executed.at("verification_eligible").get<bool>() &&
               executed.at("output").get<std::string>().find("arg=recipe") != std::string::npos,
           "recipe execution uses exactly the policy argv, cwd, timeout and verification marker");

    const auto override_attempt = aiagent::Json::parse(
        tools.execute({"recipe-override",
                       "run_recipe",
                       {{"recipe", "verify"}, {"args", aiagent::Json::array({"override"})}}}));
    expect(!override_attempt.at("ok").get<bool>(),
           "run_recipe rejects model attempts to override fixed arguments");

    const auto invalid_policy = temporary.path() / "invalid-policy.json";
    write_text(invalid_policy, R"({"schema_version":1,"unknown_capability":true})");
    bool rejected_unknown = false;
    try {
        (void)aiagent::load_task_policy(invalid_policy);
    } catch (const std::invalid_argument& error) {
        rejected_unknown = std::string(error.what()).find("未知字段") != std::string::npos;
    }
    expect(rejected_unknown, "task policy rejects unknown capability fields");

    const auto unverifiable_policy = temporary.path() / "unverifiable-policy.json";
    write_text(unverifiable_policy, aiagent::Json{{"schema_version", 1},
                                                  {"write_paths", aiagent::Json::array({"src"})},
                                                  {"recipes", aiagent::Json::array()},
                                                  {"require_verification", true}}
                                        .dump());
    bool rejected_unverifiable = false;
    try {
        (void)aiagent::load_task_policy(unverifiable_policy);
    } catch (const std::invalid_argument& error) {
        rejected_unverifiable =
            std::string(error.what()).find("verification=true") != std::string::npos;
    }
    expect(rejected_unverifiable, "verification policy requires a verification-eligible recipe");
#endif
}

void test_write_path_allowlist() {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "keep\n");

    aiagent::ToolRegistry tools(
        workspace, aiagent::ToolRegistryOptions{.allow_write = true,
                                                .allowed_write_paths = {"src", "FIX_REPORT.md"}});
    expect(tools.allowed_write_paths() == std::vector<std::string>({"src", "FIX_REPORT.md"}),
           "write allowlist exposes stable relative policy labels");

    const auto denied = aiagent::Json::parse(tools.execute({"scope-denied",
                                                            "apply_patch",
                                                            {{"path", "README.md"},
                                                             {"operation", "replace"},
                                                             {"old_text", "keep\n"},
                                                             {"new_text", "changed\n"}}}));
    expect(!denied.at("ok").get<bool>() && read_text(workspace / "README.md") == "keep\n",
           "write allowlist rejects an otherwise valid edit outside its scope");

    const auto allowed_file = aiagent::Json::parse(tools.execute(
        {"scope-report",
         "apply_patch",
         {{"path", "FIX_REPORT.md"}, {"operation", "create"}, {"new_text", "verified\n"}}}));
    expect(allowed_file.at("ok").get<bool>(),
           "write allowlist permits an exact not-yet-created file");

    const auto allowed_directory =
        aiagent::Json::parse(tools.execute({"scope-source",
                                            "apply_patch",
                                            {{"path", "src/generated.cpp"},
                                             {"operation", "create"},
                                             {"new_text", "int generated = 1;\n"}}}));
    expect(allowed_directory.at("ok").get<bool>(),
           "write allowlist permits descendants of an authorized existing directory");

    bool rejected_escape = false;
    try {
        aiagent::ToolRegistry invalid(
            workspace, aiagent::ToolRegistryOptions{.allow_write = true,
                                                    .allowed_write_paths = {"../outside.txt"}});
    } catch (const std::invalid_argument&) {
        rejected_escape = true;
    }
    expect(rejected_escape, "write allowlist rejects paths outside the workspace");
}

void test_command_runtime_controls() {
#if defined(_WIN32)
    return;
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = test_executable.generic_string();
    const aiagent::ToolCall echo_call{
        "approval-echo",
        "run_command",
        {{"program", program},
         {"args", aiagent::Json::array({"--command-helper", "echo", "approved"})},
         {"cwd", "src"},
         {"timeout_seconds", 2}}};

    bool approval_seen = false;
    bool approval_shape_valid = false;
    aiagent::ToolRegistry denied_tools(
        workspace, aiagent::ToolRegistryOptions{
                       .allowed_programs = {program},
                       .command_approval = [&](const aiagent::CommandApprovalRequest& request) {
                           approval_seen = true;
                           approval_shape_valid =
                               request.program == program && request.cwd == "src" &&
                               request.timeout_seconds == 2 && request.args.size() == 3 &&
                               request.args.back() == "approved";
                           return false;
                       }});
    const auto denied = aiagent::Json::parse(denied_tools.execute(echo_call));
    expect(approval_seen && approval_shape_valid,
           "per-command approval receives the exact argv, cwd and timeout");
    expect(!denied.at("ok").get<bool>() && denied.at("status") == "denied",
           "denied command returns a structured result without starting");

    aiagent::ToolRegistry approved_tools(
        workspace,
        aiagent::ToolRegistryOptions{
            .allowed_programs = {program},
            .command_approval = [](const aiagent::CommandApprovalRequest&) { return true; }});
    const auto approved = aiagent::Json::parse(approved_tools.execute(echo_call));
    expect(approved.at("exit_code") == 0, "approved per-command request starts normally");

    auto budget = std::make_shared<aiagent::TaskControl>(std::chrono::milliseconds(100));
    aiagent::ToolRegistry budget_tools(
        workspace, aiagent::ToolRegistryOptions{.allowed_programs = {program},
                                                .default_command_timeout_seconds = 5,
                                                .max_command_timeout_seconds = 5,
                                                .task_control = budget});
    const auto started = std::chrono::steady_clock::now();
    const auto timed_out = aiagent::Json::parse(
        budget_tools.execute({"task-budget",
                              "run_command",
                              {{"program", program},
                               {"args", aiagent::Json::array({"--command-helper", "sleep"})},
                               {"timeout_seconds", 5}}}));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    expect(timed_out.at("status") == "task_timed_out" && timed_out.at("task_timed_out").get<bool>(),
           "total task budget has a distinct command outcome");
    expect(elapsed < 1500, "total task budget terminates the running process group promptly");
#endif
}

void test_command_os_sandbox() {
#if !defined(__APPLE__)
    return;
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = test_executable.generic_string();
    const auto protected_secret = temporary.path() / "protected-secret.txt";
    write_text(protected_secret, "sandbox secret\n");
    aiagent::ToolRegistry tools(workspace,
                                aiagent::ToolRegistryOptions{.protected_paths = {protected_secret},
                                                             .allowed_programs = {program},
                                                             .require_command_sandbox = true});
    expect(tools.commands_are_os_sandboxed(), "command registry reports OS sandbox enforcement");
    expect(tools.command_sandbox_backend() == "macos-seatbelt",
           "macOS command sandbox backend is named in policy state");

    std::error_code path_error;
    const auto inside = std::filesystem::weakly_canonical(workspace / "inside.txt", path_error);
    expect(!path_error, "sandbox test resolves inside path");
    const auto allowed = aiagent::Json::parse(tools.execute(
        {"sandbox-inside",
         "run_command",
         {{"program", program},
          {"args",
           aiagent::Json::array({"--command-helper", "write", inside.generic_string()})}}}));
    expect(allowed.at("sandboxed").get<bool>() && allowed.at("sandbox_backend") == "macos-seatbelt",
           "command result carries auditable sandbox metadata");
    expect(allowed.at("exit_code") == 0 && std::filesystem::exists(inside),
           "sandbox permits writes inside the workspace");

    const auto outside =
        std::filesystem::weakly_canonical(temporary.path() / "outside.txt", path_error);
    expect(!path_error, "sandbox test resolves outside path");
    const auto blocked_write = aiagent::Json::parse(tools.execute(
        {"sandbox-outside",
         "run_command",
         {{"program", program},
          {"args",
           aiagent::Json::array({"--command-helper", "write", outside.generic_string()})}}}));
    expect(blocked_write.at("exit_code") != 0 && !std::filesystem::exists(outside),
           "sandbox blocks writes outside the workspace");

    const auto blocked_read = aiagent::Json::parse(tools.execute(
        {"sandbox-protected-read",
         "run_command",
         {{"program", program},
          {"args",
           aiagent::Json::array({"--command-helper", "verify", protected_secret.generic_string(),
                                 "sandbox secret\n"})}}}));
    expect(blocked_read.at("exit_code") != 0,
           "sandbox blocks command reads of protected runtime files");

    const auto blocked_network = aiagent::Json::parse(tools.execute(
        {"sandbox-network",
         "run_command",
         {{"program", program}, {"args", aiagent::Json::array({"--command-helper", "network"})}}}));
    expect(blocked_network.at("exit_code") == 0, "sandbox blocks network socket access with EPERM");
#endif
}

void test_agent_loop() {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Loop test\n");

    aiagent::ToolRegistry tools(workspace);
    ScriptedModel model;
    std::ostringstream log;
    aiagent::Agent agent(model, tools, log);

    const auto result = agent.run("读取 README 后回答");
    expect(result.completed, "agent reaches a final answer");
    expect(result.answer == "完成", "agent returns model final answer");
    expect(result.turns == 2, "agent performs tool turn then final turn");
    expect(result.execution.tool_calls == 1, "agent summary counts the read tool call");
    expect(result.execution.successful_tool_calls == 1,
           "agent summary counts the successful read tool");
    expect(result.model.calls == 2 && result.model.attempts == 2 &&
               result.model.usage_reports == 1 && result.model.total_tokens == 110 &&
               result.model.cached_tokens == 80,
           "agent aggregates model attempts and token usage across turns");
    expect(log.str().find("read_file") != std::string::npos, "agent log shows tool call");
    expect(log.str().find("缓存 80，命中 80%") != std::string::npos,
           "agent log shows observable prompt cache usage");
}

void test_agent_context_budget() {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "large.txt", std::string(128 * 1024, 'x'));

    aiagent::ToolRegistry tools(workspace);
    ContextBudgetModel model;
    std::ostringstream output;
    aiagent::Agent agent(model, tools, output,
                         aiagent::AgentOptions{.max_turns = 6, .max_context_bytes = 16 * 1024});
    const auto result = agent.run("重复读取大文件并验证上下文压缩");
    expect(result.completed && result.answer == "上下文预算通过",
           "agent completes after multiple compacted large tool results");
    expect(result.execution.tool_calls == 3,
           "context compaction does not alter executed tool history");
}

void test_event_log_and_machine_result() {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto event_path = temporary.path() / "events.jsonl";
    write_text(workspace / "README.md", "# Event secret body\n");

    aiagent::ToolRegistry tools(workspace);
    aiagent::DemoModelClient model;
    aiagent::EventLog events(event_path);
    std::ostringstream log;
    aiagent::Agent agent(model, tools, log, aiagent::AgentOptions{.event_log = &events});
    const auto result = agent.run("读取 README 后回答");
    const auto machine = aiagent::agent_result_to_json(result);
    expect(machine.at("schema_version") == 1 && machine.at("status") == "completed" &&
               machine.at("completed").get<bool>(),
           "machine result has a versioned completion contract");
    expect(machine.at("execution").at("tool_calls") == 3,
           "machine result exposes the execution summary");
    expect(machine.at("verification_status") == "not_required",
           "machine result exposes explicit verification state");

    std::ifstream input(event_path, std::ios::binary);
    std::string raw{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    expect(raw.find("Event secret body") == std::string::npos &&
               raw.find("\"query\":\"Agent\"") == std::string::npos,
           "JSONL trace omits file contents, search text and raw tool output");
    std::istringstream lines(raw);
    std::string line;
    std::size_t expected_sequence = 1;
    std::vector<std::string> types;
    while (std::getline(lines, line)) {
        const auto event = aiagent::Json::parse(line);
        expect(event.at("schema_version") == 1, "every JSONL event carries its schema version");
        expect(event.at("seq") == expected_sequence++, "JSONL event sequence is monotonic");
        types.push_back(event.at("type").get<std::string>());
    }
    expect(!types.empty() && types.front() == "task_started" && types.back() == "task_finished",
           "event trace brackets the complete task lifecycle");
    expect(std::find(types.begin(), types.end(), "tool_started") != types.end() &&
               std::find(types.begin(), types.end(), "tool_completed") != types.end(),
           "event trace records sanitized tool lifecycle events");
}

void test_runtime_file_guards() {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "existing.txt";
    write_text(target, "must not be overwritten\n");

    const auto append_path = temporary.path() / "append-events.jsonl";
    {
        aiagent::EventLog first(append_path);
        first.emit("first");
    }
    {
        aiagent::EventLog second(append_path, true);
        second.emit("second");
    }
    std::ifstream appended_input(append_path, std::ios::binary);
    std::string first_line;
    std::string second_line;
    std::getline(appended_input, first_line);
    std::getline(appended_input, second_line);
    expect(aiagent::Json::parse(first_line).at("seq") == 1 &&
               aiagent::Json::parse(second_line).at("seq") == 2,
           "resumed JSONL logging continues the prior sequence");

    const auto partial_path = temporary.path() / "partial-events.jsonl";
    write_text(partial_path, R"({"schema_version":1,"seq":4,"type":"stable","data":{}})"
                             "\n{\"schema_version\":1,\"seq\":5");
    {
        aiagent::EventLog recovered(partial_path, true);
        recovered.emit("after_crash");
    }
    std::ifstream recovered_input(partial_path, std::ios::binary);
    std::string recovered_line;
    std::vector<std::string> recovered_lines;
    while (std::getline(recovered_input, recovered_line)) {
        recovered_lines.push_back(recovered_line);
    }
    expect(recovered_lines.size() == 3 &&
               aiagent::Json::parse(recovered_lines.back()).at("seq") == 5,
           "JSONL recovery separates a crash-truncated final line from new events");

    std::error_code symlink_error;
    const auto event_link = temporary.path() / "events-link.jsonl";
    std::filesystem::create_symlink(target, event_link, symlink_error);
    if (!symlink_error) {
        bool event_rejected = false;
        bool session_rejected = false;
        try {
            aiagent::EventLog events(event_link);
        } catch (const std::invalid_argument&) {
            event_rejected = true;
        }
        const auto session_link = temporary.path() / "session-link.json";
        std::filesystem::create_symlink(target, session_link, symlink_error);
        try {
            aiagent::SessionStore session(session_link);
        } catch (const std::invalid_argument&) {
            session_rejected = true;
        }
        expect(event_rejected && session_rejected, "runtime files reject symbolic-link targets");
        expect(read_text(target) == "must not be overwritten\n",
               "runtime symlink rejection leaves the target unchanged");
    }

    std::error_code hard_link_error;
    const auto hard_link = temporary.path() / "events-hardlink.jsonl";
    std::filesystem::create_hard_link(target, hard_link, hard_link_error);
    if (!hard_link_error) {
        bool rejected = false;
        try {
            aiagent::EventLog events(hard_link);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        expect(rejected, "event log rejects an existing file with multiple hard links");
        expect(read_text(target) == "must not be overwritten\n",
               "hard-link rejection leaves the shared inode unchanged");
    }

    const auto existing_directory = temporary.path() / "runtime-directory";
    std::filesystem::create_directory(existing_directory);
    bool event_directory_rejected = false;
    bool session_directory_rejected = false;
    try {
        aiagent::EventLog events(existing_directory);
    } catch (const std::invalid_argument&) {
        event_directory_rejected = true;
    }
    try {
        aiagent::SessionStore session(existing_directory);
    } catch (const std::invalid_argument&) {
        session_directory_rejected = true;
    }
    expect(event_directory_rejected && session_directory_rejected,
           "runtime output paths reject existing non-regular files");
}

void test_write_agent_loop() {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Before\n");

    aiagent::ToolRegistry tools(workspace, aiagent::ToolRegistryOptions{.allow_write = true});
    WritingModel model;
    std::ostringstream log;
    aiagent::Agent agent(model, tools, log);

    const auto result = agent.run("修改 README 标题");
    expect(result.completed, "write-enabled agent reaches a final answer");
    expect(result.answer == "修改完成", "write-enabled agent returns final answer");
    expect(read_text(workspace / "README.md") == "# After\n", "agent loop executes apply_patch");
    expect(result.execution.file_changes == 1, "write agent summary counts the file modification");
    expect(result.changes.files == std::vector<std::string>{"README.md"},
           "write agent result exposes the changed file list");
    expect(result.changes.unified_diff.find("-# Before\n+# After\n") != std::string::npos,
           "write agent result exposes the unified diff");
    expect(result.verification_status == "not_run",
           "unverified write is marked not_run when the gate is disabled");
    const auto write_log = log.str();
    expect(write_log.find("README.md") != std::string::npos, "patch log includes the target path");
    const auto before_final = write_log.substr(0, write_log.find("[最终回答]"));
    expect(before_final.find("# Before") == std::string::npos,
           "patch log does not dump old file contents");
    expect(before_final.find("# After") == std::string::npos,
           "patch log does not dump new file contents");
}

void test_patch_then_verify_agent_loop() {
#if defined(_WIN32)
    return;
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Broken\n");
    const auto program = test_executable.generic_string();

    aiagent::ToolRegistry tools(workspace,
                                aiagent::ToolRegistryOptions{.allow_write = true,
                                                             .allowed_programs = {program},
                                                             .default_command_timeout_seconds = 5,
                                                             .max_command_timeout_seconds = 5});
    PatchThenVerifyModel model(program);
    std::ostringstream log;
    aiagent::Agent agent(model, tools, log,
                         aiagent::AgentOptions{.require_verification_after_write = true});

    const auto result = agent.run("修复 README 并执行验证");
    expect(result.completed, "patch-then-verify agent reaches a final answer");
    expect(result.answer == "修改并验证完成",
           "patch-then-verify agent returns the verified answer");
    expect(result.turns == 3, "agent performs patch, verification command, then final answer");
    expect(result.execution.tool_calls == 2, "end-to-end summary counts patch and command tools");
    expect(result.execution.file_changes == 1, "end-to-end summary counts the patch");
    expect(result.execution.command_calls == 1,
           "end-to-end summary counts the verification command");
    expect(result.execution.commands_passed == 1,
           "end-to-end summary records the passing verification");
    expect(result.execution.commands_failed == 0, "end-to-end summary has no failed commands");
    expect(result.verification_status == "passed", "end-to-end result records passed verification");
    expect(result.changes.files == std::vector<std::string>{"README.md"},
           "end-to-end result lists the modified file");
    expect(read_text(workspace / "README.md") == "# Fixed\n",
           "end-to-end loop leaves the requested file change");
    expect(log.str().find("apply_patch") != std::string::npos,
           "end-to-end log records the patch tool");
    expect(log.str().find("run_command") != std::string::npos,
           "end-to-end log records the verification command");
    const auto e2e_log = log.str();
    const auto e2e_before_final = e2e_log.substr(0, e2e_log.find("[最终回答]"));
    expect(e2e_before_final.find("# Fixed") == std::string::npos,
           "end-to-end tool-call logs do not dump patch or command arguments");
    expect(e2e_log.find("+# Fixed") != std::string::npos,
           "end-to-end final state prints the audited diff");
#endif
}

void test_failed_verification_requires_repair() {
#if defined(_WIN32)
    return;
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Broken\n");
    const auto program = test_executable.generic_string();

    aiagent::ToolRegistry tools(workspace,
                                aiagent::ToolRegistryOptions{.allow_write = true,
                                                             .allowed_programs = {program},
                                                             .default_command_timeout_seconds = 5,
                                                             .max_command_timeout_seconds = 5});
    FailureThenRepairModel model(program);
    std::ostringstream log;
    aiagent::Agent agent(
        model, tools, log,
        aiagent::AgentOptions{.max_turns = 8, .require_verification_after_write = true});

    const auto result = agent.run("修复 README；验证失败时继续修复");
    expect(result.completed, "verification-gated agent eventually completes");
    expect(result.answer == "失败后继续修复并验证完成", "premature final answer is not accepted");
    expect(result.turns == 6,
           "agent uses patch, failed verify, rejected final, patch, pass, final");
    expect(model.gate_seen(), "scripted model receives the harness continuation requirement");
    expect(result.execution.file_changes == 2, "execution summary counts both repair attempts");
    expect(result.execution.command_calls == 2,
           "execution summary counts both verification attempts");
    expect(result.execution.commands_failed == 1,
           "execution summary records the first failed verification");
    expect(result.execution.commands_passed == 1,
           "execution summary records the final passing verification");
    expect(result.verification_status == "passed", "final verification status is passed");
    expect(read_text(workspace / "README.md") == "# Fixed\n",
           "second repair leaves the verified contents");
    expect(result.changes.files == std::vector<std::string>{"README.md"},
           "change journal collapses repeated edits into one file");
    expect(result.changes.unified_diff.find("-# Broken\n+# Fixed\n") != std::string::npos,
           "final diff compares the original baseline with verified contents");
    expect(result.changes.unified_diff.find("Almost") == std::string::npos,
           "intermediate failed contents do not pollute the final diff");
    expect(log.str().find("[继续]") != std::string::npos,
           "console trace records the rejected premature completion");
#endif
}

void test_latest_command_controls_verification_status() {
#if defined(_WIN32)
    return;
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    write_text(workspace / "README.md", "# Broken\n");
    const auto program = test_executable.generic_string();

    aiagent::ToolRegistry tools(workspace,
                                aiagent::ToolRegistryOptions{.allow_write = true,
                                                             .allowed_programs = {program},
                                                             .default_command_timeout_seconds = 5,
                                                             .max_command_timeout_seconds = 5});
    PassThenFailModel model(program);
    std::ostringstream log;
    aiagent::Agent agent(
        model, tools, log,
        aiagent::AgentOptions{.max_turns = 4, .require_verification_after_write = true});

    const auto result = agent.run("修改后先验证成功，再模拟一次回归失败");
    expect(!result.completed, "a later failed command prevents completion despite an earlier pass");
    expect(result.verification_status == "failed",
           "the latest post-write command controls verification status");
    expect(result.execution.commands_passed == 1, "regression scenario records the initial pass");
    expect(result.execution.commands_failed == 1, "regression scenario records the later failure");
    expect(log.str().find("[继续]") != std::string::npos,
           "verification gate rejects completion after the later failure");
#endif
}

void test_denied_command_cannot_satisfy_verification() {
#if defined(_WIN32)
    return;
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = test_executable.generic_string();
    write_text(workspace / "README.md", "# Broken\n");

    aiagent::ToolRegistry tools(
        workspace,
        aiagent::ToolRegistryOptions{
            .allow_write = true,
            .allowed_programs = {program},
            .default_command_timeout_seconds = 5,
            .max_command_timeout_seconds = 5,
            .command_approval = [](const aiagent::CommandApprovalRequest&) { return false; }});
    DeniedVerificationModel model(program);
    std::ostringstream log;
    aiagent::Agent agent(
        model, tools, log,
        aiagent::AgentOptions{.max_turns = 3, .require_verification_after_write = true});
    const auto result = agent.run("修改并申请运行验证");
    expect(!result.completed && result.status == "max_turns",
           "approval denial prevents the model from ending the task");
    expect(result.verification_status == "denied",
           "verification state distinguishes approval denial from test failure");
    expect(result.execution.commands_denied == 1,
           "execution summary counts the denied command separately");
    expect(log.str().find("[继续]") != std::string::npos,
           "verification gate requests continuation after denial");
#endif
}

void test_agent_cancellation_stops_running_command() {
#if defined(_WIN32)
    return;
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto program = test_executable.generic_string();
    auto control = std::make_shared<aiagent::TaskControl>();

    aiagent::ToolRegistry tools(workspace,
                                aiagent::ToolRegistryOptions{.allowed_programs = {program},
                                                             .default_command_timeout_seconds = 5,
                                                             .max_command_timeout_seconds = 5,
                                                             .task_control = control});
    LongCommandModel model(program);
    std::ostringstream log;
    aiagent::Agent agent(model, tools, log,
                         aiagent::AgentOptions{.max_turns = 4, .task_control = control});

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

    expect(!result.completed && result.status == "cancelled",
           "agent exposes an explicit cancelled terminal state");
    expect(result.stop_reason == "user_cancelled", "cancelled result records the stop reason");
    expect(result.execution.commands_cancelled == 1,
           "execution summary counts the cancelled child command");
    expect(elapsed < 1500, "agent cancellation terminates the child process group promptly");
    const auto machine = aiagent::agent_result_to_json(result);
    expect(machine.at("status") == "cancelled" && !machine.at("completed").get<bool>(),
           "machine result preserves cancellation without pretending completion");
#endif
}

void test_session_checkpoint_and_resume() {
#if defined(_WIN32)
    return;
#else
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto session_path = temporary.path() / "session.json";
    const auto program = test_executable.generic_string();
    write_text(workspace / "README.md", "# Broken\n");
    aiagent::SessionStore session(session_path);

    auto short_budget = std::make_shared<aiagent::TaskControl>(std::chrono::milliseconds(10));
    aiagent::ToolRegistry first_tools(
        workspace, aiagent::ToolRegistryOptions{.protected_paths = {session_path},
                                                .allow_write = true,
                                                .allowed_programs = {program},
                                                .default_command_timeout_seconds = 5,
                                                .max_command_timeout_seconds = 5,
                                                .task_control = short_budget});
    SlowSessionPatchModel first_model;
    std::ostringstream first_log;
    aiagent::Agent first_agent(first_model, first_tools, first_log,
                               aiagent::AgentOptions{.max_turns = 1,
                                                     .require_verification_after_write = true,
                                                     .task_control = short_budget,
                                                     .session_store = &session});
    const auto interrupted = first_agent.run("修复 README 并验证");
    expect(interrupted.status == "timed_out" && !interrupted.completed,
           "task budget stops the first invocation explicitly");
    expect(read_text(workspace / "README.md") == "# Broken\n",
           "pending tool is checkpointed before execution when budget expires");
    const auto checkpoint = session.load();
    expect(checkpoint.at("schema_version") == 3 && checkpoint.at("status") == "timed_out" &&
               checkpoint.at("pending_tool_calls").size() == 1 &&
               checkpoint.at("in_flight_tool_call").is_null(),
           "session stores the pending call at a stable resume point");

    const auto incomplete_path = temporary.path() / "incomplete-v3-session.json";
    aiagent::SessionStore incomplete_session(incomplete_path);
    auto incomplete_checkpoint = checkpoint;
    incomplete_checkpoint.erase("in_flight_tool_call");
    incomplete_session.save(incomplete_checkpoint);
    bool rejected_incomplete_v3 = false;
    try {
        aiagent::ToolRegistry incomplete_tools(
            workspace, aiagent::ToolRegistryOptions{.protected_paths = {incomplete_path},
                                                    .allow_write = true,
                                                    .allowed_programs = {program},
                                                    .default_command_timeout_seconds = 5,
                                                    .max_command_timeout_seconds = 5});
        SlowSessionPatchModel unused_model;
        std::ostringstream unused_log;
        aiagent::Agent incomplete_agent(
            unused_model, incomplete_tools, unused_log,
            aiagent::AgentOptions{.require_verification_after_write = true,
                                  .session_store = &incomplete_session,
                                  .resume_session = true});
        (void)incomplete_agent.run("");
    } catch (const std::invalid_argument& error) {
        rejected_incomplete_v3 =
            std::string(error.what()).find("缺少必需状态") != std::string::npos;
    }
    expect(rejected_incomplete_v3,
           "schema v3 rejects a checkpoint missing its durable in-flight marker");

    const auto inflight_path = temporary.path() / "inflight-session.json";
    aiagent::SessionStore inflight_session(inflight_path);
    auto inflight_checkpoint = checkpoint;
    inflight_checkpoint["status"] = "running";
    inflight_checkpoint["in_flight_tool_call"] = inflight_checkpoint.at("pending_tool_calls").at(0);
    inflight_session.save(inflight_checkpoint);

    bool blocked_inflight_replay = false;
    try {
        aiagent::ToolRegistry blocked_tools(
            workspace, aiagent::ToolRegistryOptions{.protected_paths = {inflight_path},
                                                    .allow_write = true,
                                                    .allowed_programs = {program},
                                                    .default_command_timeout_seconds = 5,
                                                    .max_command_timeout_seconds = 5});
        ResumeVerificationModel unused_model(program);
        std::ostringstream unused_log;
        aiagent::Agent blocked_agent(unused_model, blocked_tools, unused_log,
                                     aiagent::AgentOptions{.max_turns = 2,
                                                           .require_verification_after_write = true,
                                                           .session_store = &inflight_session,
                                                           .resume_session = true});
        (void)blocked_agent.run("");
    } catch (const std::runtime_error& error) {
        blocked_inflight_replay =
            std::string(error.what()).find("--retry-inflight") != std::string::npos;
    }
    expect(blocked_inflight_replay,
           "resume blocks an ambiguous side-effecting in-flight tool by default");

    aiagent::ToolRegistry retry_tools(
        workspace, aiagent::ToolRegistryOptions{.protected_paths = {inflight_path},
                                                .allow_write = true,
                                                .allowed_programs = {program},
                                                .default_command_timeout_seconds = 5,
                                                .max_command_timeout_seconds = 5});
    ResumeVerificationModel retry_model(program);
    std::ostringstream retry_log;
    aiagent::Agent retry_agent(retry_model, retry_tools, retry_log,
                               aiagent::AgentOptions{.max_turns = 2,
                                                     .require_verification_after_write = true,
                                                     .session_store = &inflight_session,
                                                     .resume_session = true,
                                                     .retry_in_flight_tool = true});
    const auto retry_result = retry_agent.run("");
    expect(retry_result.completed && read_text(workspace / "README.md") == "# Fixed\n",
           "explicit retry authorization replays the in-flight tool and completes verification");
    write_text(workspace / "README.md", "# Broken\n");

    const auto legacy_path = temporary.path() / "legacy-v2-session.json";
    aiagent::SessionStore legacy_session(legacy_path);
    auto legacy_checkpoint = checkpoint;
    legacy_checkpoint["schema_version"] = 2;
    legacy_checkpoint.erase("in_flight_tool_call");
    legacy_checkpoint.erase("model");
    legacy_checkpoint["capabilities"].erase("command_recipes");
    legacy_checkpoint["capabilities"].erase("policy_fingerprint");
    legacy_checkpoint["capabilities"].erase("approve_each_changeset");
    legacy_checkpoint["execution"].erase("recipe_calls");
    legacy_checkpoint["execution"].erase("verification_commands");
    legacy_checkpoint["execution"].erase("last_command_verification_eligible");
    legacy_checkpoint["change_journal"] = {{"schema_version", 1},
                                           {"entries", aiagent::Json::array()}};
    legacy_session.save(legacy_checkpoint);
    aiagent::ToolRegistry legacy_tools(
        workspace, aiagent::ToolRegistryOptions{.protected_paths = {legacy_path},
                                                .allow_write = true,
                                                .allowed_programs = {program},
                                                .default_command_timeout_seconds = 5,
                                                .max_command_timeout_seconds = 5});
    ResumeVerificationModel legacy_model(program);
    std::ostringstream legacy_log;
    aiagent::Agent legacy_agent(legacy_model, legacy_tools, legacy_log,
                                aiagent::AgentOptions{.max_turns = 2,
                                                      .require_verification_after_write = true,
                                                      .session_store = &legacy_session,
                                                      .resume_session = true});
    const auto migrated = legacy_agent.run("");
    expect(migrated.completed && legacy_session.load().at("schema_version") == 3,
           "v1.2 restores a v2 checkpoint and rewrites it as schema v3");
    write_text(workspace / "README.md", "# Broken\n");

    bool rejected_policy_downgrade = false;
    try {
        aiagent::ToolRegistry mismatched_tools(
            workspace,
            aiagent::ToolRegistryOptions{
                .protected_paths = {session_path},
                .allow_write = true,
                .allowed_programs = {program},
                .command_approval = [](const aiagent::CommandApprovalRequest&) { return true; }});
        ScriptedModel unused_model;
        std::ostringstream unused_log;
        aiagent::Agent mismatched_agent(
            unused_model, mismatched_tools, unused_log,
            aiagent::AgentOptions{.require_verification_after_write = true,
                                  .session_store = &session,
                                  .resume_session = true});
        (void)mismatched_agent.run("");
    } catch (const std::invalid_argument& error) {
        rejected_policy_downgrade = std::string(error.what()).find("能力授权") != std::string::npos;
    }
    expect(rejected_policy_downgrade, "resume rejects a different per-command approval policy");

    aiagent::ToolRegistry resumed_tools(
        workspace, aiagent::ToolRegistryOptions{.protected_paths = {session_path},
                                                .allow_write = true,
                                                .allowed_programs = {program},
                                                .default_command_timeout_seconds = 5,
                                                .max_command_timeout_seconds = 5});
    ResumeVerificationModel resumed_model(program);
    std::ostringstream resumed_log;
    aiagent::Agent resumed_agent(resumed_model, resumed_tools, resumed_log,
                                 aiagent::AgentOptions{.max_turns = 2,
                                                       .require_verification_after_write = true,
                                                       .session_store = &session,
                                                       .resume_session = true});
    const auto resumed = resumed_agent.run("");
    expect(resumed.completed && resumed.status == "completed",
           "resumed session executes the pending patch and reaches completion");
    expect(resumed.answer == "恢复后验证完成" && resumed.turns == 3,
           "resume preserves prior turns and continues the original conversation");
    expect(resumed.execution.file_changes == 1 && resumed.execution.commands_passed == 1,
           "resume preserves and extends the execution summary");
    expect(resumed.verification_status == "passed",
           "resumed task still satisfies the verification gate");
    expect(read_text(workspace / "README.md") == "# Fixed\n",
           "restored pending patch changes the workspace exactly once");
    expect(resumed.changes.unified_diff.find("-# Broken\n+# Fixed\n") != std::string::npos,
           "restored change journal preserves the original-to-final diff");
    expect(session.load().at("status") == "completed",
           "final stable checkpoint records terminal completion");
#endif
}

void test_json_config() {
    TemporaryDirectory temporary;
    const auto valid_path = temporary.path() / "valid.json";
    write_text(valid_path, R"({"api_url":"https://example.test/chat/completions",)"
                           R"("api_key":"secret","model":"example-model",)"
                           R"("connect_timeout_seconds":7,"request_timeout_seconds":42,)"
                           R"("max_retries":4,"retry_initial_delay_ms":123,)"
                           R"("max_completion_tokens":777})");

    const auto config = aiagent::load_chat_completions_config(valid_path);
    expect(config.api_url == "https://example.test/chat/completions", "JSON config loads api_url");
    expect(config.api_key == "secret", "JSON config loads api_key");
    expect(config.model == "example-model", "JSON config loads model");
    expect(config.connect_timeout_seconds == 7, "JSON config loads connect timeout");
    expect(config.request_timeout_seconds == 42, "JSON config loads request timeout");
    expect(config.max_retries == 4, "JSON config loads retry count");
    expect(config.retry_initial_delay_ms == 123, "JSON config loads retry delay");
    expect(config.max_completion_tokens == 777, "JSON config loads the completion token ceiling");
    expect(config.adapter == aiagent::ModelAdapter::chat_completions && !config.stream,
           "legacy JSON config keeps the Chat Completions non-streaming defaults");

    const auto invalid_path = temporary.path() / "invalid.json";
    write_text(invalid_path, R"({"api_url":"https://example.test","api_key":"x"})");
    bool rejected_missing_model = false;
    try {
        (void)aiagent::load_chat_completions_config(invalid_path);
    } catch (const std::runtime_error& error) {
        rejected_missing_model = std::string(error.what()).find("model") != std::string::npos;
    }
    expect(rejected_missing_model, "JSON config explains a missing model field");

    bool rejected_zero_timeout = false;
    try {
        aiagent::ChatCompletionsClient invalid_client(
            {.api_url = "https://example.test/chat/completions",
             .model = "example-model",
             .connect_timeout_seconds = 0,
             .request_timeout_seconds = 1});
    } catch (const std::invalid_argument&) {
        rejected_zero_timeout = true;
    }
    expect(rejected_zero_timeout,
           "model client rejects non-positive timeout policy even without config loading");
}

void test_model_retry_backoff() {
#if defined(_WIN32)
    return;
#else
    RetryHttpServer server;
    std::vector<aiagent::ModelProgress> progress;
    aiagent::ChatCompletionsClient client(
        {.api_url = server.url(),
         .model = "retry-test-model",
         .connect_timeout_seconds = 2,
         .request_timeout_seconds = 2,
         .max_retries = 2,
         .retry_initial_delay_ms = 1,
         .max_completion_tokens = 321,
         .progress = [&](const aiagent::ModelProgress& event) { progress.push_back(event); }});
    const auto started = std::chrono::steady_clock::now();
    const auto reply =
        client.complete(aiagent::Json::array({{{"role", "system"}, {"content", "test"}},
                                              {{"role", "user"}, {"content", "test retry"}}}),
                        aiagent::Json::array());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    expect(reply.text == "retry passed",
           "model client returns the successful response after transient failures");
    expect(server.requests() == 3,
           "model client retries exactly the configured transient failures");
    expect(elapsed.count() >= 50,
           "model client honors the server-provided token reset delay for HTTP 429");
    expect(server.saw_completion_limit(),
           "model request carries the configured completion token ceiling");
    expect(reply.usage.available && reply.usage.prompt_tokens == 120 &&
               reply.usage.completion_tokens == 8 && reply.usage.cached_tokens == 96,
           "model client exposes prompt, completion and cached token usage");
    expect(reply.metadata.adapter == "chat_completions" &&
               reply.metadata.model == "retry-test-model" && reply.metadata.attempts == 3 &&
               reply.metadata.retries == 2 && reply.metadata.http_status == 200 &&
               reply.metadata.duration_ms >= 50,
           "model client exposes adapter, retry and latency metadata");
    expect(progress.size() == 6 &&
               progress.at(0).kind == aiagent::ModelProgressKind::attempt_started &&
               progress.at(1).kind == aiagent::ModelProgressKind::retry_scheduled &&
               progress.at(1).http_status == 429 && progress.at(1).delay_ms >= 50 &&
               progress.at(2).kind == aiagent::ModelProgressKind::attempt_started &&
               progress.at(3).kind == aiagent::ModelProgressKind::retry_scheduled &&
               progress.at(3).http_status == 503 &&
               progress.at(4).kind == aiagent::ModelProgressKind::attempt_started &&
               progress.at(5).kind == aiagent::ModelProgressKind::request_succeeded &&
               progress.at(5).http_status == 200 && progress.at(5).attempt == 3 &&
               progress.at(5).max_attempts == 3,
           "model progress reports each attempt, retry delay and final response");
    const auto progress_json = aiagent::model_progress_to_json(progress.at(1));
    expect(progress_json.at("kind") == "retry_scheduled" && progress_json.at("attempt") == 1 &&
               progress_json.at("http_status") == 429,
           "model progress has a stable event-log JSON contract");
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--command-helper") {
        return run_command_helper(argc, argv);
    }

    try {
        std::error_code executable_error;
        test_executable = std::filesystem::weakly_canonical(argv[0], executable_error);
        if (executable_error || test_executable.empty()) {
            throw std::runtime_error("could not resolve test executable path");
        }

        test_read_only_tools();
        test_apply_patch_tool();
        test_apply_changeset_tool();
        test_write_path_allowlist();
        test_command_runner();
        test_task_policy_and_command_recipes();
        test_command_runtime_controls();
        test_command_os_sandbox();
        test_agent_loop();
        test_agent_context_budget();
        test_event_log_and_machine_result();
        test_runtime_file_guards();
        test_write_agent_loop();
        test_patch_then_verify_agent_loop();
        test_failed_verification_requires_repair();
        test_latest_command_controls_verification_status();
        test_denied_command_cannot_satisfy_verification();
        test_agent_cancellation_stops_running_command();
        test_session_checkpoint_and_resume();
        test_json_config();
        test_model_retry_backoff();
        std::cout << "All aiagent tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
