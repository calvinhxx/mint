#include "ui/components/settings_panel.hpp"
#include "ui/components/task_composer.hpp"
#include "ui/components/underline_free_input.hpp"
#include "ui/theme/layout_metrics.hpp"
#include "ui/theme/mint_theme.hpp"

#include <FluentQt/BasicInput.h>
#include <FluentQt/TextFields.h>

#include <QApplication>
#include <QImage>
#include <QTest>

#include <gtest/gtest.h>

namespace {

TEST(DesktopInputs, EditableFieldsAvoidFluentUnderlines) {
    mint::ui::TaskComposer composer;
    composer.resize(640, 96);
    composer.show();
    QApplication::processEvents();

    auto* input =
        composer.findChild<fluent::textfields::TextEdit*>(QStringLiteral("MintTaskInput"));
    auto* action =
        composer.findChild<fluent::basicinput::Button*>(QStringLiteral("MintTaskAction"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(action, nullptr);
    EXPECT_TRUE(composer.testAttribute(Qt::WA_StyledBackground));
    EXPECT_EQ(input->focusedBorderWidth(), 0);
    EXPECT_EQ(input->unfocusedBorderWidth(), 0);
    EXPECT_EQ(input->minVisibleLines(), 2);
    EXPECT_EQ(input->maxVisibleLines(), 6);
    EXPECT_TRUE(input->tabChangesFocus());
    EXPECT_FALSE(input->accessibleName().isEmpty());
    ASSERT_NE(input->focusProxy(), nullptr);
    EXPECT_EQ(input->focusProxy()->accessibleName(), input->accessibleName());
    EXPECT_GE(input->height(), input->lineHeight() * 2);
    EXPECT_EQ(action->focusPolicy(), Qt::StrongFocus);
    EXPECT_EQ(action->size(), QSize(mint::ui::metrics::composer_action_size,
                                    mint::ui::metrics::composer_action_size));
    EXPECT_EQ(action->cornerRadii(), QMargins(mint::ui::metrics::composer_action_size / 2,
                                              mint::ui::metrics::composer_action_size / 2,
                                              mint::ui::metrics::composer_action_size / 2,
                                              mint::ui::metrics::composer_action_size / 2));
    EXPECT_EQ(action->geometry().right(),
              composer.contentsRect().right() - mint::ui::metrics::space_sm);
    EXPECT_EQ(action->geometry().bottom(),
              composer.contentsRect().bottom() - mint::ui::metrics::space_sm);

    mint::ui::SettingsPanel settings;
    settings.set_model_state(
        {.provider =
             mint::desktop::ProviderDescriptor{
                 .provider = "deepseek",
                 .model = "deepseek-chat",
                 .adapter = "chat_completions",
                 .credential_scope = "v1:deepseek:https://api.deepseek.com/chat/completions"},
         .saved_credential = mint::desktop::SavedCredentialState::missing});
    auto* api_key =
        settings.findChild<fluent::textfields::PasswordBox*>(QStringLiteral("MintApiKeyEditor"));
    ASSERT_NE(api_key, nullptr);
    EXPECT_EQ(api_key->focusedBorderWidth(), 0);
    EXPECT_EQ(api_key->unfocusedBorderWidth(), 0);
}

TEST(DesktopInputs, SingleFrameHasMatchingTopAndBottomEdgesInBothThemes) {
    for (const auto mode : {mint::desktop::ThemeMode::light, mint::desktop::ThemeMode::dark}) {
        mint::ui::theme::install(mode);
        mint::ui::UnderlineFreeLineEdit line;
        mint::ui::UnderlineFreePasswordBox password;
        password.setPasswordRevealMode(fluent::textfields::PasswordBox::PasswordRevealMode::Hidden);
        line.setReadOnly(true);
        password.setReadOnly(true);
        EXPECT_FALSE(line.isFrameVisible());
        for (auto* field : {static_cast<QWidget*>(&line), static_cast<QWidget*>(&password)}) {
            field->resize(280, 40);
            field->show();
            QApplication::processEvents();
            const auto image = field->grab().toImage();
            const auto center = image.width() / 2;
            const auto edge_depth = qRound(4 * field->devicePixelRatioF());
            for (int offset = 0; offset < edge_depth; ++offset) {
                const auto top = image.pixelColor(center, offset);
                const auto bottom = image.pixelColor(center, image.height() - 1 - offset);
                EXPECT_NEAR(top.red(), bottom.red(), 2);
                EXPECT_NEAR(top.green(), bottom.green(), 2);
                EXPECT_NEAR(top.blue(), bottom.blue(), 2);
                EXPECT_NEAR(top.alpha(), bottom.alpha(), 2);
            }
        }
    }
    mint::ui::theme::install(mint::desktop::ThemeMode::dark);
}

TEST(DesktopInputs, UniformPasswordFramePreservesNativeEditingAndMasking) {
    mint::ui::UnderlineFreePasswordBox password;
    password.setPasswordRevealMode(fluent::textfields::PasswordBox::PasswordRevealMode::Hidden);
    password.resize(280, 40);
    password.show();
    password.setFocus();
    QApplication::processEvents();
    QTest::keyClicks(&password, "offline-fixture-key");
    EXPECT_EQ(password.text(), QStringLiteral("offline-fixture-key"));
    EXPECT_EQ(password.echoMode(), QLineEdit::Password);
    EXPECT_NE(password.displayText(), password.text());
    QTest::keyClick(&password, Qt::Key_Backspace);
    EXPECT_EQ(password.text(), QStringLiteral("offline-fixture-ke"));
}

} // namespace
