#include "path_identity.hpp"

#include <optional>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace mint::tools::detail {
namespace {

bool ascii_case_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    const auto fold = [](unsigned char character) {
        return character >= 'A' && character <= 'Z'
                   ? static_cast<unsigned char>(character + ('a' - 'A'))
                   : character;
    };
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold(static_cast<unsigned char>(left[index])) !=
            fold(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> absolute_normalized(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, error);
    if (error || absolute.empty()) {
        return std::nullopt;
    }
    return absolute.lexically_normal();
}

bool same_existing_entry(const std::filesystem::path& left, const std::filesystem::path& right) {
#if defined(_WIN32)
    constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    constexpr DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    const auto open = [&](const std::filesystem::path& path) {
        return ::CreateFileW(path.c_str(), 0, sharing, nullptr, OPEN_EXISTING, flags, nullptr);
    };
    const HANDLE left_handle = open(left);
    if (left_handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const HANDLE right_handle = open(right);
    if (right_handle == INVALID_HANDLE_VALUE) {
        ::CloseHandle(left_handle);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION left_info{};
    BY_HANDLE_FILE_INFORMATION right_info{};
    const bool equal = ::GetFileInformationByHandle(left_handle, &left_info) != FALSE &&
                       ::GetFileInformationByHandle(right_handle, &right_info) != FALSE &&
                       left_info.dwVolumeSerialNumber == right_info.dwVolumeSerialNumber &&
                       left_info.nFileIndexHigh == right_info.nFileIndexHigh &&
                       left_info.nFileIndexLow == right_info.nFileIndexLow;
    ::CloseHandle(right_handle);
    ::CloseHandle(left_handle);
    return equal;
#elif defined(__unix__) || defined(__APPLE__)
    struct stat left_status = {};
    struct stat right_status = {};
    return ::lstat(left.c_str(), &left_status) == 0 && ::lstat(right.c_str(), &right_status) == 0 &&
           left_status.st_dev == right_status.st_dev && left_status.st_ino == right_status.st_ino;
#else
    std::error_code error;
    const bool equal = std::filesystem::equivalent(left, right, error);
    return !error && equal;
#endif
}

std::optional<std::filesystem::path>
actual_entry_name(const std::filesystem::path& parent, const std::filesystem::path& candidate,
                  const std::filesystem::path& requested_name) {
    std::error_code error;
    const auto candidate_status = std::filesystem::symlink_status(candidate, error);
    if (error || candidate_status.type() == std::filesystem::file_type::not_found) {
        return std::nullopt;
    }

    std::optional<std::filesystem::path> spelling_match;
    for (std::filesystem::directory_iterator entry(parent, error), end; !error && entry != end;
         entry.increment(error)) {
        if (entry->path().filename() == requested_name) {
            return requested_name;
        }
        if (same_existing_entry(candidate, entry->path())) {
            const auto entry_name = entry->path().filename();
            if (!spelling_match.has_value() &&
                ascii_case_equal(requested_name.string(), entry_name.string())) {
                spelling_match = entry_name;
            }
        }
    }
    return error ? std::nullopt : spelling_match;
}

std::filesystem::path spelled_path(const std::filesystem::path& normalized) {
    std::filesystem::path result = normalized.root_path();
    for (const auto& component : normalized.relative_path()) {
        const auto candidate = result / component;
        if (const auto actual = actual_entry_name(result, candidate, component)) {
            result /= *actual;
        } else {
            result /= component;
        }
    }
    return result.lexically_normal();
}

} // namespace

std::filesystem::path path_entry_identity(const std::filesystem::path& path) {
    const auto normalized = absolute_normalized(path);
    if (!normalized.has_value()) {
        return path.lexically_normal();
    }
    if (*normalized == normalized->root_path()) {
        return *normalized;
    }

    std::error_code error;
    auto parent = std::filesystem::weakly_canonical(normalized->parent_path(), error);
    if (error || parent.empty()) {
        parent = normalized->parent_path();
    }
    parent = spelled_path(parent.lexically_normal());
    const auto name = normalized->filename();
    const auto candidate = parent / name;
    if (const auto actual = actual_entry_name(parent, candidate, name)) {
        return (parent / *actual).lexically_normal();
    }
    return candidate.lexically_normal();
}

bool same_path_identity(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(left, right, error);
    if (!error) {
        return equivalent;
    }

    error.clear();
    const auto resolved_left = std::filesystem::weakly_canonical(left, error);
    if (error) {
        return path_entry_identity(left) == path_entry_identity(right);
    }
    error.clear();
    const auto resolved_right = std::filesystem::weakly_canonical(right, error);
    if (error) {
        return path_entry_identity(left) == path_entry_identity(right);
    }
    return path_entry_identity(resolved_left) == path_entry_identity(resolved_right);
}

bool same_path_entry_identity(const std::filesystem::path& left,
                              const std::filesystem::path& right) {
    return path_entry_identity(left) == path_entry_identity(right);
}

} // namespace mint::tools::detail
