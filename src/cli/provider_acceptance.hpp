#pragma once

#include "mint/domain/model.hpp"

namespace mint::cli::provider_detail {

[[nodiscard]] Json run_provider_acceptance(ModelClient& client, bool expect_streaming);

} // namespace mint::cli::provider_detail
