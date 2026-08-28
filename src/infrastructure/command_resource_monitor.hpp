#pragma once

#include <cstddef>
#include <filesystem>

namespace mint::command_detail {

[[nodiscard]] bool workspace_disk_limit_exceeded(const std::filesystem::path& workspace,
                                                 std::size_t limit_bytes);

} // namespace mint::command_detail
