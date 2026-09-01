#pragma once

#include "mint/domain/model.hpp"

#include <string>

namespace mint::agent_detail {

class Conversation final {
  public:
    [[nodiscard]] static Conversation start(std::string system_prompt, std::string user_request);
    [[nodiscard]] static Conversation restore(Json messages);

    [[nodiscard]] const Json& messages() const noexcept;
    void append_user(std::string content);
    void append_assistant(Json message);
    void append_tool_result(const ToolCall& call, std::string result);

  private:
    explicit Conversation(Json messages);

    Json messages_ = Json::array();
};

} // namespace mint::agent_detail
