#pragma once

#include "mint/localization/localization.hpp"

#include <string>

namespace mint::cli::provider_detail {

using localization::Message;
using localization::Placeholder;

inline std::string inline_api_key_deprecation_message() {
    return localization::message(Message::cli_provider_inline_key_deprecated);
}

} // namespace mint::cli::provider_detail
