#pragma once

#include "mint/infrastructure/model_provider_client.hpp"

#include <cstddef>
#include <string>

namespace mint::model_detail {

[[nodiscard]] ModelReply complete_provider_request(const ModelProviderConfig& config,
                                                   std::string request_body,
                                                   std::size_t message_count,
                                                   std::size_t tool_count);

} // namespace mint::model_detail
