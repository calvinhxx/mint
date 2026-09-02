#include "mint/runtime/path.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <strings.h>
#endif

namespace mint {
namespace {

bool same_component(const std::filesystem::path& left, const std::filesystem::path& right) {
#if defined(_WIN32)
    const auto& left_native = left.native();
    const auto& right_native = right.native();
    return ::CompareStringOrdinal(left_native.c_str(), -1, right_native.c_str(), -1, TRUE) ==
           CSTR_EQUAL;
#elif defined(__APPLE__)
    return ::strcasecmp(left.c_str(), right.c_str()) == 0;
#else
    return left == right;
#endif
}

} // namespace

bool is_path_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    if (root.empty() || candidate.empty() || !root.is_absolute() || !candidate.is_absolute()) {
        return false;
    }

    const auto normalized_root = root.lexically_normal();
    const auto normalized_candidate = candidate.lexically_normal();
    auto root_part = normalized_root.begin();
    auto candidate_part = normalized_candidate.begin();
    for (; root_part != normalized_root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == normalized_candidate.end() ||
            !same_component(*root_part, *candidate_part)) {
            return false;
        }
    }
    return true;
}

} // namespace mint
