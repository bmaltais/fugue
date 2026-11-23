#include "engine/mask_ops.h"

#include <cmath>

namespace fugue::mask::ops {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The mask cut to `area`, and `area` of a mask written back from a cut.
std::vector<uint8_t> cut(const std::vector<uint8_t> &mask, int width, const IRect &area) {
    std::vector<uint8_t> out(size_t(area.width) * size_t(area.height));
    for (int y = 0; y < area.height; ++y)
        std::copy_n(&mask[size_t(area.y + y) * size_t(width) + size_t(area.x)], area.width, &out[size_t(y) * size_t(area.width)]);
    return out;
}

void paste(std::vector<uint8_t> &mask, int width, const IRect &area, const std::vector<uint8_t> &piece) {
    for (int y = 0; y < area.height; ++y)
        std::copy_n(&piece[size_t(y) * size_t(area.width)], area.width, &mask[size_t(area.y + y) * size_t(width) + size_t(area.x)]);
}

// The bounds of the mask grown by `margin` and held to the picture.
IRect reach(const std::vector<uint8_t> &mask, int width, int height, int margin) {
    const IRect box = bounds(mask, width, height);
    if (box.empty())
        return box;
    const int x0 = std::max(0, box.x - margin), y0 = std::max(0, box.y - margin);
    const int x1 = std::min(width, box.x + box.width + margin), y1 = std::min(height, box.y + box.height + margin);
    return {x0, y0, x1 - x0, y1 - y0};
}

// `amount` passes in the plane, alternating the tapered and the full footprint (octagon).
void morph(std::vector<uint8_t> &plane, int width, int height, int amount) {
    std::vector<uint8_t> next(plane.size());
    const bool grow = amount > 0;
    for (int pass = 0; pass < std::abs(amount); ++pass) {
        morphPass(plane.data(), next.data(), width, height, grow, pass % 2 == 0);
        plane.swap(next);
    }
}

// A separable Gaussian of sigma = radius / 2 over the plane, edge pixels repeated.
void blur(std::vector<uint8_t> &plane, int width, int height, int radius) {
    const double sigma = std::max(0.5, radius / 2.0);
    std::vector<float> kernel(size_t(radius) * 2 + 1);
    float total = 0;
    for (int i = -radius; i <= radius; ++i) {
        const float k = float(std::exp(-double(i) * i / (2 * sigma * sigma)));
        kernel[size_t(i + radius)] = k;
        total += k;
    }
    for (float &k : kernel)
        k /= total;
    std::vector<float> across(plane.size());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float acc = 0;
            for (int i = -radius; i <= radius; ++i)
                acc += kernel[size_t(i + radius)] * plane[size_t(y) * size_t(width) + size_t(std::clamp(x + i, 0, width - 1))];
            across[size_t(y) * size_t(width) + size_t(x)] = acc;
        }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float acc = 0;
            for (int i = -radius; i <= radius; ++i)
                acc += kernel[size_t(i + radius)] * across[size_t(std::clamp(y + i, 0, height - 1)) * size_t(width) + size_t(x)];
            plane[size_t(y) * size_t(width) + size_t(x)] = uint8_t(std::clamp(int(std::lround(acc)), 0, 255));
        }
}

} // namespace

IRect bounds(const std::vector<uint8_t> &mask, int width, int height) {
    int x0 = width, y0 = height, x1 = -1, y1 = -1;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (mask[size_t(y) * size_t(width) + size_t(x)] > 0) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    return x1 < 0 ? IRect() : IRect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

std::vector<uint8_t> expanded(const std::vector<uint8_t> &mask, int width, int height, int amount) {
    amount = std::clamp(amount, -kMaxAmount, kMaxAmount);
    if (amount == 0)
        return mask;
    // A shrink reads the 0 around the selection, so it too needs one pixel of margin; a grow needs `amount`.
    const IRect area = reach(mask, width, height, amount > 0 ? amount + 1 : 1);
    if (area.empty())
        return mask;
    std::vector<uint8_t> piece = cut(mask, width, area);
    morph(piece, area.width, area.height, amount);
    std::vector<uint8_t> out = mask;
    paste(out, width, area, piece);
    return out;
}

std::vector<uint8_t> feathered(const std::vector<uint8_t> &mask, int width, int height, int radius) {
    radius = std::clamp(radius, 0, kMaxAmount);
    if (radius == 0)
        return mask;
    const IRect area = reach(mask, width, height, radius + 1);
    if (area.empty())
        return mask;
    std::vector<uint8_t> piece = cut(mask, width, area);
    blur(piece, area.width, area.height, radius);
    std::vector<uint8_t> out = mask;
    paste(out, width, area, piece);
    return out;
}

std::vector<uint8_t> border(const std::vector<uint8_t> &mask, int width, int height, int bandWidth) {
    bandWidth = std::clamp(bandWidth, 1, kMaxAmount);
    const int outer = (bandWidth + 1) / 2, inner = bandWidth - outer;
    const std::vector<uint8_t> grown = expanded(mask, width, height, outer);
    const std::vector<uint8_t> shrunk = expanded(mask, width, height, -inner);
    std::vector<uint8_t> out(mask.size());
    for (size_t i = 0; i < out.size(); ++i) {
        const int keepOut = 255 - shrunk[i]; // the outside, and the `inner` layers the shrink took
        out[i] = uint8_t(std::min<int>(grown[i], keepOut));
    }
    return out;
}

std::vector<uint8_t> smoothed(const std::vector<uint8_t> &mask, int width, int height, int radius) {
    radius = std::clamp(radius, 0, kMaxAmount);
    if (radius == 0)
        return mask;
    const IRect area = reach(mask, width, height, radius + 1);
    if (area.empty())
        return mask;
    std::vector<uint8_t> piece = cut(mask, width, area);
    blur(piece, area.width, area.height, radius);
    for (uint8_t &v : piece)
        v = v >= 128 ? 255 : 0;
    std::vector<uint8_t> out = mask;
    paste(out, width, area, piece);
    return out;
}

std::vector<uint8_t> transformed(const std::vector<uint8_t> &mask, int width, int height, const Placement &placement) {
    const IRect box = bounds(mask, width, height);
    if (box.empty() || placement.identity() || placement.scaleX <= 0 || placement.scaleY <= 0)
        return mask;
    const double cx = box.x + box.width / 2.0, cy = box.y + box.height / 2.0;
    const double angle = placement.degrees * kPi / 180.0;
    const double cosA = std::cos(angle), sinA = std::sin(angle);
    std::vector<uint8_t> out(mask.size(), 0);
    const auto at = [&](int x, int y) -> double {
        return x < 0 || y < 0 || x >= width || y >= height ? 0.0 : mask[size_t(y) * size_t(width) + size_t(x)];
    };
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            // The destination pixel center, back through the move, the turn and the scale.
            const double px = x + 0.5 - cx - placement.dx, py = y + 0.5 - cy - placement.dy;
            const double rx = (px * cosA + py * sinA) / placement.scaleX;
            const double ry = (-px * sinA + py * cosA) / placement.scaleY;
            const double sx = rx + cx - 0.5, sy = ry + cy - 0.5;
            const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
            const double fx = sx - x0, fy = sy - y0;
            const double top = at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx;
            const double bottom = at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx;
            out[size_t(y) * size_t(width) + size_t(x)] = uint8_t(std::clamp(int(std::lround(top * (1 - fy) + bottom * fy)), 0, 255));
        }
    return out;
}

} // namespace fugue::mask::ops
