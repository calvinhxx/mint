#pragma once

#include <filesystem>

#include "mint/domain/model.hpp"

namespace mint {

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

} // namespace mint
