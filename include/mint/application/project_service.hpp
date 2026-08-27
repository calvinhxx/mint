#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "mint/domain/model.hpp"

namespace mint {

struct ProjectSuggestion {
    std::string project_kind;
    Json policy;
    std::vector<std::string> evidence;
};

[[nodiscard]] ProjectSuggestion suggest_project_policy(const std::filesystem::path& root);

} // namespace mint
