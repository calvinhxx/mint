#pragma once

#include "mint/infrastructure/model_provider_client.hpp"

#include <filesystem>

namespace mint {

ModelProviderConfig load_model_provider_config(const std::filesystem::path& config_path);
// EN: Shared typed validation for file readers and the model settings service.
// ZH-CN: 文件读取器和模型设置服务共用的类型化校验。
ModelProviderConfig parse_model_provider_config(const Json& document,
                                                const std::filesystem::path& source_path);

} // namespace mint
