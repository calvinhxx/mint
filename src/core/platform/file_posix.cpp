#include "mint/platform/file.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mint::platform {
namespace {

void native_error(std::error_code& error) noexcept {
    error = std::error_code(errno, std::generic_category());
}

bool verify_descriptor(int descriptor, std::error_code& error) noexcept {
    struct stat info = {};
    if (::fstat(descriptor, &info) != 0) {
        native_error(error);
        return false;
    }
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1) {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    return true;
}

} // namespace

std::FILE* open_regular_file(const std::filesystem::path& path, FileOpenMode mode,
                             std::error_code& error) noexcept {
    error.clear();
    const auto write = mode != FileOpenMode::read_existing;
    // EN: Avoid blocking on a substituted FIFO before checking the opened file type.
    // ZH-CN: 避免路径被换成 FIFO 后，在检查已打开文件类型前阻塞。
    int flags = (write ? O_RDWR | O_CREAT : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    if (mode == FileOpenMode::create_new) {
        flags |= O_EXCL;
    }
    const int descriptor = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        native_error(error);
        return nullptr;
    }
    if (!verify_descriptor(descriptor, error)) {
        (void)::close(descriptor);
        return nullptr;
    }
    auto* stream = ::fdopen(descriptor, write ? "r+b" : "rb");
    if (stream == nullptr) {
        native_error(error);
        (void)::close(descriptor);
    }
    return stream;
}

std::FILE* open_lock_file(const std::filesystem::path& path, std::error_code& error) noexcept {
    return open_regular_file(path, FileOpenMode::update_or_create, error);
}

bool verify_regular_file(std::FILE* stream, std::error_code& error) noexcept {
    error.clear();
    if (stream == nullptr) {
        error = std::make_error_code(std::errc::bad_file_descriptor);
        return false;
    }
    return verify_descriptor(::fileno(stream), error);
}

bool try_lock_file(std::FILE* stream, std::error_code& error) noexcept {
    error.clear();
    if (::flock(::fileno(stream), LOCK_EX | LOCK_NB) == 0) {
        return true;
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
        native_error(error);
    }
    return false;
}

bool write_file(std::FILE* stream, std::string_view content, std::error_code& error) noexcept {
    error.clear();
    while (!content.empty()) {
        const auto count =
            std::min(content.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto written = ::write(::fileno(stream), content.data(), count);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0) {
            native_error(error);
            return false;
        }
        if (written == 0) {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }
        content.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

bool sync_file(std::FILE* stream, std::error_code& error) noexcept {
    error.clear();
    if (std::fflush(stream) != 0 || ::fsync(::fileno(stream)) != 0) {
        native_error(error);
        return false;
    }
    return true;
}

bool replace_file_durably(const std::filesystem::path& source,
                          const std::filesystem::path& destination, std::error_code& error) {
    error.clear();
    const auto parent =
        destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
    if (::rename(source.c_str(), destination.c_str()) != 0) {
        native_error(error);
        return false;
    }
    const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        native_error(error);
        return false;
    }
    const auto synced = ::fsync(directory);
    if (synced != 0) {
        native_error(error);
    }
    (void)::close(directory);
    return synced == 0;
}

std::FILE* create_exclusive_file(const std::filesystem::path& path,
                                 std::error_code& error) noexcept {
    error.clear();
    auto* stream = std::fopen(path.c_str(), "wbx");
    if (stream == nullptr) {
        native_error(error);
    }
    return stream;
}

bool same_existing_entry(const std::filesystem::path& left,
                         const std::filesystem::path& right) noexcept {
    struct stat left_info = {};
    struct stat right_info = {};
    return ::lstat(left.c_str(), &left_info) == 0 && ::lstat(right.c_str(), &right_info) == 0 &&
           left_info.st_dev == right_info.st_dev && left_info.st_ino == right_info.st_ino;
}

} // namespace mint::platform
