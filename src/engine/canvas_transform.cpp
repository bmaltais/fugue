#include "engine/canvas_transform.h"

#include <algorithm>
#include <cmath>

#include "engine/paint_file.h"

namespace fugue::canvas {

QString placeableProblem(int width, int height) {
    if (width < 1 || height < 1)
        return QStringLiteral("The picture is empty.");
    if (width > mask::paintFileMaxSide || height > mask::paintFileMaxSide || qint64(width) * height > mask::paintFileMaxPixels)
        return QStringLiteral("The picture is %1 x %2 px: at most %3 px a side and %4 megapixels can be placed on a canvas.")
            .arg(width).arg(height).arg(mask::paintFileMaxSide).arg(mask::paintFileMaxPixels / 1000000);
    return {};
}

LayerTransform centeredPlacement(int sourceWidth, int sourceHeight, double cx, double cy) {
    LayerTransform t;
    t.width = sourceWidth;
    t.height = sourceHeight;
    // Whole pixels: the corner (center - side / 2) is whole.
    t.cx = std::floor(cx - sourceWidth / 2.0 + 0.5) + sourceWidth / 2.0;
    t.cy = std::floor(cy - sourceHeight / 2.0 + 0.5) + sourceHeight / 2.0;
    return t.normalized();
}

Result<mask::MaskPaint> renderPlaced(const mask::MaskPaint &source, const LayerTransform &transform, int canvasWidth, int canvasHeight) {
    if (source.channels() != 4)
        return makeError(QStringLiteral("input_invalid"), QStringLiteral("Only a color layer is placed."));
    if (!transform.isPlacement(source.width(), source.height()))
        return makeError(QStringLiteral("input_invalid"), QStringLiteral("This placement scales or turns the picture."));
    const int left = int(std::lround(transform.cx - transform.width / 2.0)), top = int(std::lround(transform.cy - transform.height / 2.0));
    const int x0 = std::max(0, left), x1 = std::min(canvasWidth, left + source.width());
    const int y0 = std::max(0, top), y1 = std::min(canvasHeight, top + source.height());
    std::vector<uint8_t> pixels(size_t(canvasWidth) * size_t(canvasHeight) * 4, 0);
    const std::vector<uint8_t> &from = source.pixels();
    for (int y = y0; y < y1 && x0 < x1; ++y)
        std::copy_n(from.begin() + (size_t(y - top) * size_t(source.width()) + size_t(x0 - left)) * 4, size_t(x1 - x0) * 4,
                    pixels.begin() + (size_t(y) * size_t(canvasWidth) + size_t(x0)) * 4);
    return mask::MaskPaint(canvasWidth, canvasHeight, pixels, 4);
}

} // namespace fugue::canvas
