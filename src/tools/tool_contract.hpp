#pragma once

#include <cstddef>

namespace mint::tools::contract {

inline constexpr int default_list_depth = 2;
inline constexpr int min_list_depth = 1;
inline constexpr int max_list_depth = 4;
inline constexpr std::size_t max_search_query_bytes = 256;
inline constexpr std::size_t binary_probe_bytes = 4096;
inline constexpr std::size_t newline_alignment_min_bytes = 4096;
inline constexpr std::size_t max_search_line_bytes = 400;
inline constexpr std::size_t max_changes = 16;
inline constexpr std::size_t max_changeset_payload_bytes = 1024 * 1024;
inline constexpr std::size_t changeset_preview_bytes = 128 * 1024;
inline constexpr std::size_t max_restored_changes = 1024;

} // namespace mint::tools::contract
