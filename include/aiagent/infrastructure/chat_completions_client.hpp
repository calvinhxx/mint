#pragma once

#include "aiagent/infrastructure/model_provider_client.hpp"

namespace aiagent {

// v1.3 source compatibility. New code should use ModelProviderConfig and
// ModelProviderClient because the same transport now supports multiple API
// protocols.
using ChatCompletionsConfig = ModelProviderConfig;
using ChatCompletionsClient = ModelProviderClient;

} // namespace aiagent
