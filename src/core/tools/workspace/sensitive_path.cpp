#include "sensitive_path.hpp"

#include "workspace_support.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace mint::tools::detail {
namespace {

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

template <std::size_t Size>
bool contains(const std::array<std::string_view, Size>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool is_sensitive_directory(std::string_view name) {
    static constexpr std::array<std::string_view, 6> names = {".aws",   ".azure", ".docker",
                                                              ".gnupg", ".kube",  ".ssh"};
    return contains(names, name);
}

bool is_sensitive_config_directory(std::string_view parent, std::string_view name) {
    static constexpr std::array<std::string_view, 4> names = {"gcloud", "gh", "hub", "op"};
    return parent == ".config" && contains(names, name);
}

bool is_public_environment_template(std::string_view name) {
    static constexpr std::array<std::string_view, 4> names = {".env.dist", ".env.example",
                                                              ".env.sample", ".env.template"};
    return contains(names, name);
}

bool is_sensitive_file(std::string_view name) {
    if (name == ".env" || name == ".envrc" ||
        (name.starts_with(".env.") && !is_public_environment_template(name))) {
        return true;
    }

    static constexpr std::array<std::string_view, 11> exact_names = {
        ".git-credentials", ".netrc",     ".npmrc", ".pypirc",      "credentials.json",    "id_dsa",
        "id_ecdsa",         "id_ed25519", "id_rsa", "secrets.json", "service-account.json"};
    if (contains(exact_names, name)) {
        return true;
    }

    const auto extension = std::filesystem::path(name).extension().string();
    static constexpr std::array<std::string_view, 4> private_key_extensions = {".key", ".p12",
                                                                               ".pem", ".pfx"};
    return contains(private_key_extensions, extension);
}

std::filesystem::path relative_path(const std::filesystem::path& root,
                                    const std::filesystem::path& path) {
    auto relative = path.lexically_normal().lexically_relative(root.lexically_normal());
    if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..") {
        return relative;
    }
    return {};
}

} // namespace

bool is_sensitive_path(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = relative_path(root, path);
    if (relative.empty()) {
        return false;
    }

    std::string previous;
    for (const auto& component : relative) {
        const auto name = lowercase_ascii(component.string());
        if (name == "." || name == "..") {
            continue;
        }
        if (is_sensitive_directory(name) || is_sensitive_config_directory(previous, name)) {
            return true;
        }
        previous = name;
    }
    return is_sensitive_file(lowercase_ascii(relative.filename().string()));
}

bool is_reserved_write_path(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = relative_path(root, path);
    if (relative.empty()) {
        return false;
    }
    static constexpr std::array<std::string_view, 4> names = {".agents", ".codex", ".git",
                                                              ".husky"};
    return contains(names, lowercase_ascii(relative.begin()->string()));
}

SensitivePathScan scan_sensitive_paths(const std::filesystem::path& root, std::size_t max_entries) {
    SensitivePathScan result;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    std::size_t visited = 0;

    while (iterator != end) {
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }
        if (++visited > max_entries) {
            result.truncated = true;
            break;
        }

        const auto path = iterator->path();
        const auto status = iterator->symlink_status(error);
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }

        const bool directory = std::filesystem::is_directory(status);
        if (directory && is_ignored_directory(path)) {
            iterator.disable_recursion_pending();
            iterator.increment(error);
            continue;
        }
        if (is_sensitive_path(root, path)) {
            result.paths.push_back(path);
            if (directory) {
                iterator.disable_recursion_pending();
            }
        }
        if (std::filesystem::is_symlink(status)) {
            iterator.disable_recursion_pending();
        }
        iterator.increment(error);
    }
    return result;
}

} // namespace mint::tools::detail
