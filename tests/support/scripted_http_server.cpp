#include "scripted_http_server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <stdexcept>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mint::test {
namespace {

constexpr std::size_t max_request_bytes = 1024 * 1024;
constexpr long connection_timeout_seconds = 5;

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;

class SocketRuntime final {
  public:
    SocketRuntime() {
        WSADATA data{};
        const auto result = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            throw std::runtime_error("could not initialize Winsock: " + std::to_string(result));
        }
    }

    ~SocketRuntime() {
        (void)::WSACleanup();
    }
};

void ensure_socket_runtime() {
    static SocketRuntime runtime;
    (void)runtime;
}

int last_socket_error() noexcept {
    return ::WSAGetLastError();
}

bool is_client_disconnect(int error) noexcept {
    return error == WSAECONNABORTED || error == WSAECONNRESET || error == WSAENOTCONN ||
           error == WSAESHUTDOWN;
}

void close_socket(Socket socket) noexcept {
    (void)::closesocket(socket);
}

void shutdown_socket(Socket socket) noexcept {
    (void)::shutdown(socket, SD_BOTH);
}
#else
using Socket = int;
constexpr Socket invalid_socket = -1;

void ensure_socket_runtime() {}

int last_socket_error() noexcept {
    return errno;
}

bool is_client_disconnect(int error) noexcept {
    return error == ECONNABORTED || error == ECONNRESET || error == ENOTCONN || error == EPIPE;
}

void close_socket(Socket socket) noexcept {
    (void)::close(socket);
}

void shutdown_socket(Socket socket) noexcept {
    (void)::shutdown(socket, SHUT_RDWR);
}
#endif

[[noreturn]] void throw_socket_error(std::string_view context) {
    throw std::runtime_error(std::string(context) + ": " + std::to_string(last_socket_error()));
}

std::uintptr_t store_socket(Socket socket) noexcept {
    return static_cast<std::uintptr_t>(socket);
}

Socket native_socket(std::uintptr_t socket) noexcept {
    return static_cast<Socket>(socket);
}

class SocketGuard final {
  public:
    explicit SocketGuard(Socket socket) noexcept : socket_(socket) {}

    ~SocketGuard() {
        if (socket_ != invalid_socket) {
            close_socket(socket_);
        }
    }

    [[nodiscard]] Socket get() const noexcept {
        return socket_;
    }

  private:
    Socket socket_;
};

bool wait_for_connection(Socket listener) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    timeval timeout{connection_timeout_seconds, 0};
#if defined(_WIN32)
    const auto result = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
    const auto result = ::select(listener + 1, &readable, nullptr, nullptr, &timeout);
#endif
    if (result < 0) {
        throw_socket_error("loopback server wait failed");
    }
    return result > 0;
}

std::size_t content_length(const std::string& request) {
    constexpr std::string_view field = "Content-Length:";
    const auto position = request.find(field);
    if (position == std::string::npos) {
        return 0;
    }
    const auto begin = request.find_first_not_of(" \t", position + field.size());
    const auto end = request.find("\r\n", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        throw std::runtime_error("loopback request has an invalid Content-Length header");
    }
    return static_cast<std::size_t>(std::stoull(request.substr(begin, end - begin)));
}

std::string receive_request(Socket connection) {
    std::string request;
    std::array<char, 4096> buffer{};
    std::size_t expected_bytes = 0;
    while (expected_bytes == 0 || request.size() < expected_bytes) {
#if defined(_WIN32)
        const auto received = ::recv(connection, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
        const auto received = ::recv(connection, buffer.data(), buffer.size(), 0);
#endif
        if (received <= 0) {
            throw_socket_error("loopback server could not read the complete request");
        }
        request.append(buffer.data(), static_cast<std::size_t>(received));
        if (request.size() > max_request_bytes) {
            throw std::runtime_error("loopback request exceeded the 1 MiB test limit");
        }
        if (expected_bytes == 0) {
            const auto header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                expected_bytes = header_end + 4 + content_length(request);
            }
        }
    }
    return request;
}

bool send_all(Socket connection, std::string_view value, bool allow_client_disconnect) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto remaining = value.size() - offset;
#if defined(_WIN32)
        const auto length = static_cast<int>(std::min<std::size_t>(remaining, INT_MAX));
#else
        const auto length = remaining;
#endif
        constexpr int send_flags =
#if defined(MSG_NOSIGNAL)
            MSG_NOSIGNAL;
#else
            0;
#endif
        const auto sent = ::send(connection, value.data() + offset, length, send_flags);
        if (sent <= 0) {
            if (allow_client_disconnect && is_client_disconnect(last_socket_error())) {
                return false;
            }
            throw_socket_error("loopback server could not write the response");
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

std::string_view reason_phrase(int status) noexcept {
    if (status == 200) {
        return "OK";
    }
    if (status == 429) {
        return "Too Many Requests";
    }
    if (status == 503) {
        return "Service Unavailable";
    }
    return "Test Response";
}

void validate_response(const ScriptedHttpResponse& response) {
    if (response.status < 100 || response.status > 599 || response.content_type.empty()) {
        throw std::invalid_argument("scripted HTTP response is invalid");
    }
    for (const auto& [name, value] : response.headers) {
        if (name.empty() || name.find_first_of("\r\n:") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos) {
            throw std::invalid_argument("scripted HTTP response header is invalid");
        }
    }
}

} // namespace

ScriptedHttpServer::ScriptedHttpServer(std::vector<ScriptedHttpResponse> responses)
    : responses_(std::move(responses)) {
    if (responses_.empty() || responses_.size() > 64) {
        throw std::invalid_argument("scripted HTTP server needs 1 to 64 responses");
    }
    for (const auto& response : responses_) {
        validate_response(response);
    }

    ensure_socket_runtime();
    const auto listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == invalid_socket) {
        throw_socket_error("could not create loopback server");
    }
    listener_ = store_socket(listener);
    try {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 ||
            ::listen(listener, static_cast<int>(responses_.size())) < 0) {
            throw_socket_error("could not bind loopback server");
        }
#if defined(_WIN32)
        int length = static_cast<int>(sizeof(address));
#else
        socklen_t length = sizeof(address);
#endif
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
            throw_socket_error("could not inspect loopback server port");
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this, listener] { serve(store_socket(listener)); });
    } catch (...) {
        close_listener();
        throw;
    }
}

ScriptedHttpServer::~ScriptedHttpServer() {
    if (thread_.joinable()) {
        shutdown_socket(native_socket(listener_));
        close_listener();
        thread_.join();
    } else {
        close_listener();
    }
}

std::string ScriptedHttpServer::url(std::string_view path) const {
    if (path.empty() || path.front() != '/') {
        throw std::invalid_argument("loopback server URL path must start with '/'");
    }
    return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
}

void ScriptedHttpServer::wait() {
    if (thread_.joinable()) {
        thread_.join();
        close_listener();
    }
    if (error_ != nullptr) {
        std::rethrow_exception(error_);
    }
}

std::size_t ScriptedHttpServer::request_count() const {
    ensure_finished();
    return requests_.size();
}

const std::string& ScriptedHttpServer::request(std::size_t index) const {
    ensure_finished();
    return requests_.at(index);
}

void ScriptedHttpServer::serve(std::uintptr_t stored_listener) noexcept {
    try {
        const auto listener = native_socket(stored_listener);
        for (const auto& response : responses_) {
            if (!wait_for_connection(listener)) {
                throw std::runtime_error("loopback server timed out waiting for a request");
            }
            SocketGuard connection(::accept(listener, nullptr, nullptr));
            if (connection.get() == invalid_socket) {
                throw_socket_error("loopback server could not accept a connection");
            }
            requests_.push_back(receive_request(connection.get()));

            std::string headers = "HTTP/1.1 " + std::to_string(response.status) + " " +
                                  std::string(reason_phrase(response.status)) + "\r\n";
            for (const auto& [name, value] : response.headers) {
                headers += name + ": " + value + "\r\n";
            }
            headers += "Content-Type: " + response.content_type +
                       "\r\nContent-Length: " + std::to_string(response.body.size()) +
                       "\r\nConnection: close\r\n\r\n";
            if (!send_all(connection.get(), headers, response.allow_client_disconnect)) {
                continue;
            }

            const auto fragment = response.fragment_bytes == 0
                                      ? std::max<std::size_t>(response.body.size(), 1)
                                      : response.fragment_bytes;
            for (std::size_t offset = 0; offset < response.body.size(); offset += fragment) {
                if (!send_all(connection.get(),
                              std::string_view(response.body).substr(offset, fragment),
                              response.allow_client_disconnect)) {
                    break;
                }
            }
            shutdown_socket(connection.get());
        }
    } catch (...) {
        error_ = std::current_exception();
    }
}

void ScriptedHttpServer::close_listener() noexcept {
    if (listener_ != no_socket) {
        close_socket(native_socket(listener_));
        listener_ = no_socket;
    }
}

void ScriptedHttpServer::ensure_finished() const {
    if (thread_.joinable()) {
        throw std::logic_error("wait for the scripted HTTP server before reading requests");
    }
    if (error_ != nullptr) {
        std::rethrow_exception(error_);
    }
}

} // namespace mint::test
