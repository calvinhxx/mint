#pragma once

#include "model_settings_files.hpp"

#include <string_view>

namespace mint::model_settings_detail {

[[nodiscard]] std::string reference(const Json& document);
[[nodiscard]] Document credentials_document(const std::filesystem::path& path);
// EN: Views borrow the verified document; only runtime loading copies a bound key into a profile.
// ZH-CN: 视图借用已校验文档；仅运行时加载会将绑定密钥复制到配置中。
[[nodiscard]] std::string_view stored_key(const Json& credentials, const std::string& key_reference,
                                          const ModelProviderConfig& config);
[[nodiscard]] std::string_view selected_key(const Json& document, const ModelProviderConfig& config,
                                            const Json& credentials);
[[nodiscard]] std::string_view legacy_key(const Json& credentials,
                                          const ModelProviderConfig& config);
[[nodiscard]] bool prune_managed_keys(Json& document, const std::string& keep);
void remove_inline_key(Json& document);

} // namespace mint::model_settings_detail
