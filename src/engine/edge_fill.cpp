#include "engine/edge_fill.h"

#include <algorithm>
#include <cmath>

namespace fugue::edgefill {

const char *modeName(Mode mode) {
    switch (mode) {
    case Mode::Gray: return "gray";
    case Mode::Edge: return "edge";
    case Mode::EdgeBlur: return "edge-blur";
    case Mode::Transparent: return "transparent";
    }
    return "edge-blur";
}

bool modeFromName(const QString &name, Mode *mode) {
    for (const Mode candidate : {Mode::Gray, Mode::Edge, Mode::EdgeBlur, Mode::Transparent})
        if (name == QLatin1String(modeName(candidate))) {
            *mode = candidate;
            return true;
        }
    return false;
}

std::vector<int> nearestKept(int width, int height, const std::vector<uint8_t> &hole) {
    const size_t count = size_t(width) * size_t(height);
    std::vector<int> source(count, -1);
    bool any = false;
    for (size_t i = 0; i < count; ++i)
        if (!hole[i]) {
            source[i] = int(i);
            any = true;
        }
    if (!any)
        return source;
    const auto squared = [width](int at, int from) {
        const long long dx = (at % width) - (from % width), dy = (at / width) - (from / width);
        return dx * dx + dy * dy;
    };
    const auto relax = [&](int x, int y, int nx, int ny) {
        if (nx < 0 || ny < 0 || nx >= width || ny >= height)
            return;
        const int other = source[size_t(ny) * width + nx];
        if (other < 0)
            return;
        const int at = y * width + x;
        if (source[size_t(at)] < 0 || squared(at, other) < squared(at, source[size_t(at)]))
            source[size_t(at)] = other;
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                relax(x, y, x - 1, y);
                relax(x, y, x - 1, y - 1);
                relax(x, y, x, y - 1);
                relax(x, y, x + 1, y - 1);
            }
        for (int y = height - 1; y >= 0; --y)
            for (int x = width - 1; x >= 0; --x) {
                relax(x, y, x + 1, y);
                relax(x, y, x + 1, y + 1);
                relax(x, y, x, y + 1);
                relax(x, y, x - 1, y + 1);
            }
    }
    return source;
}

namespace {

// One box pass along a line of `count` samples spaced `stride` apart, half-width `half`, the ends repeated.
void boxLine(float *line, size_t count, size_t stride, int half, std::vector<double> &scratch) {
    const long long n = (long long)count;
    scratch.resize(size_t(n) + 2 * size_t(half));
    for (long long i = -half; i < n + half; ++i)
        scratch[size_t(i + half)] = line[size_t(std::clamp<long long>(i, 0, n - 1)) * stride];
    double sum = 0;
    for (long long i = 0; i < 2LL * half + 1; ++i)
        sum += scratch[size_t(i)];
    const double width = double(2 * half + 1);
    for (long long i = 0; i < n; ++i) {
        line[size_t(i) * stride] = float(sum / width);
        if (i + 1 < n)
            sum += scratch[size_t(i + 2 * half + 1)] - scratch[size_t(i)];
    }
}

} // namespace

void blurImage(Image &image, int radius) {
    if (radius <= 0 || image.width == 0 || image.height == 0)
        return;
    const int channels = image.channels;
    const size_t pixels = size_t(image.width) * size_t(image.height);
    if (channels == 4)
        for (size_t i = 0; i < pixels; ++i)
            for (int c = 0; c < 3; ++c)
                image.data[i * 4 + size_t(c)] *= image.data[i * 4 + 3];
    // Three boxes of width w have the variance 3 (w^2 - 1) / 12; a Gaussian of sigma radius / 2 has radius^2 / 4.
    const int half = std::max(1, int(std::lround((std::sqrt(double(radius) * radius + 1.0) - 1.0) / 2.0)));
    std::vector<double> scratch;
    for (int pass = 0; pass < 3; ++pass)
        for (int c = 0; c < channels; ++c) {
            for (int y = 0; y < image.height; ++y)
                boxLine(image.data.data() + size_t(y) * image.width * channels + c, size_t(image.width), size_t(channels), half, scratch);
            for (int x = 0; x < image.width; ++x)
                boxLine(image.data.data() + size_t(x) * channels + c, size_t(image.height), size_t(image.width) * channels, half, scratch);
        }
    if (channels == 4)
        for (size_t i = 0; i < pixels; ++i) {
            const float alpha = image.data[i * 4 + 3];
            for (int c = 0; c < 3; ++c)
                image.data[i * 4 + size_t(c)] = alpha > 1e-6f ? std::clamp(image.data[i * 4 + size_t(c)] / alpha, 0.0f, 1.0f) : 0.0f;
        }
}

void fillHole(Image &image, const std::vector<uint8_t> &hole, Mode mode, int blur) {
    const size_t pixels = size_t(image.width) * size_t(image.height);
    const int channels = image.channels;
    const auto paint = [&](size_t i, float value, float alpha) {
        for (int c = 0; c < channels; ++c)
            image.data[i * size_t(channels) + size_t(c)] = (c == 3) ? alpha : value;
    };
    if (mode == Mode::Transparent && channels == 3)
        mode = Mode::Gray; // no alpha to hide the hole in
    if (mode == Mode::Gray || mode == Mode::Transparent) {
        for (size_t i = 0; i < pixels; ++i)
            if (hole[i])
                paint(i, mode == Mode::Gray ? 0.5f : 0.0f, mode == Mode::Gray ? 1.0f : 0.0f);
        return;
    }
    const std::vector<int> source = nearestKept(image.width, image.height, hole);
    if (!source.empty() && source[0] < 0 && pixels > 0) { // nothing outside the hole to extend
        fillHole(image, hole, Mode::Gray, blur);
        return;
    }
    Image stretched = image;
    for (size_t i = 0; i < pixels; ++i)
        if (hole[i])
            for (int c = 0; c < channels; ++c)
                stretched.data[i * size_t(channels) + size_t(c)] = image.data[size_t(source[i]) * size_t(channels) + size_t(c)];
    if (mode == Mode::EdgeBlur)
        blurImage(stretched, blur);
    for (size_t i = 0; i < pixels; ++i)
        if (hole[i])
            for (int c = 0; c < channels; ++c)
                image.data[i * size_t(channels) + size_t(c)] = stretched.data[i * size_t(channels) + size_t(c)];
}

} // namespace fugue::edgefill
