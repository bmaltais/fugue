#include "engine/canvas_copy.h"

#include <algorithm>

namespace fugue::canvas {

namespace {

Error problem(const QString &message, const QString &hint = {}) { return makeError(QStringLiteral("input_invalid"), message, hint); }

// The area as bounds inside the canvas; an empty result when nothing is selected there.
Result<mask::IRect> boundsOf(int width, int height, const CopyArea &area) {
    if (area.selection) {
        if (area.selection->width() != width || area.selection->height() != height || area.selection->channels() != 1)
            return problem(QStringLiteral("The selection is not the size of the canvas."));
        int left = width, top = height, right = -1, bottom = -1;
        const std::vector<uint8_t> &m = area.selection->pixels();
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                if (m[size_t(y) * size_t(width) + size_t(x)] != 0) {
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = y;
                }
        if (right < 0)
            return problem(QStringLiteral("Nothing is selected."), QStringLiteral("Select an area first, or deselect to copy the whole picture."));
        return mask::IRect{left, top, right - left + 1, bottom - top + 1};
    }
    if (area.rect.empty())
        return mask::IRect{0, 0, width, height};
    const int left = std::max(0, area.rect.x), top = std::max(0, area.rect.y);
    const int right = std::min(width, area.rect.x + area.rect.width), bottom = std::min(height, area.rect.y + area.rect.height);
    if (right <= left || bottom <= top)
        return problem(QStringLiteral("The selection is outside the canvas."));
    return mask::IRect{left, top, right - left, bottom - top};
}

} // namespace

mask::MaskPaint maskedPixels(const mask::PaintDocument &document, const mask::LayerInfo &layer, const mask::MaskPaint &pixels) {
    const mask::MaskPaint *layerMask = layer.hasMask && layer.maskEnabled ? document.maskPixels(layer.id) : nullptr;
    if (!layerMask)
        return pixels;
    std::vector<uint8_t> bytes = pixels.pixels();
    const std::vector<uint8_t> &m = layerMask->pixels();
    for (size_t p = 0; p < m.size() && p * 4 + 3 < bytes.size(); ++p)
        bytes[p * 4 + 3] = uint8_t((bytes[p * 4 + 3] * m[p] + 127) / 255);
    return mask::MaskPaint(pixels.width(), pixels.height(), bytes, 4);
}

Result<mask::MaskPaint> layerPixels(const mask::PaintDocument &document, int position) {
    if (position < 0 || position >= document.layerCount())
        return problem(QStringLiteral("There is no such layer."));
    const mask::LayerInfo info = document.layer(position);
    const mask::MaskPaint *pixels = info.kind == mask::LayerKind::Mask ? nullptr : document.pixels(info.id);
    if (!pixels)
        return problem(QStringLiteral("A mask layer is a selection, not a picture."), QStringLiteral("Pick a paint layer to copy."));
    return maskedPixels(document, info, *pixels);
}

Result<CopiedPicture> copyArea(const mask::MaskPaint &source, const CopyArea &area) {
    if (source.channels() != 4)
        return problem(QStringLiteral("Only a picture can be copied."));
    const Result<mask::IRect> bounds = boundsOf(source.width(), source.height(), area);
    if (!bounds)
        return bounds.errors();
    const mask::IRect b = bounds.value();
    std::vector<uint8_t> out(size_t(b.width) * size_t(b.height) * 4);
    const std::vector<uint8_t> &in = source.pixels();
    for (int y = 0; y < b.height; ++y)
        for (int x = 0; x < b.width; ++x) {
            const size_t from = (size_t(b.y + y) * size_t(source.width()) + size_t(b.x + x)) * 4;
            const size_t to = (size_t(y) * size_t(b.width) + size_t(x)) * 4;
            std::copy_n(in.begin() + long(from), 4, out.begin() + long(to));
            if (area.selection) {
                const uint8_t m = area.selection->pixels()[size_t(b.y + y) * size_t(source.width()) + size_t(b.x + x)];
                out[to + 3] = uint8_t((out[to + 3] * m + 127) / 255);
            }
        }
    return CopiedPicture{mask::MaskPaint(b.width, b.height, out, 4), b};
}

Result<mask::MaskPaint> areaAsMask(int width, int height, const CopyArea &area) {
    if (area.selection) {
        if (area.selection->width() != width || area.selection->height() != height || area.selection->channels() != 1)
            return problem(QStringLiteral("The selection is not the size of the canvas."));
        const Result<mask::IRect> bounds = boundsOf(width, height, area); // says "Nothing is selected."
        if (!bounds)
            return bounds.errors();
        return *area.selection;
    }
    const Result<mask::IRect> bounds = boundsOf(width, height, area);
    if (!bounds)
        return bounds.errors();
    const mask::IRect b = bounds.value();
    std::vector<uint8_t> out(size_t(width) * size_t(height), 0);
    for (int y = b.y; y < b.y + b.height; ++y)
        std::fill_n(out.begin() + long(size_t(y) * size_t(width) + size_t(b.x)), b.width, uint8_t(255));
    return mask::MaskPaint(width, height, out, 1);
}

} // namespace fugue::canvas
