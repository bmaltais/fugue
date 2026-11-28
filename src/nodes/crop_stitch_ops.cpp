#include "nodes/crop_stitch_ops.h"

#include <algorithm>
#include <cmath>

#include "nodes/image_size.h"

namespace fugue::nodes {

std::optional<CropBox> maskBounds(const Pixels &mask) {
    int left = mask.width, top = mask.height, right = -1, bottom = -1;
    for (int y = 0; y < mask.height; ++y)
        for (int x = 0; x < mask.width; ++x)
            if (mask.at(x, y, 0) > 0.0f) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = y;
            }
    if (right < 0)
        return std::nullopt;
    return CropBox{left, top, right - left + 1, bottom - top + 1};
}

CropBox paddedBox(const CropBox &bounds, int padding, int imageWidth, int imageHeight) {
    const long long pad = std::max(0, padding);
    const auto axis = [pad](int start, int length, int limit, int &outStart, int &outLength) {
        const long long grown = length + 2 * pad;
        outLength = int(std::min<long long>(grown, limit));
        outStart = int(std::clamp<long long>(start - pad, 0, limit - outLength));
    };
    CropBox box;
    axis(bounds.x, bounds.width, imageWidth, box.x, box.width);
    axis(bounds.y, bounds.height, imageHeight, box.y, box.height);
    return box;
}

CropSize cropSizeFor(const CropBox &box, double megapixels, int multiple) {
    if (!(megapixels > 0.0))
        return {box.width, box.height};
    const int step = std::clamp(multiple, 1, kMaxCropSide);
    QSize size = sizeForMegapixels(double(box.width) / box.height, megapixels, step);
    // A sliver of a box would be a picture no model takes (1 x 4000 at 1 MP is 16 x 65536): no side
    // goes past kMaxCropSide, the shape kept as far as the step allows.
    if (const int longest = std::max(size.width(), size.height()); longest > kMaxCropSide) {
        const double scale = double(kMaxCropSide) / longest;
        // Rounded to the step, but never up past the largest multiple of it that fits.
        const int units = std::max(1, kMaxCropSide / step);
        const auto fit = [&](int side) { return std::clamp(int(std::lround(side * scale / step)), 1, units) * step; };
        size = QSize(fit(size.width()), fit(size.height()));
    }
    return {size.width(), size.height()};
}

Pixels cutBox(const Pixels &source, const CropBox &box) {
    Pixels out(box.width, box.height, source.channels);
    for (int y = 0; y < box.height; ++y)
        std::copy_n(&source.data[(size_t(box.y + y) * source.width + box.x) * source.channels],
                    size_t(box.width) * source.channels, &out.data[size_t(y) * box.width * source.channels]);
    return out;
}

Cropped cropAt(const Pixels &image, const Pixels &mask, const CropBox &box, const CropSize &size) {
    Cropped out;
    out.box = box;
    out.size = size;
    const Pixels pictureBox = cutBox(image, box);
    const Pixels maskBox = cutBox(mask, box);
    const bool same = size.width == box.width && size.height == box.height;
    out.image = same ? pictureBox : resample(pictureBox, size.width, size.height, ResizeMethod::Lanczos);
    out.mask = same ? maskBox : resample(maskBox, size.width, size.height, ResizeMethod::Bilinear);
    for (float &v : out.mask.data)
        v = std::clamp(v, 0.0f, 1.0f);
    return out;
}

std::optional<Cropped> cropToMask(const Pixels &image, const Pixels &mask, int padding, double megapixels, int multiple) {
    const std::optional<CropBox> bounds = maskBounds(mask);
    if (!bounds)
        return std::nullopt;
    const CropBox box = paddedBox(*bounds, padding, image.width, image.height);
    return cropAt(image, mask, box, cropSizeFor(box, megapixels, multiple));
}

bool shapeFits(const CropSize &size, int width, int height) {
    if (size.width <= 0 || size.height <= 0 || width <= 0 || height <= 0)
        return false;
    const double want = double(size.width) / size.height;
    const double got = double(width) / height;
    return std::fabs(got - want) <= 0.02 * want;
}

Pixels stitch(const Pixels &original, const Pixels &mask, const CropBox &box, const Pixels &redrawn, int blend) {
    const bool same = redrawn.width == box.width && redrawn.height == box.height;
    const Pixels fitted = same ? redrawn : resample(redrawn, box.width, box.height, ResizeMethod::Lanczos);
    // Feathered over the whole picture, so a mask that reaches the box's edge fades the same way
    // there as anywhere; then cut at the box.
    const Pixels feathered = featherMask(mask, std::max(0, blend), FeatherDirection::Outward);
    const Pixels inBox = cutBox(feathered, box);
    return compositeMasked(original, fitted, &inBox, box.x, box.y);
}

} // namespace fugue::nodes
