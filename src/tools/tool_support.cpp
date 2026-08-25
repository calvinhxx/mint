#include "tool_support.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace aiagent::tools::detail {

std::string dump_json(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Json error_result(std::string message) {
    return {{"ok", false}, {"error", std::move(message)}};
}

std::string require_string(const Json& arguments, std::string_view name) {
    const std::string key(name);
    if (!arguments.contains(key) || !arguments.at(key).is_string()) {
        throw std::invalid_argument("参数 " + key + " 必须是字符串");
    }
    return arguments.at(key).get<std::string>();
}

bool is_inside(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() || *root_part != *candidate_part) {
            return false;
        }
    }
    return true;
}

bool is_ignored_directory(const std::filesystem::path& path) {
    static const std::unordered_set<std::string> ignored = {
        ".aiagent", ".git", ".cache", "build", "dist", "node_modules", "target", "__pycache__"};
    const auto name = path.filename().string();
    return ignored.contains(name) || name.starts_with("cmake-build-");
}

bool contains_ignored_component(const std::filesystem::path& root,
                                const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(root);
    for (const auto& component : relative) {
        if (component == "." || component == "..") {
            continue;
        }
        if (is_ignored_directory(component)) {
            return true;
        }
    }
    return false;
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string shorten_line(std::string line) {
    constexpr std::size_t max_line_length = 400;
    if (line.size() > max_line_length) {
        line.resize(max_line_length);
        line += "...";
    }
    return line;
}

} // namespace aiagent::tools::detail
