#include "nodes/restore_color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <future>

namespace fugue::nodes {

namespace {

// One blur level on one axis: [1 2 1] / 4 at distance `step`, the edge pixel repeated past the border.
void blurAxis(const std::vector<float> &in, std::vector<float> &out, int width, int height, int channels, bool horizontal, int step) {
    const int length = horizontal ? width : height;
    const size_t strideAlong = horizontal ? size_t(channels) : size_t(width) * channels;
    const int lines = horizontal ? height : width;
    const size_t strideAcross = horizontal ? size_t(width) * channels : size_t(channels);
    for (int line = 0; line < lines; ++line) {
        const size_t base = size_t(line) * strideAcross;
        for (int i = 0; i < length; ++i) {
            const size_t lo = base + size_t(std::max(0, i - step)) * strideAlong;
            const size_t mid = base + size_t(i) * strideAlong;
            const size_t hi = base + size_t(std::min(length - 1, i + step)) * strideAlong;
            for (int c = 0; c < channels; ++c)
                out[mid + c] = 0.25f * in[lo + c] + 0.5f * in[mid + c] + 0.25f * in[hi + c];
        }
    }
}

// The low-frequency part after `levels` blurs of dilation 1, 2, 4, ...
std::vector<float> lowPart(const Pixels &p, int levels) {
    std::vector<float> current = p.data, pass(p.data.size());
    for (int level = 0; level < levels; ++level) {
        const int step = 1 << level;
        blurAxis(current, pass, p.width, p.height, p.channels, true, step);
        blurAxis(pass, current, p.width, p.height, p.channels, false, step);
    }
    return current;
}

// The reference at the content's size and channel count (colour of the last band when it has fewer).
Pixels referenceLike(const Pixels &content, const Pixels &reference) {
    const Pixels sized = reference.width == content.width && reference.height == content.height
                             ? reference
                             : resample(reference, content.width, content.height, ResizeMethod::Bicubic);
    Pixels out(content.width, content.height, content.channels);
    for (int y = 0; y < content.height; ++y)
        for (int x = 0; x < content.width; ++x)
            for (int c = 0; c < content.channels; ++c)
                out.at(x, y, c) = sized.at(x, y, std::min(c, sized.channels - 1));
    return out;
}

// sRGB D65 (the standard matrices) and the CIE constants.
constexpr double kWhite[3] = {0.95047, 1.0, 1.08883};
constexpr double kEpsilon = 216.0 / 24389.0, kKappa = 24389.0 / 27.0;

double toLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double fromLinear(double v) { return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055; }
double labF(double t) { return t > kEpsilon ? std::cbrt(t) : (kKappa * t + 16.0) / 116.0; }
double labFInverse(double f) { return f * f * f > kEpsilon ? f * f * f : (116.0 * f - 16.0) / kKappa; }

} // namespace

Lab rgbToLab(float r, float g, float b) {
    const double lr = toLinear(r), lg = toLinear(g), lb = toLinear(b);
    const double x = 0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb;
    const double y = 0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb;
    const double z = 0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb;
    const double fx = labF(x / kWhite[0]), fy = labF(y / kWhite[1]), fz = labF(z / kWhite[2]);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

void labToRgb(const Lab &lab, float *rgb) {
    const double fy = (lab.l + 16.0) / 116.0, fx = fy + lab.a / 500.0, fz = fy - lab.b / 200.0;
    const double x = labFInverse(fx) * kWhite[0], y = labFInverse(fy) * kWhite[1], z = labFInverse(fz) * kWhite[2];
    const double lr = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    const double lg = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    const double lb = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
    rgb[0] = float(fromLinear(std::clamp(lr, 0.0, 1.0)));
    rgb[1] = float(fromLinear(std::clamp(lg, 0.0, 1.0)));
    rgb[2] = float(fromLinear(std::clamp(lb, 0.0, 1.0)));
}

Pixels waveletReconstruction(const Pixels &content, const Pixels &reference, int levels) {
    if (content.width == 0 || content.height == 0 || reference.width == 0 || reference.height == 0)
        return content;
    const std::vector<float> contentLow = lowPart(content, levels);
    const std::vector<float> styleLow = lowPart(referenceLike(content, reference), levels);
    Pixels out = content;
    const int colour = std::min(3, content.channels);
    for (size_t pixel = 0; pixel < size_t(content.width) * content.height; ++pixel)
        for (int c = 0; c < colour; ++c) {
            const size_t i = pixel * content.channels + c;
            out.data[i] = std::clamp(content.data[i] - contentLow[i] + styleLow[i], 0.0f, 1.0f);
        }
    return out;
}

void matchHistogram(std::vector<float> &values, std::vector<float> reference) {
    if (values.empty() || reference.empty())
        return;
    // Sorted with their places, so each value's rank is its position (one sort and no searching).
    std::vector<std::pair<float, uint32_t>> order(values.size());
    for (size_t i = 0; i < values.size(); ++i)
        order[i] = {values[i], uint32_t(i)};
    std::sort(order.begin(), order.end());
    std::sort(reference.begin(), reference.end());
    const double n = double(order.size()), m = double(reference.size());
    for (size_t begin = 0; begin < order.size();) {
        size_t end = begin + 1;
        while (end < order.size() && order[end].first == order[begin].first)
            ++end;
        // Equal values share the middle of their run as the rank share, and so one new value.
        const double rank = 0.5 * double(begin + end - 1) + 0.5;
        // The reference's value at that share, interpolated between its two nearest ranks.
        const double at = std::clamp(rank / n * m - 0.5, 0.0, m - 1.0);
        const size_t below = size_t(std::floor(at));
        const size_t above = std::min(below + 1, reference.size() - 1);
        const float matched = float(reference[below] + (at - double(below)) * (reference[above] - reference[below]));
        for (size_t i = begin; i < end; ++i)
            values[order[i].second] = matched;
        begin = end;
    }
}

Pixels labColorFix(const Pixels &content, const Pixels &reference) {
    Pixels out = waveletReconstruction(content, reference);
    if (out.width == 0 || out.height == 0 || reference.width == 0 || reference.height == 0)
        return out;
    const Pixels style = referenceLike(out, reference);
    const size_t count = size_t(out.width) * out.height;
    std::array<std::vector<float>, 3> own, target;
    for (int c = 0; c < 3; ++c) {
        own[c].resize(count);
        target[c].resize(count);
    }
    for (size_t i = 0; i < count; ++i) {
        const float *a = &out.data[i * out.channels], *b = &style.data[i * style.channels];
        const Lab mine = rgbToLab(a[0], a[1], a[2]), theirs = rgbToLab(b[0], b[1], b[2]);
        own[0][i] = float(mine.l);
        own[1][i] = float(mine.a);
        own[2][i] = float(mine.b);
        target[0][i] = float(theirs.l);
        target[1][i] = float(theirs.a);
        target[2][i] = float(theirs.b);
    }
    std::vector<float> matchedL = own[0];
    // The three channels are independent: a thread each.
    auto a = std::async(std::launch::async, [&] { matchHistogram(own[1], target[1]); });
    auto b = std::async(std::launch::async, [&] { matchHistogram(own[2], target[2]); });
    matchHistogram(matchedL, target[0]);
    a.get();
    b.get();
    for (size_t i = 0; i < count; ++i) {
        const double l = 0.8 * double(own[0][i]) + 0.2 * double(matchedL[i]);
        labToRgb({l, double(own[1][i]), double(own[2][i])}, &out.data[i * out.channels]);
    }
    return out;
}

} // namespace fugue::nodes
