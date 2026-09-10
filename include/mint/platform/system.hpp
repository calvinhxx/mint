#pragma once

#include <ctime>
#include <filesystem>
#include <optional>
#include <string>

namespace mint::platform {

[[nodiscard]] std::optional<std::tm> utc_time(std::time_t value) noexcept;
[[nodiscard]] long process_id() noexcept;
[[nodiscard]] std::string user_interface_language();
[[nodiscard]] std::optional<std::filesystem::path> state_directory_base();

} // namespace mint::platform
