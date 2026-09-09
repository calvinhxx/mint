#pragma once

#include "model_settings_files.hpp"

#include <string_view>

namespace mint::model_settings_detail {

inline constexpr std::size_t max_key_bytes = 32U * 1024U;

[[noreturn]] void invalid_config();
[[noreturn]] void invalid_input();
[[nodiscard]] bool valid_text(std::string_view value, std::size_t limit);
[[nodiscard]] std::string trim(std::string value);
[[nodiscard]] std::string normalized_url(std::string base_url);
[[nodiscard]] std::string normalized_endpoint(std::string base_url, ModelAdapter adapter);
[[nodiscard]] std::string normalize_update(ModelConfigUpdate& update);
[[nodiscard]] ModelProviderConfig validated_config(const Json& document);
[[nodiscard]] ModelProviderConfig override_config(const Json& document);

// EN: Build display metadata from an already parsed profile without reading a credential store.
// ZH-CN: 从已解析的配置生成展示信息，不读取凭据存储。
[[nodiscard]] ModelConfigSnapshot redacted_snapshot(ModelProviderConfig config,
                                                    const std::filesystem::path& path,
                                                    const std::string& revision, bool is_override,
                                                    bool saved_api_key);

} // namespace mint::model_settings_detail
