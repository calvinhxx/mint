#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "mint/domain/model.hpp"

namespace mint {

class EventLog {
  public:
    explicit EventLog(std::filesystem::path path, bool append = false);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    void emit(std::string type, Json data = Json::object());

  private:
    std::filesystem::path path_;
    std::ofstream output_;
    std::size_t sequence_ = 0;
    std::mutex mutex_;
};

} // namespace mint
