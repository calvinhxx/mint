#include "workspace_support.hpp"

#include "path_identity.hpp"
#include "registry/tool_contract.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace mint::tools::detail {
namespace {

bool ascii_case_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](unsigned char character) {
            return character >= 'A' && character <= 'Z'
                       ? static_cast<unsigned char>(character + ('a' - 'A'))
                       : character;
        };
        if (fold(static_cast<unsigned char>(left[index])) !=
            fold(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

bool ascii_case_starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           ascii_case_equal(value.substr(0, prefix.size()), prefix);
}

const std::unordered_set<std::string>& ignored_directory_names() {
    static const std::unordered_set<std::string> ignored = {
        ".mint", ".git", ".cache", "build", "dist", "node_modules", "target", "__pycache__"};
    return ignored;
}

bool aliases_ignored_directory(const std::filesystem::path& parent,
                               const std::filesystem::path& component) {
    const auto name = component.string();
    const auto candidate = parent / component;
    for (const auto& ignored : ignored_directory_names()) {
        if (name != ignored && ascii_case_equal(name, ignored) &&
            same_path_entry_identity(candidate, parent / ignored)) {
            return true;
        }
    }
    if (!name.starts_with("cmake-build-") && ascii_case_starts_with(name, "cmake-build-")) {
        const auto actual_name = path_entry_identity(candidate).filename().string();
        return actual_name.starts_with("cmake-build-");
    }
    return false;
}

} // namespace

bool is_entry_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto root_identity = path_entry_identity(root);
    const auto candidate_identity = path_entry_identity(candidate);
    auto root_part = root_identity.begin();
    auto candidate_part = candidate_identity.begin();
    for (; root_part != root_identity.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate_identity.end() || *root_part != *candidate_part) {
            return false;
        }
    }
    return true;
}

bool is_ignored_directory(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    if (ignored_directory_names().contains(name) || name.starts_with("cmake-build-")) {
        return true;
    }
    return aliases_ignored_directory(path.parent_path(), path.filename());
}

bool contains_ignored_component(const std::filesystem::path& root,
                                const std::filesystem::path& path) {
    auto relative = path.lexically_normal().lexically_relative(root.lexically_normal());
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        relative = path_entry_identity(path).lexically_relative(path_entry_identity(root));
    }
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return false;
    }
    auto parent = root;
    for (const auto& component : relative) {
        if (component == "." || component == "..") {
            continue;
        }
        if (is_ignored_directory(parent / component)) {
            return true;
        }
        parent /= component;
    }
    return false;
}

bool contains_text(std::string_view text, std::string_view query, bool case_sensitive) {
    if (case_sensitive) {
        return text.find(query) != std::string_view::npos;
    }
    const auto same_letter = [](unsigned char left, unsigned char right) {
        const auto fold = [](unsigned char character) {
            return character >= 'A' && character <= 'Z'
                       ? static_cast<unsigned char>(character + ('a' - 'A'))
                       : character;
        };
        return fold(left) == fold(right);
    };
    return std::search(text.begin(), text.end(), query.begin(), query.end(), same_letter) !=
           text.end();
}

std::string shorten_line(std::string line) {
    if (line.size() > contract::max_search_line_bytes) {
        line.resize(contract::max_search_line_bytes);
        line += "...";
    }
    return line;
}

} // namespace mint::tools::detail
