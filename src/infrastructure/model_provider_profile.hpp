#pragma once

#include "mint/infrastructure/model_provider_client.hpp"

#include <optional>
#include <string_view>

namespace mint::model_detail {

[[nodiscard]] std::optional<ModelAdapter> parse_model_adapter(std::string_view value) noexcept;
[[nodiscard]] std::optional<ModelProvider> parse_model_provider(std::string_view value) noexcept;
[[nodiscard]] std::optional<ModelTokenLimitParameter>
parse_model_token_limit_parameter(std::string_view value) noexcept;

void validate_model_provider_credentials(const ModelProviderConfig& config);
void resolve_model_provider_credentials(ModelProviderConfig& config);

} // namespace mint::model_detail
