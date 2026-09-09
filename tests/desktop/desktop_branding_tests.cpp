#include <QImage>
#include <QPainter>
#include <QSize>
#include <QSvgRenderer>

#include <gtest/gtest.h>

namespace {

QImage render_svg(const QString& resource, QSize size) {
    QSvgRenderer renderer(resource);
    if (!renderer.isValid()) {
        return {};
    }
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    renderer.render(&painter);
    return image;
}

int visible_pixels(const QImage& image) {
    auto visible = 0;
    for (auto y = 0; y < image.height(); ++y) {
        for (auto x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 24) {
                ++visible;
            }
        }
    }
    return visible;
}

TEST(DesktopBranding, OrbitTailAssetsRenderAtCompactAndLauncherSizes) {
    const auto mark = QStringLiteral(":/mint/branding/mint-arctic-fox-mark.svg");
    for (const auto size : {16, 24, 32}) {
        const auto image = render_svg(mark, {size, size});
        ASSERT_FALSE(image.isNull());
        EXPECT_GT(visible_pixels(image), size * size / 8);
        EXPECT_LT(visible_pixels(image), size * size * 7 / 8);
    }

    const auto tile =
        render_svg(QStringLiteral(":/mint/branding/mint-arctic-fox-tile.svg"), {64, 64});
    ASSERT_FALSE(tile.isNull());
    EXPECT_GT(visible_pixels(tile), 64 * 64 / 2);
    EXPECT_LT(visible_pixels(tile), 64 * 64);
}

} // namespace
