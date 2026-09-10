#include "mint/runtime/path.hpp"
#include "mint/platform/path.hpp"

namespace mint {

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
            !platform::same_path_component(*root_part, *candidate_part)) {
            return false;
        }
    }
    return true;
}

} // namespace mint
