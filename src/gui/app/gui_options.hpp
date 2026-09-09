#pragma once

#include <QSize>
#include <QString>

#include <filesystem>
#include <optional>
#include <string>

namespace mint::desktop {

struct GuiOptions {
    std::filesystem::path root;
    std::filesystem::path mint_cli;
    std::filesystem::path state_root;
    std::filesystem::path config;
    std::filesystem::path screenshot;
    std::filesystem::path quality_report;
    std::optional<QSize> window_size;
    QString screenshot_panel;
    std::string language;
    std::string theme;
    bool demo = false;
};

[[nodiscard]] std::optional<GuiOptions> parse_gui_options(int argc, char** argv);
void resolve_gui_invocation_paths(GuiOptions& options);

} // namespace mint::desktop
