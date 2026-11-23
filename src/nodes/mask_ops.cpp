#include "nodes/mask_ops.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fugue::nodes {

namespace {

// The part of a source placed at x, y that lies inside a width x height destination.
struct Overlap {
    int left, top, right, bottom; // in destination pixels, right and bottom exclusive
    bool empty() const { return right <= left || bottom <= top; }
};

Overlap overlapOf(int x, int y, int sourceWidth, int sourceHeight, int width, int height) {
    // 64 bit, so a far offset cannot wrap.
    const long long right = std::min<long long>(width, (long long)x + sourceWidth);
    const long long bottom = std::min<long long>(height, (long long)y + sourceHeight);
    return {std::max(x, 0), std::max(y, 0), int(std::max<long long>(right, 0)), int(std::max<long long>(bottom, 0))};
}

// One box pass along a row (horizontal) or a column (vertical): every output is the sum of the
// 2n + 1 values around it plus `fraction` of the two beyond, over the box's width, the edge
// repeated. The running sums are doubles so a long box does not drift.
void boxPass(const std::vector<float> &in, std::vector<float> &out, int width, int height, bool horizontal, int n,
             double fraction) {
    const double norm = 1.0 / (2.0 * n + 1.0 + 2.0 * fraction);
    if (horizontal) {
        for (int y = 0; y < height; ++y) {
            const float *row = &in[size_t(y) * width];
            float *dest = &out[size_t(y) * width];
            const auto at = [&](long long i) { return double(row[std::clamp<long long>(i, 0, width - 1)]); };
            double sum = 0;
            for (int k = -n; k <= n; ++k)
                sum += at(k);
            for (int x = 0; x < width; ++x) {
                dest[x] = float((sum + fraction * (at(x - n - 1) + at(x + n + 1))) * norm);
                sum += at(x + n + 1) - at(x - n);
            }
        }
        return;
    }
    // Vertical: a running sum per column, moved down a row at a time, so memory is read in order.
    std::vector<double> sum(width, 0.0);
    const auto row = [&](long long y) { return &in[size_t(std::clamp<long long>(y, 0, height - 1)) * width]; };
    for (int k = -n; k <= n; ++k) {
        const float *r = row(k);
        for (int x = 0; x < width; ++x)
            sum[x] += r[x];
    }
    for (int y = 0; y < height; ++y) {
        const float *before = row(y - n - 1), *after = row(y + n + 1), *leaving = row(y - n);
        float *dest = &out[size_t(y) * width];
        for (int x = 0; x < width; ++x) {
            dest[x] = float((sum[x] + fraction * (double(before[x]) + after[x])) * norm);
            sum[x] += double(after[x]) - leaving[x];
        }
    }
}

// The mask blurred: three box passes each way. A box of 2n + 1 pixels with the two beyond it at
// weight `fraction` has the variance (S + 2 f (n + 1)^2) / (2n + 1 + 2f), S = n (n + 1) (2n + 1) / 3;
// three of them add up to sigma^2 = radius^2 / 4 when each has radius^2 / 12. So n is the longest box
// that stays under that, and `fraction` closes the rest of the way, which makes the width continuous
// in the radius (a radius of 1 still blurs).
std::vector<float> blurred(const Pixels &mask, int radius) {
    const double target = double(radius) * radius / 12.0;
    const int n = int(std::floor((-1.0 + std::sqrt(1.0 + 12.0 * target)) / 2.0 + 1e-9));
    const double s = double(n) * (n + 1) * (2 * n + 1) / 3.0;
    const double fraction = std::clamp((target * (2 * n + 1) - s) / (2.0 * ((n + 1.0) * (n + 1.0) - target)), 0.0, 1.0);
    std::vector<float> a = mask.data, b(a.size());
    for (int pass = 0; pass < 3; ++pass) {
        boxPass(a, b, mask.width, mask.height, true, n, fraction);
        std::swap(a, b);
    }
    for (int pass = 0; pass < 3; ++pass) {
        boxPass(a, b, mask.width, mask.height, false, n, fraction);
        std::swap(a, b);
    }
    return a;
}

// ---- Fill Masked: push-pull ---------------------------------------------------------

// One level of the pyramid: the average of the color times its weight, and of the weight.
struct Level {
    int width = 0, height = 0;
    std::vector<float> color; // channels per pixel, premultiplied by weight
    std::vector<float> weight;
};

Level halved(const Level &in, int channels) {
    Level out;
    out.width = (in.width + 1) / 2;
    out.height = (in.height + 1) / 2;
    out.color.assign(size_t(out.width) * out.height * channels, 0.0f);
    out.weight.assign(size_t(out.width) * out.height, 0.0f);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            int count = 0;
            double w = 0;
            double c[4] = {0, 0, 0, 0};
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = 2 * x + dx, sy = 2 * y + dy;
                    if (sx >= in.width || sy >= in.height)
                        continue;
                    const size_t i = size_t(sy) * in.width + sx;
                    ++count;
                    w += in.weight[i];
                    for (int ch = 0; ch < channels; ++ch)
                        c[ch] += in.color[i * channels + ch];
                }
            const size_t o = size_t(y) * out.width + x;
            out.weight[o] = float(w / count);
            for (int ch = 0; ch < channels; ++ch)
                out.color[o * channels + ch] = float(c[ch] / count);
        }
    return out;
}

// The filled picture: the colors of `levels` from the coarsest, each level's own where it has
// weight and the coarser one's (bilinear, at the pixel centers) where it has not.
std::vector<float> pushed(const std::vector<Level> &levels, int channels) {
    const Level &top = levels.back();
    std::vector<float> filled(size_t(top.width) * top.height * channels, 0.5f);
    for (size_t i = 0; i < top.weight.size(); ++i)
        if (top.weight[i] > 0)
            for (int ch = 0; ch < channels; ++ch)
                filled[i * channels + ch] = top.color[i * channels + ch] / top.weight[i];
    for (int l = int(levels.size()) - 2; l >= 0; --l) {
        const Level &level = levels[l];
        const Level &coarse = levels[l + 1];
        std::vector<float> next(size_t(level.width) * level.height * channels);
        for (int y = 0; y < level.height; ++y) {
            const double fy = std::clamp((y + 0.5) / 2.0 - 0.5, 0.0, double(coarse.height - 1));
            const int y0 = int(fy), y1 = std::min(y0 + 1, coarse.height - 1);
            const double ty = fy - y0;
            for (int x = 0; x < level.width; ++x) {
                const double fx = std::clamp((x + 0.5) / 2.0 - 0.5, 0.0, double(coarse.width - 1));
                const int x0 = int(fx), x1 = std::min(x0 + 1, coarse.width - 1);
                const double tx = fx - x0;
                const size_t i = size_t(y) * level.width + x;
                const double a = level.weight[i];
                for (int ch = 0; ch < channels; ++ch) {
                    const auto f = [&](int cx, int cy) { return double(filled[(size_t(cy) * coarse.width + cx) * channels + ch]); };
                    const double up = (f(x0, y0) * (1 - tx) + f(x1, y0) * tx) * (1 - ty) + (f(x0, y1) * (1 - tx) + f(x1, y1) * tx) * ty;
                    const double own = a > 0 ? level.color[i * channels + ch] / a : 0.0;
                    next[i * channels + ch] = float(a * own + (1 - a) * up);
                }
            }
        }
        filled = std::move(next);
    }
    return filled;
}

} // namespace

Pixels maskResized(const Pixels &mask, int width, int height) {
    if (mask.width == width && mask.height == height)
        return mask;
    return resample(mask, width, height, ResizeMethod::Bilinear);
}

Pixels compositeMasked(const Pixels &destination, const Pixels &source, const Pixels *mask, int x, int y) {
    Pixels out = destination;
    const Overlap o = overlapOf(x, y, source.width, source.height, destination.width, destination.height);
    if (o.empty())
        return out;
    Pixels m;
    if (mask)
        m = maskResized(*mask, source.width, source.height);
    for (int j = o.top; j < o.bottom; ++j)
        for (int i = o.left; i < o.right; ++i) {
            const int sx = i - x, sy = j - y;
            const float weight = mask ? m.at(sx, sy, 0) : 1.0f;
            for (int ch = 0; ch < destination.channels; ++ch) {
                const float s = ch < source.channels ? source.at(sx, sy, ch) : 1.0f;
                out.at(i, j, ch) = (1.0f - weight) * destination.at(i, j, ch) + weight * s;
            }
        }
    return out;
}

Pixels thresholdMask(const Pixels &mask, float value) {
    Pixels out = mask;
    for (float &v : out.data)
        v = v > value ? 1.0f : 0.0f;
    return out;
}

Pixels combineMasks(const Pixels &destination, const Pixels &source, CombineOperation operation, int x, int y) {
    Pixels out = destination;
    const Overlap o = overlapOf(x, y, source.width, source.height, destination.width, destination.height);
    if (!o.empty())
        for (int j = o.top; j < o.bottom; ++j)
            for (int i = o.left; i < o.right; ++i) {
                const float d = destination.at(i, j, 0), s = source.at(i - x, j - y, 0);
                const bool db = std::nearbyint(d) != 0.0f, sb = std::nearbyint(s) != 0.0f;
                float v = 0;
                switch (operation) {
                case CombineOperation::Multiply: v = d * s; break;
                case CombineOperation::Add: v = d + s; break;
                case CombineOperation::Subtract: v = d - s; break;
                case CombineOperation::And: v = db && sb ? 1.0f : 0.0f; break;
                case CombineOperation::Or: v = db || sb ? 1.0f : 0.0f; break;
                case CombineOperation::Xor: v = db != sb ? 1.0f : 0.0f; break;
                }
                out.at(i, j, 0) = v;
            }
    for (float &v : out.data)
        v = std::clamp(v, 0.0f, 1.0f);
    return out;
}

Pixels featherMask(const Pixels &mask, int radius, FeatherDirection direction) {
    if (radius <= 0 || mask.width == 0 || mask.height == 0)
        return mask;
    Pixels out = mask;
    out.data = blurred(mask, radius);
    if (direction == FeatherDirection::Centered)
        return out;
    for (size_t i = 0; i < out.data.size(); ++i)
        out.data[i] = direction == FeatherDirection::Outward ? std::max(mask.data[i], out.data[i])
                                                             : std::min(mask.data[i], out.data[i]);
    return out;
}

Pixels fillMasked(const Pixels &image, const Pixels &maskIn, FillMode mode) {
    const Pixels mask = maskResized(maskIn, image.width, image.height);
    const int channels = image.channels;
    const size_t pixels = size_t(image.width) * image.height;
    std::vector<float> fill;
    if (mode == FillMode::Gray) {
        fill.assign(pixels * channels, 0.5f);
        if (channels == 4)
            for (size_t i = 0; i < pixels; ++i)
                fill[i * 4 + 3] = 1.0f;
    } else {
        std::vector<Level> levels(1);
        levels[0].width = image.width;
        levels[0].height = image.height;
        levels[0].weight.resize(pixels);
        levels[0].color.resize(pixels * channels);
        for (size_t i = 0; i < pixels; ++i) {
            const float w = std::clamp(1.0f - mask.data[i], 0.0f, 1.0f);
            levels[0].weight[i] = w;
            for (int ch = 0; ch < channels; ++ch)
                levels[0].color[i * channels + ch] = image.data[i * channels + ch] * w;
        }
        while (levels.back().width > 1 || levels.back().height > 1)
            levels.push_back(halved(levels.back(), channels));
        fill = pushed(levels, channels);
    }
    Pixels out = image;
    for (size_t i = 0; i < pixels; ++i) {
        const float m = mask.data[i];
        if (m == 0.0f)
            continue;
        for (int ch = 0; ch < channels; ++ch)
            out.data[i * channels + ch] = (1.0f - m) * image.data[i * channels + ch] + m * fill[i * channels + ch];
    }
    return out;
}

Pixels overlayMask(const Pixels &image, const Pixels &maskIn, const std::array<float, 3> &color, float opacity) {
    const Pixels mask = maskResized(maskIn, image.width, image.height);
    Pixels out = image;
    const size_t pixels = size_t(image.width) * image.height;
    for (size_t i = 0; i < pixels; ++i) {
        const float a = std::clamp(mask.data[i] * opacity, 0.0f, 1.0f);
        for (int ch = 0; ch < std::min(3, image.channels); ++ch)
            out.data[i * image.channels + ch] = (1.0f - a) * image.data[i * image.channels + ch] + a * color[ch];
    }
    return out;
}

Pixels maskToAlpha(const Pixels &image, const Pixels &mask) {
    const Pixels m = maskResized(mask, image.width, image.height);
    Pixels out(image.width, image.height, 4, 1.0f);
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            for (int ch = 0; ch < 3; ++ch)
                out.at(x, y, ch) = image.at(x, y, ch);
            const float alpha = image.channels == 4 ? image.at(x, y, 3) : 1.0f;
            out.at(x, y, 3) = std::clamp(alpha * m.at(x, y, 0), 0.0f, 1.0f);
        }
    return out;
}

} // namespace fugue::nodes
