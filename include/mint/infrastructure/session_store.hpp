#pragma once

#include <filesystem>

#include "mint/ports/session_repository.hpp"

namespace mint {

class SessionStore final : public SessionRepository {
  public:
    explicit SessionStore(std::filesystem::path path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] bool exists() const override;
    [[nodiscard]] Json load() const override;
    void save(const Json& snapshot) const override;

  private:
    std::filesystem::path path_;
};

} // namespace mint
