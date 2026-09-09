#pragma once

#include "mint/infrastructure/model_provider_client.hpp"
#include "mint/localization/localization.hpp"

#include <string>
#include <string_view>

namespace mint::cli::provider_detail {

using localization::Message;
using localization::Placeholder;

inline std::string inline_api_key_deprecation_message() {
    return localization::message(Message::cli_provider_inline_key_deprecated);
}

inline void apply_api_key_environment_override(ModelProviderConfig& config,
                                               std::string_view environment) {
    if (environment.empty()) {
        return;
    }
    config.api_key.clear();
    config.api_key_env = environment;
}

} // namespace mint::cli::provider_detail
