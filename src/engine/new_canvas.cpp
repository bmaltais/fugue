#include "engine/new_canvas.h"


#include "engine/layout_color.h"
#include "engine/paint_file.h"

namespace fugue::mask {

Result<CanvasBackground> parseBackground(const QString &text) {
    const QString word = text.trimmed().toLower();
    if (word == QLatin1String("transparent") || word.isEmpty())
        return CanvasBackground{};
    if (word == QLatin1String("white"))
        return CanvasBackground{true, Rgba{255, 255, 255, 255}};
    if (word == QLatin1String("black"))
        return CanvasBackground{true, Rgba{0, 0, 0, 255}};
    const QString hex = normalizeHex(word.startsWith(QLatin1Char('#')) ? word : QLatin1Char('#') + word);
    if (hex.isEmpty())
        return makeError(QStringLiteral("input_invalid"), QStringLiteral("A background is transparent, white, black or a color such as #336699."), text);
    const Rgb rgb = rgbFromHex(hex);
    return CanvasBackground{true, Rgba{uint8_t(rgb.r), uint8_t(rgb.g), uint8_t(rgb.b), 255}};
}

QString canvasSizeCap() {
    return QStringLiteral("at most %1 px a side and %2 megapixels").arg(paintFileMaxSide).arg(paintFileMaxPixels / 1000000);
}

QString canvasSizeProblem(int width, int height) {
    if (width < 1 || height < 1 || width > paintFileMaxSide || height > paintFileMaxSide || qint64(width) * height > paintFileMaxPixels)
        return QStringLiteral("A canvas is %1.").arg(canvasSizeCap());
    return {};
}

Result<PaintDocument> blankCanvas(int width, int height, const CanvasBackground &background) {
    if (const QString why = canvasSizeProblem(width, height); !why.isEmpty())
        return makeError(QStringLiteral("input_invalid"), why);
    PaintDocument document(width, height);
    if (!background.filled) {
        document.addLayer(LayerKind::Paint, QStringLiteral("Layer 1"));
        return document;
    }
    std::vector<uint8_t> pixels(size_t(width) * size_t(height) * 4);
    for (size_t at = 0; at < pixels.size(); at += 4) {
        pixels[at] = background.color.r;
        pixels[at + 1] = background.color.g;
        pixels[at + 2] = background.color.b;
        pixels[at + 3] = background.color.a;
    }
    document.loadLayer(LayerKind::Paint, QStringLiteral("Background"), true, 1.0, MaskPaint(width, height, pixels, 4));
    document.forgetHistory();
    return document;
}

Result<PaintDocument> canvasFromPixels(int width, int height, const std::vector<uint8_t> &rgba, const QString &layerName) {
    if (const QString why = canvasSizeProblem(width, height); !why.isEmpty())
        return makeError(QStringLiteral("input_invalid"), why);
    PaintDocument document(width, height);
    document.loadLayer(LayerKind::Paint, layerName, true, 1.0, MaskPaint(width, height, rgba, 4));
    document.forgetHistory();
    return document;
}

} // namespace fugue::mask
