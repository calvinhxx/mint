#include "model_settings_files.hpp"

#include "filesystem/output_path.hpp"
#include "filesystem/private_path.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mint::model_settings_detail {
namespace {

[[noreturn]] void file_error() {
    throw ModelConfigError(ModelConfigErrorCode::unavailable,
                           "Cannot access the private model settings files.");
}

[[noreturn]] void document_error() {
    throw ModelConfigError(ModelConfigErrorCode::invalid_config,
                           "The model settings file is invalid or too large.");
}

using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

std::FILE* open_file(const std::filesystem::path& path, bool write, bool exclusive) {
#if defined(_WIN32)
    const auto handle =
        ::CreateFileW(path.c_str(), write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      exclusive ? CREATE_NEW
                      : write   ? OPEN_ALWAYS
                                : OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        CloseHandle(handle);
        file_error();
    }
    const int descriptor = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                             _O_BINARY | (write ? _O_RDWR : _O_RDONLY));
    if (descriptor < 0) {
        CloseHandle(handle);
        file_error();
    }
    auto* file = ::_fdopen(descriptor, write ? "w+b" : "rb");
#else
    int flags = (write ? O_RDWR | O_CREAT : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW;
    if (exclusive) {
        flags |= O_EXCL;
    }
    const int descriptor = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        return nullptr;
    }
    auto* file = ::fdopen(descriptor, write ? "r+b" : "rb");
#endif
    if (file == nullptr) {
#if defined(_WIN32)
        ::_close(descriptor);
#else
        ::close(descriptor);
#endif
        file_error();
    }
    return file;
}

void verify_file(std::FILE* stream, bool require_private) {
#if defined(_WIN32)
    const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(stream)));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        file_error();
    }
    if (require_private) {
        // EN: The parent is private; this also removes inherited file grants.
        // ZH-CN: 父目录为私有目录；这里同时移除文件继承的权限。
        private_path::secure_open_file({}, stream);
    }
#else
    struct stat info = {};
    if (::fstat(::fileno(stream), &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
        (require_private &&
         (info.st_uid != ::geteuid() || (info.st_mode & (S_IRWXG | S_IRWXO)) != 0))) {
        file_error();
    }
#endif
}

std::filesystem::path checked_path(const std::filesystem::path& path) {
    try {
        return infrastructure_detail::validated_output_path(
            path, "model settings", infrastructure_detail::HardLinkPolicy::reject);
    } catch (...) {
        file_error();
    }
}

std::string revision_for(std::string_view text) {
    // EN: Change detection only, not a credential hash or authentication mechanism.
    // ZH-CN: 仅用于变更检测，不是凭据哈希或身份认证机制。
    std::uint64_t first = 14695981039346656037ULL;
    std::uint64_t second = 7809847782465536322ULL;
    for (const unsigned char byte : text) {
        first = (first ^ byte) * 1099511628211ULL;
        second = (second ^ byte) * 14029467366897019727ULL;
    }
    std::ostringstream result;
    result << std::hex << std::setfill('0') << std::setw(16) << first << std::setw(16) << second;
    return result.str();
}

bool lock_file(std::FILE* stream) {
#if defined(_WIN32)
    OVERLAPPED overlapped{};
    const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(stream)));
    if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD,
                     MAXDWORD, &overlapped)) {
        return true;
    }
    if (GetLastError() != ERROR_LOCK_VIOLATION) {
        file_error();
    }
#else
    if (::flock(::fileno(stream), LOCK_EX | LOCK_NB) == 0) {
        return true;
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
        file_error();
    }
#endif
    return false;
}

void flush_file(std::FILE* stream) {
    if (std::fflush(stream) != 0) {
        file_error();
    }
#if defined(_WIN32)
    if (::_commit(::_fileno(stream)) != 0) {
        file_error();
    }
#else
    if (::fsync(::fileno(stream)) != 0) {
        file_error();
    }
#endif
}

void write_private_file(std::FILE* stream, std::string_view text) {
    // EN: Validate the private regular file before writing its native descriptor.
    // ZH-CN: 向原生描述符写入前，校验目标是私有普通文件。
    verify_file(stream, true);
    while (!text.empty()) {
#if defined(_WIN32)
        const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(stream)));
        DWORD written = 0;
        if (!::WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
            written == 0) {
            file_error();
        }
#else
        const auto written = ::write(::fileno(stream), text.data(), text.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            file_error();
        }
#endif
        text.remove_prefix(static_cast<std::size_t>(written));
    }
}

} // namespace

void scrub_memory(char* value, std::size_t bytes) noexcept {
    // EN: Volatile writes keep owned secret erasure observable under optimization.
    // ZH-CN: 使用 volatile 写入，避免优化器省略持有密钥的擦除操作。
    volatile char* cursor = value;
    while (bytes-- != 0) {
        *cursor++ = '\0';
    }
}

void scrub(std::string& value) noexcept {
    scrub_memory(value.data(), value.size());
}

void scrub(Json& document) noexcept {
    if (document.is_string()) {
        scrub(document.get_ref<std::string&>());
    } else if (document.is_structured()) {
        for (auto& value : document) {
            scrub(value);
        }
    }
}

FileLock::FileLock(const std::filesystem::path& directory) {
    try {
        private_path::ensure_directory(directory, "model settings directory");
        const auto path = checked_path(directory / "model.lock");
        File stream(open_file(path, true, false), &std::fclose);
        if (!stream) {
            file_error();
        }
        verify_file(stream.get(), true);
        private_path::secure_open_file(path, stream.get());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!lock_file(stream.get())) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw ModelConfigError(ModelConfigErrorCode::unavailable,
                                       "Model settings are busy. Try again.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stream_ = stream.release();
    } catch (const ModelConfigError&) {
        throw;
    } catch (...) {
        file_error();
    }
}

FileLock::~FileLock() {
    if (stream_ != nullptr) {
        // EN: Closing the handle releases the cross-process advisory lock.
        // ZH-CN: 关闭句柄会释放跨进程建议锁。
        std::fclose(stream_);
    }
}

std::optional<Document> read_document(const std::filesystem::path& path, bool require_private) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && status.type() == std::filesystem::file_type::not_found)) {
        return std::nullopt;
    }
    if (error) {
        file_error();
    }
    const auto checked = checked_path(path);
    File stream(open_file(checked, false, false), &std::fclose);
    if (!stream) {
        file_error();
    }
    verify_file(stream.get(), require_private);
    if (std::setvbuf(stream.get(), nullptr, _IONBF, 0) != 0) {
        file_error();
    }
    std::string source;
    const SecretGuard erase(source);
    // EN: The bounded reserve avoids leaving secret fragments in reallocated buffers.
    // ZH-CN: 按文件上限预留空间，避免扩容释放的缓冲区残留密钥片段。
    source.reserve(max_document_bytes);
    std::array<char, 8192> buffer{};
    struct EraseBuffer {
        std::array<char, 8192>& value;
        ~EraseBuffer() {
            scrub_memory(value.data(), value.size());
        }
    } erase_buffer{buffer};
    for (;;) {
        const auto bytes = std::fread(buffer.data(), 1, buffer.size(), stream.get());
        if (source.size() + bytes > max_document_bytes) {
            document_error();
        }
        source.append(buffer.data(), bytes);
        scrub_memory(buffer.data(), buffer.size());
        if (bytes < buffer.size()) {
            if (std::ferror(stream.get()) != 0) {
                file_error();
            }
            break;
        }
    }
    Document document{Json::parse(source, nullptr, false), {}};
    if (!document.value.is_object()) {
        document_error();
    }
    document.revision = revision_for(source);
    return document;
}

std::string unique_reference() {
    std::random_device random;
    std::ostringstream result;
    result << "model:" << std::hex << std::setfill('0');
    for (int index = 0; index < 4; ++index) {
        result << std::setw(8) << static_cast<std::uint32_t>(random());
    }
    return result.str();
}

void write_document(const std::filesystem::path& path, const Json& document) {
    const auto destination = checked_path(path);
    const auto staging = destination.parent_path() / ".model-staging";
    try {
        // EN: Keep in-flight credential copies inside one protected private directory.
        // ZH-CN: 将提交中的凭据副本限制在一个受保护的私有目录内。
        private_path::ensure_directory(staging, "model settings staging directory");
    } catch (...) {
        file_error();
    }
    auto temporary = staging / (".model-" + unique_reference().substr(6) + ".tmp");
    struct RemoveTemporary {
        const std::filesystem::path& path;
        bool created = false;
        ~RemoveTemporary() {
            if (created) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }
    } remove{temporary};
    File stream(open_file(temporary, true, true), &std::fclose);
    if (!stream) {
        file_error();
    }
    remove.created = true;
    private_path::secure_open_file(temporary, stream.get());
    auto source = document.dump(2);
    const SecretGuard erase(source);
    if (source.size() >= max_document_bytes) {
        document_error();
    }
    write_private_file(stream.get(), source);
    write_private_file(stream.get(), "\n");
    flush_file(stream.get());
    stream.reset();
    (void)checked_path(destination);
#if defined(_WIN32)
    if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        file_error();
    }
#else
    if (::rename(temporary.c_str(), destination.c_str()) != 0) {
        file_error();
    }
    const int directory = ::open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory < 0) {
        file_error();
    }
    const int synced = ::fsync(directory);
    (void)::close(directory);
    if (synced != 0) {
        file_error();
    }
#endif
}

} // namespace mint::model_settings_detail
