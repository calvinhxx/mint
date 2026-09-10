#include "mint/platform/path.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <strings.h>
#endif

namespace mint::platform {

std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string path_to_generic_utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

bool same_path_component(const std::filesystem::path& left, const std::filesystem::path& right) {
#if defined(_WIN32)
    return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#elif defined(__APPLE__)
    return ::strcasecmp(left.c_str(), right.c_str()) == 0;
#else
    return left == right;
#endif
}

} // namespace mint::platform
