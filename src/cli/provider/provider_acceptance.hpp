#pragma once

#include "mint/ports/model_client.hpp"

namespace mint::cli::provider_detail {

[[nodiscard]] Json run_provider_acceptance(ModelClient& client, bool expect_streaming);

} // namespace mint::cli::provider_detail
