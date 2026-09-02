#pragma once

#include "mint/domain/model.hpp"

#include <initializer_list>
#include <string>
#include <string_view>

namespace mint::tools::detail {

[[nodiscard]] std::string dump_json(const Json& value);
[[nodiscard]] Json error_result(std::string message);
void require_only_fields(const Json& arguments, std::string_view context,
                         std::initializer_list<std::string_view> allowed_fields);
[[nodiscard]] std::string require_string(const Json& arguments, std::string_view name);

} // namespace mint::tools::detail
