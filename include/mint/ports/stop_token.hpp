#pragma once

#include <string>

namespace mint {

class StopToken {
  public:
    virtual ~StopToken() = default;

    [[nodiscard]] virtual std::string stop_reason() const = 0;
};

} // namespace mint
