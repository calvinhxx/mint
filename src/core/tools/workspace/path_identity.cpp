#include "path_identity.hpp"

#include "mint/platform/file.hpp"

#include <optional>
#include <string_view>
#include <system_error>

namespace mint::tools::detail {
namespace {

bool ascii_case_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    const auto fold = [](unsigned char character) {
        return character >= 'A' && character <= 'Z'
                   ? static_cast<unsigned char>(character + ('a' - 'A'))
                   : character;
    };
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold(static_cast<unsigned char>(left[index])) !=
            fold(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> absolute_normalized(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, error);
    if (error || absolute.empty()) {
        return std::nullopt;
    }
    return absolute.lexically_normal();
}

std::filesystem::path entry_location(const std::filesystem::path& path) {
    const auto normalized = absolute_normalized(path);
    if (!normalized.has_value()) {
        return path.lexically_normal();
    }
    if (*normalized == normalized->root_path()) {
        return *normalized;
    }
    std::error_code error;
    const auto parent = std::filesystem::weakly_canonical(normalized->parent_path(), error);
    return error || parent.empty() ? *normalized
                                   : (parent / normalized->filename()).lexically_normal();
}

bool has_path_prefix(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto candidate_part = candidate.begin();
    for (const auto& root_part : root) {
        if (candidate_part == candidate.end() || root_part != *candidate_part++) {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path>
actual_entry_name(const std::filesystem::path& parent, const std::filesystem::path& candidate,
                  const std::filesystem::path& requested_name) {
    std::error_code error;
    const auto candidate_status = std::filesystem::symlink_status(candidate, error);
    if (error || candidate_status.type() == std::filesystem::file_type::not_found) {
        return std::nullopt;
    }

    std::optional<std::filesystem::path> spelling_match;
    for (std::filesystem::directory_iterator entry(parent, error), end; !error && entry != end;
         entry.increment(error)) {
        if (entry->path().filename() == requested_name) {
            return requested_name;
        }
        const auto entry_name = entry->path().filename();
        if (!spelling_match.has_value() &&
            ascii_case_equal(requested_name.string(), entry_name.string()) &&
            platform::same_existing_entry(candidate, entry->path())) {
            spelling_match = entry_name;
        }
    }
    return error ? std::nullopt : spelling_match;
}

std::filesystem::path spelled_path(const std::filesystem::path& normalized) {
    std::filesystem::path result = normalized.root_path();
    for (const auto& component : normalized.relative_path()) {
        const auto candidate = result / component;
        if (const auto actual = actual_entry_name(result, candidate, component)) {
            result /= *actual;
        } else {
            result /= component;
        }
    }
    return result.lexically_normal();
}

} // namespace

std::filesystem::path path_entry_identity(const std::filesystem::path& path) {
    const auto location = entry_location(path);
    if (location == location.root_path()) {
        return location;
    }
    const auto parent = spelled_path(location.parent_path());
    const auto name = location.filename();
    const auto candidate = parent / name;
    if (const auto actual = actual_entry_name(parent, candidate, name)) {
        return (parent / *actual).lexically_normal();
    }
    return candidate.lexically_normal();
}

bool same_path_identity(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(left, right, error);
    if (!error) {
        return equivalent;
    }

    error.clear();
    const auto resolved_left = std::filesystem::weakly_canonical(left, error);
    if (error) {
        return path_entry_identity(left) == path_entry_identity(right);
    }
    error.clear();
    const auto resolved_right = std::filesystem::weakly_canonical(right, error);
    if (error) {
        return path_entry_identity(left) == path_entry_identity(right);
    }
    return same_path_entry_identity(resolved_left, resolved_right);
}

bool same_path_entry_identity(const std::filesystem::path& left,
                              const std::filesystem::path& right) {
    const auto left_location = entry_location(left);
    const auto right_location = entry_location(right);
    if (left_location == right_location) {
        return true;
    }
    if (!ascii_case_equal(left_location.generic_string(), right_location.generic_string())) {
        return false;
    }
    return path_entry_identity(left_location) == path_entry_identity(right_location);
}

bool is_entry_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto root_location = entry_location(root);
    const auto candidate_location = entry_location(candidate);
    auto candidate_part = candidate_location.begin();
    bool needs_spelling = false;
    for (const auto& root_part : root_location) {
        if (candidate_part == candidate_location.end()) {
            return false;
        }
        if (root_part != *candidate_part) {
            if (!ascii_case_equal(root_part.generic_string(), candidate_part->generic_string())) {
                return false;
            }
            needs_spelling = true;
        }
        ++candidate_part;
    }
    // EN: Only case aliases need directory enumeration; ordinary paths use component comparisons.
    // ZH-CN: 只有大小写别名需要枚举目录，普通路径直接按组件比较。
    return !needs_spelling || has_path_prefix(path_entry_identity(root_location),
                                              path_entry_identity(candidate_location));
}

} // namespace mint::tools::detail
