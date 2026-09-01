#pragma once

#include <filesystem>
#include <memory>

#include "mint/domain/model.hpp"

namespace mint {

class ChangeTransactionStore {
  public:
    explicit ChangeTransactionStore(std::filesystem::path path);
    ~ChangeTransactionStore();

    ChangeTransactionStore(const ChangeTransactionStore&) = delete;
    ChangeTransactionStore& operator=(const ChangeTransactionStore&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] bool exists() const;
    [[nodiscard]] Json load() const;
    void save(const Json& transaction) const;
    void clear() const;

  private:
    struct LockState;
    [[nodiscard]] static std::unique_ptr<LockState>
    acquire_lock(const std::filesystem::path& transaction_path);

    std::filesystem::path path_;
    std::unique_ptr<LockState> lock_;
};

[[nodiscard]] std::filesystem::path
change_transaction_path_for_session(const std::filesystem::path& session_path);
[[nodiscard]] std::filesystem::path
change_transaction_lock_path(const std::filesystem::path& transaction_path);

} // namespace mint
