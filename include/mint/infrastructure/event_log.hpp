#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "mint/ports/agent_event_sink.hpp"

namespace mint {

class EventLog final : public AgentEventSink {
  public:
    explicit EventLog(std::filesystem::path path, bool append = false);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    void emit(std::string type, Json data = Json::object()) override;

  private:
    std::filesystem::path path_;
    std::ofstream output_;
    std::size_t sequence_ = 0;
    std::mutex mutex_;
};

} // namespace mint
