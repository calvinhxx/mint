#pragma once

#include "mint/infrastructure/model_provider_client.hpp"

#include <filesystem>

namespace mint {

ModelProviderConfig load_model_provider_config(const std::filesystem::path& config_path);

// v1.3 source compatibility. This loader now also understands the optional
// adapter and stream fields.
ModelProviderConfig load_chat_completions_config(const std::filesystem::path& config_path);

} // namespace mint
