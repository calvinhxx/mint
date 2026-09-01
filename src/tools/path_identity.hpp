#pragma once

#include <filesystem>

namespace mint::tools::detail {

// Returns an absolute, lexically normalized path whose existing components use the spelling
// reported by the filesystem. Parent links are resolved, but the final directory entry is not
// followed.
[[nodiscard]] std::filesystem::path path_entry_identity(const std::filesystem::path& path);

// Object identity for normal file access. Existing paths are compared by the filesystem; the
// component-aware fallback keeps non-existing suffixes case-sensitive on case-sensitive volumes.
[[nodiscard]] bool same_path_identity(const std::filesystem::path& left,
                                      const std::filesystem::path& right);

// Directory-entry identity for snapshot filters. Unlike same_path_identity(), a symbolic link is
// never considered identical to its target.
[[nodiscard]] bool same_path_entry_identity(const std::filesystem::path& left,
                                            const std::filesystem::path& right);

} // namespace mint::tools::detail
