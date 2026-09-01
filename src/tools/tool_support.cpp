#include "tool_support.hpp"

#include "path_identity.hpp"
#include "tool_contract.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>
#include <utility>

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

std::string dump_json(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Json error_result(std::string message) {
    return {{"ok", false}, {"error", std::move(message)}};
}

void require_only_fields(const Json& arguments, std::string_view context,
                         std::initializer_list<std::string_view> allowed_fields) {
    if (!arguments.is_object()) {
        throw std::invalid_argument(std::string(context) + " 参数必须是 JSON 对象");
    }
    for (auto field = arguments.begin(); field != arguments.end(); ++field) {
        const auto allowed = std::find(allowed_fields.begin(), allowed_fields.end(), field.key()) !=
                             allowed_fields.end();
        if (!allowed) {
            throw std::invalid_argument(std::string(context) + " 包含未知字段: " + field.key());
        }
    }
}

std::string require_string(const Json& arguments, std::string_view name) {
    const std::string key(name);
    if (!arguments.contains(key) || !arguments.at(key).is_string()) {
        throw std::invalid_argument("参数 " + key + " 必须是字符串");
    }
    return arguments.at(key).get<std::string>();
}

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
