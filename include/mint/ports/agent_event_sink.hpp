#pragma once

#include "mint/domain/model.hpp"

#include <string>

namespace mint {

class AgentEventSink {
  public:
    virtual ~AgentEventSink() = default;

    virtual void emit(std::string type, Json data = Json::object()) = 0;
};

} // namespace mint
