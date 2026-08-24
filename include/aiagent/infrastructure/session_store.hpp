#pragma once

#include <filesystem>

#include "aiagent/domain/model.hpp"

namespace aiagent {

class SessionStore {
  public:
    explicit SessionStore(std::filesystem::path path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] bool exists() const;
    [[nodiscard]] Json load() const;
    void save(const Json& snapshot) const;

  private:
    std::filesystem::path path_;
};

} // namespace aiagent
