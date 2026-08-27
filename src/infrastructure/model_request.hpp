#pragma once

#include "mint/infrastructure/model_provider_client.hpp"

namespace mint::model_detail {

[[nodiscard]] ModelReply complete_provider_request(const ModelProviderConfig& config,
                                                   const Json& messages, const Json& tools);

} // namespace mint::model_detail
