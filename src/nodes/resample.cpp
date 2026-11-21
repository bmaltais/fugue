#include "nodes/resample.h"

#include <algorithm>
#include <cmath>

namespace fugue::nodes {

namespace {

struct Tap {
    int index;
    float weight;
};

double cubicNear(double x, double A) { return ((A + 2) * x - (A + 3)) * x * x + 1; }
double cubicFar(double x, double A) { return ((A * x - 5 * A) * x + 8 * A) * x - 4 * A; }
double sinc(double x) { return x == 0 ? 1.0 : std::sin(M_PI * x) / (M_PI * x); }
double lanczos3(double x) { return (x > -3 && x < 3) ? sinc(x) * sinc(x / 3) : 0.0; }

// The source pixels and weights of output index `o` along an axis of `in` pixels resized to `out`.
std::vector<Tap> tapsFor(ResizeMethod method, int in, int out, int o) {
    const double scale = double(in) / out;
    std::vector<Tap> taps;
    switch (method) {
    case ResizeMethod::Nearest: // "nearest-exact"
        taps.push_back({std::min(int(std::floor((o + 0.5) * scale)), in - 1), 1.0f});
        break;
    case ResizeMethod::Bilinear: { // align_corners=False
        const double src = std::max(scale * (o + 0.5) - 0.5, 0.0);
        const int i0 = int(std::floor(src));
        const int i1 = i0 + (i0 < in - 1 ? 1 : 0);
        const double l = src - i0;
        taps.push_back({i0, float(1 - l)});
        taps.push_back({i1, float(l)});
        break;
    }
    case ResizeMethod::Bicubic: { // A = -0.75, indices clamped at the edges
        const double src = scale * (o + 0.5) - 0.5;
        const int i0 = int(std::floor(src));
        const double f = src - i0;
        const double w[4] = {cubicFar(f + 1, -0.75), cubicNear(f, -0.75), cubicNear(1 - f, -0.75),
                             cubicFar(2 - f, -0.75)};
        for (int k = 0; k < 4; ++k)
            taps.push_back({std::clamp(i0 - 1 + k, 0, in - 1), float(w[k])});
        break;
    }
    case ResizeMethod::Area: { // adaptive_avg_pool2d
        const int start = int(std::floor(double(o) * in / out));
        const int end = int(std::ceil(double(o + 1) * in / out));
        for (int i = start; i < end; ++i)
            taps.push_back({i, 1.0f / float(end - start)});
        break;
    }
    case ResizeMethod::Lanczos: { // PIL's LANCZOS (a = 3), support widened when shrinking
        const double filterScale = std::max(scale, 1.0);
        const double support = 3.0 * filterScale;
        const double center = (o + 0.5) * scale;
        const int lo = std::max(int(center - support + 0.5), 0);
        const int hi = std::min(int(center + support + 0.5), in);
        std::vector<double> weights;
        double sum = 0;
        for (int i = lo; i < hi; ++i) {
            weights.push_back(lanczos3((i - center + 0.5) / filterScale));
            sum += weights.back();
        }
        for (int i = lo; i < hi; ++i)
            taps.push_back({i, float(weights[size_t(i - lo)] / sum)});
        break;
    }
    }
    return taps;
}

}

Pixels resample(const Pixels &in, int width, int height, ResizeMethod method) {
    Pixels src = in;
    // PIL resizes RGBA with premultiplied color; so does every interpolating method here.
    const bool premultiply = method != ResizeMethod::Nearest && in.channels == 4;
    if (premultiply)
        for (int y = 0; y < in.height; ++y)
            for (int x = 0; x < in.width; ++x)
                for (int ch = 0; ch < 3; ++ch)
                    src.at(x, y, ch) *= in.at(x, y, 3);

    std::vector<std::vector<Tap>> columns(static_cast<size_t>(width));
    std::vector<std::vector<Tap>> rows(static_cast<size_t>(height));
    for (int x = 0; x < width; ++x)
        columns[size_t(x)] = tapsFor(method, in.width, width, x);
    for (int y = 0; y < height; ++y)
        rows[size_t(y)] = tapsFor(method, in.height, height, y);

    // Horizontal pass, then vertical: the filters are separable.
    Pixels mid(width, in.height, in.channels);
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < width; ++x)
            for (int ch = 0; ch < in.channels; ++ch) {
                float acc = 0;
                for (const Tap &t : columns[size_t(x)])
                    acc += t.weight * src.at(t.index, y, ch);
                mid.at(x, y, ch) = acc;
            }
    Pixels out(width, height, in.channels);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            for (int ch = 0; ch < in.channels; ++ch) {
                float acc = 0;
                for (const Tap &t : rows[size_t(y)])
                    acc += t.weight * mid.at(x, t.index, ch);
                out.at(x, y, ch) = acc;
            }
            if (premultiply) {
                const float alpha = out.at(x, y, 3);
                for (int ch = 0; ch < 3; ++ch)
                    out.at(x, y, ch) = alpha > 0 ? out.at(x, y, ch) / alpha : 0.0f;
            }
        }
    return out;
}

} // namespace fugue::nodes
