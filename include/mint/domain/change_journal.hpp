#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <string>

#include "mint/domain/model.hpp"

namespace mint {

class ChangeJournal {
  public:
    void record_created(std::string path, std::string contents);
    void record_modified(std::string path, std::string before, std::string after);
    void record_deleted(std::string path, std::string contents);

    [[nodiscard]] bool has_changes() const;
    [[nodiscard]] Json snapshot(std::size_t diff_byte_limit = 256 * 1024) const;
    [[nodiscard]] Json state() const;
    void restore(const Json& state);

  private:
    struct Entry {
        bool before_exists = true;
        std::string before;
        bool after_exists = true;
        std::string after;
    };

    void record_transition(std::string path, bool before_exists, std::string before,
                           bool after_exists, std::string after);

    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
};

} // namespace mint
