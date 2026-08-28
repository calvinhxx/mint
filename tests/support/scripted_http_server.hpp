#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mint::test {

struct ScriptedHttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::vector<std::pair<std::string, std::string>> headers{};
    std::string body;
    std::size_t fragment_bytes = 0;
};

class ScriptedHttpServer final {
  public:
    explicit ScriptedHttpServer(std::vector<ScriptedHttpResponse> responses);
    ~ScriptedHttpServer();

    ScriptedHttpServer(const ScriptedHttpServer&) = delete;
    ScriptedHttpServer& operator=(const ScriptedHttpServer&) = delete;

    [[nodiscard]] std::string url(std::string_view path) const;
    void wait();

    [[nodiscard]] std::size_t request_count() const;
    [[nodiscard]] const std::string& request(std::size_t index = 0) const;

  private:
    static constexpr std::uintptr_t no_socket = static_cast<std::uintptr_t>(-1);

    void serve(std::uintptr_t listener) noexcept;
    void close_listener() noexcept;
    void ensure_finished() const;

    std::uintptr_t listener_ = no_socket;
    unsigned short port_ = 0;
    std::vector<ScriptedHttpResponse> responses_;
    std::vector<std::string> requests_;
    std::thread thread_;
    std::exception_ptr error_;
};

} // namespace mint::test
