#pragma once

#include "mint/domain/model.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace mint::tools::transaction_detail {

struct FileState {
    bool exists = false;
    std::string content;
};

using FileStates = std::map<std::filesystem::path, FileState>;

[[nodiscard]] std::string next_id();
[[nodiscard]] Json document(const std::string& id, const std::filesystem::path& root,
                            const FileStates& originals, const FileStates& final_states);
void restore_originals(const FileStates& originals);

} // namespace mint::tools::transaction_detail
