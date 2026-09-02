#pragma once

#include <cstddef>
#include <memory>

#include "mint/infrastructure/model_provider_client.hpp"

namespace mint::detail {

class AnthropicStreamAccumulator final {
  public:
    AnthropicStreamAccumulator(ModelStreamCallback callback, ModelResponseLimits limits);
    ~AnthropicStreamAccumulator();

    AnthropicStreamAccumulator(const AnthropicStreamAccumulator&) = delete;
    AnthropicStreamAccumulator& operator=(const AnthropicStreamAccumulator&) = delete;

    void dispatch(const Json& event);
    [[nodiscard]] Json finish();
    [[nodiscard]] std::size_t streamed_bytes() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace mint::detail
