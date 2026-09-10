#include "mint/platform/system.hpp"

#include <cstdlib>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mint::platform {

std::optional<std::tm> utc_time(std::time_t value) noexcept {
    std::tm result{};
#if defined(_WIN32)
    if (::gmtime_s(&result, &value) != 0) {
        return std::nullopt;
    }
#else
    if (::gmtime_r(&value, &result) == nullptr) {
        return std::nullopt;
    }
#endif
    return result;
}

long process_id() noexcept {
#if defined(_WIN32)
    return static_cast<long>(::_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

std::string user_interface_language() {
#if defined(_WIN32)
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    const auto locale = MAKELCID(::GetUserDefaultUILanguage(), SORT_DEFAULT);
    const auto count = ::LCIDToLocaleName(locale, name, LOCALE_NAME_MAX_LENGTH, 0);
    if (count > 0) {
        return std::string(name, name + count - 1);
    }
#endif
    return {};
}

std::optional<std::filesystem::path> state_directory_base() {
#if defined(_WIN32)
    const auto* base = ::_wgetenv(L"LOCALAPPDATA");
    if (base != nullptr && base[0] != L'\0') {
        return std::filesystem::path(base);
    }
#else
#if !defined(__APPLE__)
    if (const auto* state = std::getenv("XDG_STATE_HOME"); state != nullptr && state[0] != '\0') {
        return std::filesystem::path(state);
    }
#endif
    if (const auto* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
#if defined(__APPLE__)
        return std::filesystem::path(home) / "Library" / "Application Support";
#else
        return std::filesystem::path(home) / ".local" / "state";
#endif
    }
#endif
    return std::nullopt;
}

} // namespace mint::platform
