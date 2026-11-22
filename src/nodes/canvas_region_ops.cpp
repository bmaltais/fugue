#include "nodes/canvas_region_ops.h"

#include <algorithm>
#include <cmath>

namespace fugue::nodes {

std::optional<CropBox> rectBox(int x, int y, int width, int height, int canvasWidth, int canvasHeight) {
    const long long left = std::clamp(x, 0, canvasWidth);
    const long long top = std::clamp(y, 0, canvasHeight);
    const long long right = width > 0 ? std::min<long long>(canvasWidth, (long long)x + width) : canvasWidth;
    const long long bottom = height > 0 ? std::min<long long>(canvasHeight, (long long)y + height) : canvasHeight;
    if (right <= left || bottom <= top)
        return std::nullopt;
    return CropBox{int(left), int(top), int(right - left), int(bottom - top)};
}

Pixels boxMask(const CropBox &box, int canvasWidth, int canvasHeight) {
    Pixels mask(canvasWidth, canvasHeight, 1);
    for (int y = box.y; y < box.y + box.height; ++y)
        std::fill_n(&mask.data[size_t(y) * canvasWidth + box.x], box.width, 1.0f);
    return mask;
}

namespace {

int snapped(double value, int step, bool down) {
    const double steps = value / step;
    return std::max(1, int(down ? std::floor(steps) : std::lround(steps))) * step;
}

} // namespace

CropSize regionSize(const CropBox &box, double scale, double capMegapixels, int multiple) {
    const int step = std::max(1, multiple);
    double width = box.width * (scale > 0 ? scale : 1.0);
    double height = box.height * (scale > 0 ? scale : 1.0);
    bool capped = false;
    if (capMegapixels > 0 && width * height > capMegapixels * 1e6) {
        const double shrink = std::sqrt(capMegapixels * 1e6 / (width * height));
        width *= shrink;
        height *= shrink;
        capped = true;
    }
    if (const double longest = std::max(width, height); longest > kMaxCropSide) {
        width *= kMaxCropSide / longest;
        height *= kMaxCropSide / longest;
        capped = true;
    }
    // With step 1 nothing is lost to rounding down, and a size under one pixel is one.
    return {snapped(width, step, capped), snapped(height, step, capped)};
}

Pixels placedLayer(int canvasWidth, int canvasHeight, const CropBox &box, const Pixels &redrawn, const Pixels &mask, int blend) {
    const bool same = redrawn.width == box.width && redrawn.height == box.height;
    const Pixels fitted = same ? redrawn : resample(redrawn, box.width, box.height, ResizeMethod::Lanczos);
    const Pixels feathered = featherMask(mask, std::max(0, blend), FeatherDirection::Outward);
    Pixels layer(canvasWidth, canvasHeight, 4);
    for (int y = 0; y < box.height; ++y)
        for (int x = 0; x < box.width; ++x) {
            float alpha = std::clamp(feathered.at(box.x + x, box.y + y, 0), 0.0f, 1.0f);
            if (fitted.channels == 4)
                alpha *= fitted.at(x, y, 3);
            if (alpha < 0.5f / 255.0f)
                continue;
            for (int c = 0; c < 3; ++c)
                layer.at(box.x + x, box.y + y, c) = fitted.at(x, y, std::min(c, fitted.channels - 1));
            layer.at(box.x + x, box.y + y, 3) = alpha;
        }
    return layer;
}

PlacedLayer placedLayerWithMask(int canvasWidth, int canvasHeight, const CropBox &box, const Pixels &redrawn, const Pixels &mask, int blend) {
    const bool same = redrawn.width == box.width && redrawn.height == box.height;
    const Pixels fitted = same ? redrawn : resample(redrawn, box.width, box.height, ResizeMethod::Lanczos);
    const Pixels feathered = featherMask(mask, std::max(0, blend), FeatherDirection::Outward);
    PlacedLayer out{Pixels(canvasWidth, canvasHeight, 4), Pixels(canvasWidth, canvasHeight, 1)};
    for (int y = 0; y < box.height; ++y)
        for (int x = 0; x < box.width; ++x) {
            for (int c = 0; c < 3; ++c)
                out.layer.at(box.x + x, box.y + y, c) = fitted.at(x, y, std::min(c, fitted.channels - 1));
            out.layer.at(box.x + x, box.y + y, 3) = fitted.channels == 4 ? std::clamp(fitted.at(x, y, 3), 0.0f, 1.0f) : 1.0f;
            out.mask.at(box.x + x, box.y + y, 0) = std::clamp(feathered.at(box.x + x, box.y + y, 0), 0.0f, 1.0f);
        }
    return out;
}

} // namespace fugue::nodes
