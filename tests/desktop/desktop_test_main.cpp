#include "ui/support/gui_resources.hpp"
#include "ui/theme/mint_theme.hpp"

#include <FluentQt/FluentQt.h>

#include <QApplication>

#include <gtest/gtest.h>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    fluent::initializeResources();
    mint::ui::initialize_gui_resources();
    mint::ui::theme::install(mint::desktop::ThemeMode::dark);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
