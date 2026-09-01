#pragma once

#include "mint/domain/model.hpp"

namespace mint {

class ModelClient {
  public:
    virtual ~ModelClient() = default;

    [[nodiscard]] virtual ModelRequestLimits request_limits(const Json&) const {
        return {};
    }
    virtual ModelReply complete(const Json& messages, const Json& tools) = 0;
};

} // namespace mint
