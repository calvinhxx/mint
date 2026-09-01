#pragma once

#include <string_view>

namespace mint::cli::provider_detail {

inline constexpr std::string_view inline_api_key_deprecation_message =
    "配置字段 api_key 已弃用；请改用 api_key_env，并通过环境变量提供密钥。"
    "当前版本仍兼容内联密钥。";

} // namespace mint::cli::provider_detail
