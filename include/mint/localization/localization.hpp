#pragma once

#include "mint/localization/message_ids.hpp"

#include <concepts>
#include <initializer_list>
#include <string>
#include <string_view>

namespace mint::localization {

enum class Language { english, simplified_chinese };

struct Argument {
    Placeholder name;
    std::string value;
};

[[nodiscard]] Argument arg(Placeholder name, std::string value);
[[nodiscard]] Argument arg(Placeholder name, std::string_view value);
[[nodiscard]] Argument arg(Placeholder name, const char* value);

template <std::integral T> [[nodiscard]] Argument arg(Placeholder name, T value) {
    return {name, std::to_string(value)};
}

[[nodiscard]] Language current_language() noexcept;
void set_language(Language language) noexcept;
[[nodiscard]] bool set_language(std::string_view language) noexcept;
void use_environment_language() noexcept;

[[nodiscard]] std::string_view language_code(Language language) noexcept;
[[nodiscard]] std::string message(Message message, std::initializer_list<Argument> arguments = {});

class ScopedLanguage final {
  public:
    explicit ScopedLanguage(Language language) noexcept;
    ~ScopedLanguage() noexcept;

    ScopedLanguage(const ScopedLanguage&) = delete;
    ScopedLanguage& operator=(const ScopedLanguage&) = delete;

  private:
    Language previous_;
};

} // namespace mint::localization
