#include "engine/canvas_resize.h"

#include <algorithm>
#include <optional>

#include "engine/new_canvas.h"

namespace fugue::mask {

MaskPaint movedPixels(const MaskPaint &source, int width, int height, int dx, int dy, uint8_t outside) {
    const int channels = source.channels();
    std::vector<uint8_t> pixels(size_t(width) * size_t(height) * size_t(channels), outside);
    const int x0 = std::max(0, dx), x1 = std::min(width, dx + source.width());
    const int y0 = std::max(0, dy), y1 = std::min(height, dy + source.height());
    if (x0 < x1)
        for (int y = y0; y < y1; ++y) {
            const uint8_t *from = source.pixels().data() + (size_t(y - dy) * size_t(source.width()) + size_t(x0 - dx)) * size_t(channels);
            std::copy(from, from + size_t(x1 - x0) * size_t(channels), pixels.data() + (size_t(y) * size_t(width) + size_t(x0)) * size_t(channels));
        }
    return MaskPaint(width, height, pixels, channels);
}

bool growsCanvas(int oldWidth, int oldHeight, const IRect &to) {
    return to.x < 0 || to.y < 0 || to.x + to.width > oldWidth || to.y + to.height > oldHeight;
}

Result<PaintDocument> resizedCanvas(const PaintDocument &document, const IRect &to, edgefill::Mode fill, int blur) {
    if (const QString why = canvasSizeProblem(to.width, to.height); !why.isEmpty())
        return makeError(QStringLiteral("input_invalid"), why);
    const int oldWidth = document.width(), oldHeight = document.height();
    if (to.x >= oldWidth || to.y >= oldHeight || to.x + to.width <= 0 || to.y + to.height <= 0)
        return makeError(QStringLiteral("input_invalid"), QStringLiteral("The new area must hold part of the canvas (%1 x %2); it holds none.").arg(oldWidth).arg(oldHeight));
    const int dx = -to.x, dy = -to.y; // where an old pixel (0, 0) lands
    PaintDocument out(to.width, to.height);

    if (growsCanvas(oldWidth, oldHeight, to) && fill != edgefill::Mode::Transparent) {
        // What the old canvas shows, in the new canvas's pixels; the hole is everything the old canvas does not cover.
        const MaskPaint flat = movedPixels(document.flatten(), to.width, to.height, dx, dy, 0);
        edgefill::Image image;
        image.width = to.width;
        image.height = to.height;
        image.channels = 4;
        image.data.resize(flat.pixels().size());
        for (size_t i = 0; i < image.data.size(); ++i)
            image.data[i] = float(flat.pixels()[i]) / 255.0f;
        std::vector<uint8_t> hole(size_t(to.width) * size_t(to.height), 1);
        for (int y = std::max(0, dy); y < std::min(to.height, dy + oldHeight); ++y)
            for (int x = std::max(0, dx); x < std::min(to.width, dx + oldWidth); ++x)
                hole[size_t(y) * size_t(to.width) + size_t(x)] = 0;
        edgefill::fillHole(image, hole, fill, blur);
        std::vector<uint8_t> bytes(image.data.size(), 0);
        for (size_t p = 0; p < hole.size(); ++p)
            if (hole[p])
                for (size_t c = 0; c < 4; ++c)
                    bytes[p * 4 + c] = uint8_t(std::clamp(std::lround(image.data[p * 4 + c] * 255.0f), 0L, 255L));
        LayerInfo info;
        info.name = QString::fromLatin1(resizeFillLayerName);
        info.producer = QString::fromLatin1(resizeProducer);
        out.loadLayer(info, MaskPaint(to.width, to.height, bytes, 4));
    }

    for (int position = 0; position < document.layerCount(); ++position) {
        const LayerInfo info = document.layer(position);
        const MaskPaint *layerMask = info.hasMask ? document.maskPixels(info.id) : nullptr;
        std::optional<MaskPaint> mask;
        if (layerMask)
            mask = movedPixels(*layerMask, to.width, to.height, dx, dy, 255); // a new area does not hide the layer
        if (info.kind == LayerKind::Picture) {
            out.loadLayer(info, MaskPaint(1, 1, 4), mask ? &*mask : nullptr);
            continue;
        }
        const MaskPaint *pixels = document.pixels(info.id);
        if (!pixels)
            continue;
        out.loadLayer(info, movedPixels(*pixels, to.width, to.height, dx, dy, 0), mask ? &*mask : nullptr);
    }
    out.forgetHistory();
    return out;
}

} // namespace fugue::mask
