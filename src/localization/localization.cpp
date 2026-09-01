#include "mint/localization/localization.hpp"

#include "catalogs.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mint::localization {
namespace {

using Catalog = std::unordered_map<std::string, std::string>;

template <std::size_t Size> Catalog parse_catalog(const std::array<std::string_view, Size>& parts) {
    std::size_t total_size = 0;
    for (const auto part : parts) {
        total_size += part.size();
    }
    std::string document;
    document.reserve(total_size);
    for (const auto part : parts) {
        document += part;
    }
    const auto root = nlohmann::json::parse(document);
    return root.at("messages").get<Catalog>();
}

const Catalog& catalog(Language language) {
    static const auto english = parse_catalog(detail::english_catalog);
    static const auto simplified_chinese = parse_catalog(detail::simplified_chinese_catalog);
    return language == Language::simplified_chinese ? simplified_chinese : english;
}

std::string normalized_language(std::string_view language) {
    std::string result;
    result.reserve(language.size());
    for (const unsigned char character : language) {
        if (character == '.' || character == '@') {
            break;
        }
        result.push_back(character == '_' ? '-' : static_cast<char>(std::tolower(character)));
    }
    return result;
}

std::optional<Language> parse_language(std::string_view language) {
    const auto normalized = normalized_language(language);
    if (normalized == "c" || normalized == "posix" || normalized == "en" ||
        normalized.starts_with("en-")) {
        return Language::english;
    }
    if (normalized == "zh" || normalized.starts_with("zh-") || normalized == "chinese") {
        return Language::simplified_chinese;
    }
    return std::nullopt;
}

Language environment_language() noexcept {
    constexpr std::array variables = {"MINT_LANG", "LC_ALL", "LC_MESSAGES", "LANG"};
    for (const auto* variable : variables) {
        const auto* value = std::getenv(variable);
        if (value != nullptr && value[0] != '\0') {
            if (const auto parsed = parse_language(value)) {
                return *parsed;
            }
        }
    }
#if defined(_WIN32)
    if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE) {
        return Language::simplified_chinese;
    }
#endif
    return Language::english;
}

std::atomic<Language> selected_language{environment_language()};

void replace_all(std::string& text, std::string_view placeholder, std::string_view value) {
    std::size_t offset = 0;
    while ((offset = text.find(placeholder, offset)) != std::string::npos) {
        text.replace(offset, placeholder.size(), value);
        offset += value.size();
    }
}

} // namespace

Argument arg(Placeholder name, std::string value) {
    return {name, std::move(value)};
}

Argument arg(Placeholder name, std::string_view value) {
    return {name, std::string(value)};
}

Argument arg(Placeholder name, const char* value) {
    return {name, value == nullptr ? std::string{} : std::string(value)};
}

Language current_language() noexcept {
    return selected_language.load(std::memory_order_relaxed);
}

void set_language(Language language) noexcept {
    selected_language.store(language, std::memory_order_relaxed);
}

bool set_language(std::string_view language) noexcept {
    const auto parsed = parse_language(language);
    if (!parsed.has_value()) {
        return false;
    }
    set_language(*parsed);
    return true;
}

void use_environment_language() noexcept {
    set_language(environment_language());
}

std::string_view language_code(Language language) noexcept {
    return language == Language::simplified_chinese ? "zh-CN" : "en";
}

std::string message(Message message_id, std::initializer_list<Argument> arguments) {
    const auto key = detail::message_key(message_id);
    const auto find_message = [key](Language language) -> const std::string* {
        const auto& messages = catalog(language);
        const auto found = messages.find(std::string(key));
        return found == messages.end() ? nullptr : &found->second;
    };

    const auto* pattern = find_message(current_language());
    if (pattern == nullptr) {
        pattern = find_message(Language::english);
    }
    std::string result = pattern == nullptr ? std::string(key) : *pattern;
    for (const auto& argument : arguments) {
        replace_all(result, "{" + std::string(detail::placeholder_name(argument.name)) + "}",
                    argument.value);
    }
    return result;
}

ScopedLanguage::ScopedLanguage(Language language) noexcept : previous_(current_language()) {
    set_language(language);
}

ScopedLanguage::~ScopedLanguage() noexcept {
    set_language(previous_);
}

} // namespace mint::localization
