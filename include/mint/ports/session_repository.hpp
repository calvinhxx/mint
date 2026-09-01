#pragma once

#include "mint/domain/model.hpp"

namespace mint {

class SessionRepository {
  public:
    virtual ~SessionRepository() = default;

    [[nodiscard]] virtual bool exists() const = 0;
    [[nodiscard]] virtual Json load() const = 0;
    virtual void save(const Json& snapshot) const = 0;
};

} // namespace mint
