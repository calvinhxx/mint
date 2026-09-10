#include "gui_options.hpp"

#include "support/qt_path.hpp"

#include <QStringList>

namespace mint::desktop {
namespace {

std::optional<QSize> parse_window_size(const QString& value) {
    const auto dimensions = value.split(QLatin1Char('x'));
    if (dimensions.size() != 2) {
        return std::nullopt;
    }
    bool width_ok = false;
    bool height_ok = false;
    const auto width = dimensions.front().toInt(&width_ok);
    const auto height = dimensions.back().toInt(&height_ok);
    if (!width_ok || !height_ok || width <= 0 || height <= 0) {
        return std::nullopt;
    }
    return QSize(width, height);
}

bool missing_path_argument(int index, int argc, char** argv) {
    if (index + 1 >= argc) {
        return true;
    }
    const auto next = QString::fromLocal8Bit(argv[index + 1]);
    return next.isEmpty() || next.startsWith(QStringLiteral("--"));
}

std::filesystem::path resolve_state_directory(const std::filesystem::path& path) {
    auto absolute = std::filesystem::absolute(path);
    // EN: Resolve parent aliases and '..' before normalization, but preserve the named leaf
    // so the existing private-directory policy can reject a symlink (also for '/.' or '/').
    // ZH-CN: 先按文件系统语义解析父路径别名及 '..'，保留具名叶目录，
    // 让现有私有目录策略继续拒绝符号链接，包括带 '/.' 或 '/' 的输入。
    while (absolute.has_relative_path() &&
           (!absolute.has_filename() || absolute.filename() == ".")) {
        absolute = absolute.parent_path();
    }
    if (absolute.filename() == "..") {
        return std::filesystem::weakly_canonical(absolute);
    }
    return std::filesystem::weakly_canonical(absolute.parent_path()) / absolute.filename();
}

} // namespace

std::optional<GuiOptions> parse_gui_options(int argc, char** argv) {
    GuiOptions options;
    for (int index = 1; index < argc; ++index) {
        const auto argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QStringLiteral("--demo")) {
            options.demo = true;
        } else if (argument == QStringLiteral("--root")) {
            if (missing_path_argument(index, argc, argv)) {
                return std::nullopt;
            }
            options.root = mint::gui::path_from_qstring(QString::fromLocal8Bit(argv[++index]));
        } else if (argument == QStringLiteral("--mint-cli") && index + 1 < argc) {
            options.mint_cli = mint::gui::path_from_qstring(QString::fromLocal8Bit(argv[++index]));
        } else if (argument == QStringLiteral("--state-dir")) {
            if (missing_path_argument(index, argc, argv)) {
                return std::nullopt;
            }
            options.state_root =
                mint::gui::path_from_qstring(QString::fromLocal8Bit(argv[++index]));
        } else if (argument == QStringLiteral("--config")) {
            if (missing_path_argument(index, argc, argv)) {
                return std::nullopt;
            }
            options.config = mint::gui::path_from_qstring(QString::fromLocal8Bit(argv[++index]));
        } else if (argument == QStringLiteral("--lang") && index + 1 < argc) {
            options.language = argv[++index];
        } else if (argument == QStringLiteral("--theme") && index + 1 < argc) {
            options.theme = argv[++index];
        } else if (argument == QStringLiteral("--screenshot") && index + 1 < argc) {
            options.screenshot =
                mint::gui::path_from_qstring(QString::fromLocal8Bit(argv[++index]));
        } else if (argument == QStringLiteral("--quality-report") && index + 1 < argc) {
            options.quality_report =
                mint::gui::path_from_qstring(QString::fromLocal8Bit(argv[++index]));
        } else if (argument == QStringLiteral("--window-size") && index + 1 < argc) {
            options.window_size = parse_window_size(QString::fromLocal8Bit(argv[++index]));
        } else if (argument == QStringLiteral("--screenshot-panel") && index + 1 < argc) {
            options.screenshot_panel = QString::fromLocal8Bit(argv[++index]);
        }
    }
    return options;
}

void resolve_gui_invocation_paths(GuiOptions& options) {
    // EN: A lexical cleanup before resolving links changes which workspace was requested.
    // ZH-CN: 在解析符号链接前做词法清理，会改变用户请求的工作区。
    if (!options.root.empty()) {
        options.root = std::filesystem::weakly_canonical(std::filesystem::absolute(options.root));
    }
    if (!options.state_root.empty()) {
        options.state_root = resolve_state_directory(options.state_root);
    }
}

} // namespace mint::desktop
