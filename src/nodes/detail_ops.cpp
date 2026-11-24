#include "nodes/detail_ops.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace fugue::nodes {

std::optional<std::vector<Corners>> parseBoxes(const std::string &text, std::string *problem) {
    std::vector<Corners> boxes;
    std::istringstream lines(text);
    std::string line;
    int number = 0;
    while (std::getline(lines, line)) {
        ++number;
        std::istringstream words(line);
        double v[4];
        std::string rest;
        if (!(words >> v[0])) // a blank line
            continue;
        if (!(words >> v[1] >> v[2] >> v[3]) || (words >> rest) || !std::isfinite(v[0] + v[1] + v[2] + v[3])) {
            if (problem)
                *problem = "line " + std::to_string(number) + " is not four numbers (x1 y1 x2 y2)";
            return std::nullopt;
        }
        boxes.push_back({v[0], v[1], v[2], v[3]});
    }
    return boxes;
}

std::optional<CropBox> detailCrop(const Corners &box, int padding, int imageWidth, int imageHeight) {
    const long long left = std::llround(std::min(box.x1, box.x2)) - std::max(0, padding);
    const long long top = std::llround(std::min(box.y1, box.y2)) - std::max(0, padding);
    const long long right = std::llround(std::max(box.x1, box.x2)) + std::max(0, padding);
    const long long bottom = std::llround(std::max(box.y1, box.y2)) + std::max(0, padding);
    const int x1 = int(std::clamp<long long>(left, 0, imageWidth));
    const int y1 = int(std::clamp<long long>(top, 0, imageHeight));
    const int x2 = int(std::clamp<long long>(right, 0, imageWidth));
    const int y2 = int(std::clamp<long long>(bottom, 0, imageHeight));
    if (x2 - x1 < kMinCropSide || y2 - y1 < kMinCropSide)
        return std::nullopt;
    return CropBox{x1, y1, x2 - x1, y2 - y1};
}

CropSize detailSize(const CropBox &crop, int targetSize, int multiple) {
    multiple = std::max(1, multiple);
    const auto stepped = [&](double side) { return std::max(multiple, int(std::lround(side / multiple)) * multiple); };
    const int longSide = stepped(targetSize);
    const bool wide = crop.width >= crop.height;
    const double scale = double(longSide) / (wide ? crop.width : crop.height);
    const int shortSide = std::min(longSide, std::max(stepped(64), stepped((wide ? crop.height : crop.width) * scale)));
    return wide ? CropSize{longSide, shortSide} : CropSize{shortSide, longSide};
}

double scaledDenoise(double base, DenoiseScale scale, double boxArea, double imageArea) {
    if (scale == DenoiseScale::Fixed)
        return base;
    const double ratio = std::clamp(boxArea / imageArea, 0.01, 0.5) / 0.5;
    const double factor = scale == DenoiseScale::SmallerLess ? 0.4 + 0.6 * ratio : 1.0 - 0.6 * ratio;
    return std::round(std::clamp(base * factor, 0.05, 1.0) * 1000.0) / 1000.0;
}

std::int64_t detailSeed(std::int64_t seed, std::int64_t shift, int index) {
    const std::uint64_t sum = std::uint64_t(seed) + std::uint64_t(shift) * 100000u + std::uint64_t(index);
    return std::int64_t(sum);
}

Pixels featherWeights(int width, int height, int feather) {
    Pixels weights(width, height, 1, 1.0f);
    const int f = std::max(0, std::min({feather, height / 2, width / 2}));
    if (f == 0)
        return weights;
    const auto ramp = [&](int i) { return f > 1 ? float(i) / float(f - 1) : 0.0f; };
    for (int i = 0; i < f; ++i) {
        const float r = ramp(i);
        for (int x = 0; x < width; ++x) {
            weights.at(x, i, 0) *= r;
            weights.at(x, height - 1 - i, 0) *= r;
        }
        for (int y = 0; y < height; ++y) {
            weights.at(i, y, 0) *= r;
            weights.at(width - 1 - i, y, 0) *= r;
        }
    }
    return weights;
}

void pasteFeathered(Pixels &canvas, const CropBox &box, const Pixels &redrawn, int feather) {
    const Pixels weights = featherWeights(box.width, box.height, feather);
    const int channels = std::min(canvas.channels, redrawn.channels);
    for (int y = 0; y < box.height; ++y)
        for (int x = 0; x < box.width; ++x) {
            const float w = weights.at(x, y, 0);
            for (int c = 0; c < channels; ++c) {
                float &dst = canvas.at(box.x + x, box.y + y, c);
                dst = dst * (1.0f - w) + redrawn.at(x, y, c) * w;
            }
        }
}

} // namespace fugue::nodes
