#include "mint/platform/file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>

#include <fcntl.h>
#include <io.h>
#include <windows.h>

namespace mint::platform {
namespace {

void native_error(std::error_code& error) noexcept {
    error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
}

void stream_error(std::error_code& error) noexcept {
    error = std::error_code(errno, std::generic_category());
}

bool verify_handle(HANDLE handle, std::error_code& error) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle, &info)) {
        native_error(error);
        return false;
    }
    if (info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    return true;
}

HANDLE native_handle(std::FILE* stream) noexcept {
    return reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(stream)));
}

std::FILE* open_regular_file_with_sharing(const std::filesystem::path& path, FileOpenMode mode,
                                          DWORD sharing, std::error_code& error) noexcept {
    error.clear();
    const auto write = mode != FileOpenMode::read_existing;
    const auto disposition = mode == FileOpenMode::create_new ? CREATE_NEW
                             : write                          ? OPEN_ALWAYS
                                                              : OPEN_EXISTING;
    const auto handle = ::CreateFileW(
        path.c_str(), write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ, sharing, nullptr,
        disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        native_error(error);
        return nullptr;
    }
    if (!verify_handle(handle, error)) {
        (void)::CloseHandle(handle);
        return nullptr;
    }
    const int descriptor = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                             _O_BINARY | (write ? _O_RDWR : _O_RDONLY));
    if (descriptor < 0) {
        stream_error(error);
        (void)::CloseHandle(handle);
        return nullptr;
    }
    auto* stream = ::_fdopen(descriptor, write ? "r+b" : "rb");
    if (stream == nullptr) {
        stream_error(error);
        (void)::_close(descriptor);
    }
    return stream;
}

} // namespace

std::FILE* open_regular_file(const std::filesystem::path& path, FileOpenMode mode,
                             std::error_code& error) noexcept {
    return open_regular_file_with_sharing(
        path, mode, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, error);
}

std::FILE* open_lock_file(const std::filesystem::path& path, std::error_code& error) noexcept {
    return open_regular_file_with_sharing(path, FileOpenMode::update_or_create,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, error);
}

bool verify_regular_file(std::FILE* stream, std::error_code& error) noexcept {
    error.clear();
    if (stream == nullptr) {
        error = std::make_error_code(std::errc::bad_file_descriptor);
        return false;
    }
    return verify_handle(native_handle(stream), error);
}

bool try_lock_file(std::FILE* stream, std::error_code& error) noexcept {
    error.clear();
    OVERLAPPED overlapped{};
    if (::LockFileEx(native_handle(stream), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                     MAXDWORD, MAXDWORD, &overlapped)) {
        return true;
    }
    if (::GetLastError() != ERROR_LOCK_VIOLATION) {
        native_error(error);
    }
    return false;
}

bool write_file(std::FILE* stream, std::string_view content, std::error_code& error) noexcept {
    error.clear();
    while (!content.empty()) {
        const auto count = static_cast<DWORD>((std::min)(
            content.size(), static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!::WriteFile(native_handle(stream), content.data(), count, &written, nullptr)) {
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
    if (std::fflush(stream) != 0 || ::_commit(::_fileno(stream)) != 0) {
        stream_error(error);
        return false;
    }
    return true;
}

bool replace_file_durably(const std::filesystem::path& source,
                          const std::filesystem::path& destination, std::error_code& error) {
    error.clear();
    if (!::MoveFileExW(source.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        native_error(error);
        return false;
    }
    return true;
}

std::FILE* create_exclusive_file(const std::filesystem::path& path,
                                 std::error_code& error) noexcept {
    error.clear();
    auto* stream = ::_wfopen(path.c_str(), L"wbx");
    if (stream == nullptr) {
        stream_error(error);
    }
    return stream;
}

bool same_existing_entry(const std::filesystem::path& left,
                         const std::filesystem::path& right) noexcept {
    constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    constexpr DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    const auto open = [&](const std::filesystem::path& path) {
        return ::CreateFileW(path.c_str(), 0, sharing, nullptr, OPEN_EXISTING, flags, nullptr);
    };
    const auto left_handle = open(left);
    if (left_handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const auto right_handle = open(right);
    if (right_handle == INVALID_HANDLE_VALUE) {
        (void)::CloseHandle(left_handle);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION left_info{};
    BY_HANDLE_FILE_INFORMATION right_info{};
    const auto equal = ::GetFileInformationByHandle(left_handle, &left_info) &&
                       ::GetFileInformationByHandle(right_handle, &right_info) &&
                       left_info.dwVolumeSerialNumber == right_info.dwVolumeSerialNumber &&
                       left_info.nFileIndexHigh == right_info.nFileIndexHigh &&
                       left_info.nFileIndexLow == right_info.nFileIndexLow;
    (void)::CloseHandle(right_handle);
    (void)::CloseHandle(left_handle);
    return equal;
}

} // namespace mint::platform
