#include "mint/localization/localization.hpp"

#include <string>

#include <gtest/gtest.h>

namespace {

using mint::localization::arg;
using mint::localization::Language;
using mint::localization::language_code;
using mint::localization::Message;
using mint::localization::message;
using mint::localization::Placeholder;
using mint::localization::ScopedLanguage;
using mint::localization::set_language;

TEST(LocalizationTest, SelectsEnglishAndChineseCatalogs) {
    std::string english;
    {
        const ScopedLanguage language(Language::english);
        english = message(Message::common_yes);
        EXPECT_EQ(english, "yes");
        EXPECT_EQ(language_code(mint::localization::current_language()), "en");
    }
    {
        const ScopedLanguage language(Language::simplified_chinese);
        EXPECT_NE(message(Message::common_yes), english);
        EXPECT_EQ(language_code(mint::localization::current_language()), "zh-CN");
    }
}

TEST(LocalizationTest, AcceptsCommonLocaleSpellings) {
    const ScopedLanguage restore(mint::localization::current_language());
    EXPECT_TRUE(set_language("zh_CN.UTF-8"));
    EXPECT_EQ(mint::localization::current_language(), Language::simplified_chinese);
    EXPECT_TRUE(set_language("en-US"));
    EXPECT_EQ(mint::localization::current_language(), Language::english);
    EXPECT_FALSE(set_language("fr-FR"));
    EXPECT_EQ(mint::localization::current_language(), Language::english);
}

TEST(LocalizationTest, FormatsTypedArguments) {
    const ScopedLanguage language(Language::english);
    const auto formatted =
        message(Message::model_config_range,
                {arg(Placeholder::path, "config.json"), arg(Placeholder::field, "max_retries"),
                 arg(Placeholder::minimum, 0), arg(Placeholder::maximum, 5)});
    EXPECT_NE(formatted.find("config.json"), std::string::npos);
    EXPECT_NE(formatted.find("max_retries"), std::string::npos);
}

} // namespace
