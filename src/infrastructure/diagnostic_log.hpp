#pragma once

#include "mint/domain/model.hpp"

#include <string_view>

namespace mint::diagnostics {

inline constexpr std::string_view default_level = "warn";

enum class Level { trace, debug, info, warning, error, critical, off };

void configure(std::string_view level);
[[nodiscard]] std::string_view current_level();
[[nodiscard]] bool enabled(Level level) noexcept;
void emit(Level level, std::string_view event) noexcept;
void emit(Level level, std::string_view event, const Json& fields) noexcept;
void flush() noexcept;

} // namespace mint::diagnostics
