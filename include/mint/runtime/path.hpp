#pragma once

#include <filesystem>

namespace mint {

/**
 * Returns true when candidate is root or one of its descendants.
 *
 * Both paths must be absolute. The comparison is lexical and does not follow
 * symlinks; callers handling untrusted filesystem entries must canonicalize
 * first or use the identity-aware tools path helpers.
 */
[[nodiscard]] bool is_path_within(const std::filesystem::path& root,
                                  const std::filesystem::path& candidate);

} // namespace mint
