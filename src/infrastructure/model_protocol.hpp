#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "mint/infrastructure/model_provider_client.hpp"

namespace mint::detail {

[[nodiscard]] Json build_provider_request(const ModelProviderConfig& config, const Json& messages,
                                          const Json& tools);
[[nodiscard]] ModelReply parse_provider_response(ModelAdapter adapter, const Json& response);

class ModelStreamDecoder final {
  public:
    ModelStreamDecoder(ModelAdapter adapter, ModelStreamCallback callback);
    ~ModelStreamDecoder();

    ModelStreamDecoder(const ModelStreamDecoder&) = delete;
    ModelStreamDecoder& operator=(const ModelStreamDecoder&) = delete;

    void feed(std::string_view chunk);
    [[nodiscard]] Json finish();
    [[nodiscard]] std::size_t event_count() const noexcept;
    [[nodiscard]] std::size_t streamed_bytes() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace mint::detail
