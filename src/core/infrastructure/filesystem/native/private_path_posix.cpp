#include "private_path_native.hpp"

#include <cerrno>

#include <sys/stat.h>
#include <unistd.h>

namespace mint::private_path::native {

bool create_private_directory(const std::filesystem::path& path, std::error_code& error) {
    if (::mkdir(path.c_str(), S_IRWXU) == 0) {
        return true;
    }
    if (errno != EEXIST) {
        error = std::error_code(errno, std::generic_category());
    }
    return false;
}

void secure_directory(const std::filesystem::path& path, bool created,
                      ExistingDirectoryPolicy existing_policy) {
    (void)existing_policy;
    if (created && ::chmod(path.c_str(), S_IRWXU) != 0) {
        throw AccessError(AccessFailure::directory_permissions);
    }
    struct stat status = {};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != ::geteuid() || (status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        throw AccessError(AccessFailure::directory_verify);
    }
}

void secure_open_file(std::FILE* stream) {
    if (::fchmod(::fileno(stream), S_IRUSR | S_IWUSR) != 0) {
        throw AccessError(AccessFailure::file_permissions);
    }
}

void verify_open_file(std::FILE* stream) {
    struct stat info = {};
    if (::fstat(::fileno(stream), &info) != 0 || info.st_uid != ::geteuid() ||
        (info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        throw AccessError(AccessFailure::file_permissions);
    }
}

} // namespace mint::private_path::native
