#pragma once

#include "mint/infrastructure/model_provider_client.hpp"

#include <filesystem>

namespace mint {

ModelProviderConfig load_model_provider_config(const std::filesystem::path& config_path);

} // namespace mint
