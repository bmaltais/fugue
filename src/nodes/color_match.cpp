#include "nodes/color_match.h"

// Ticket #180: Color Match (spec 14.4). Per-channel mean and standard deviation transfer in
// Ruderman's l-alpha-beta space, as Reinhard et al. (2001) describe it. Written from the paper's
// formulas: the GPL color-matcher library and ComfyUI's nodes were not read.

#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>
#include <utility>

#include "nodes/kernels.h"
#include "nodes/mask_ops.h"
#include "nodes/pixel_io.h"

namespace fugue::nodes {

namespace {

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<std::array<double, 3>, 3>;

// The paper's RGB to LMS matrix (for linear RGB).
constexpr Mat3 kRgbToLms = {{{0.3811, 0.5783, 0.0402}, {0.1967, 0.7244, 0.0782}, {0.0241, 0.1288, 0.8444}}};

Mat3 inverse(const Mat3 &m) {
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const int r0 = (j + 1) % 3, r1 = (j + 2) % 3, c0 = (i + 1) % 3, c1 = (i + 2) % 3;
            r[i][j] = (m[r0][c0] * m[r1][c1] - m[r0][c1] * m[r1][c0]) / det;
        }
    return r;
}

const Mat3 kLmsToRgb = inverse(kRgbToLms);

constexpr double kFloor = 1e-5; // a black channel's log is finite; the same floor comes back out.

double toLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double fromLinear(double v) { return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055; }

Vec3 toLab(float r, float g, float b) {
    const Vec3 rgb = {toLinear(r), toLinear(g), toLinear(b)};
    Vec3 lms{};
    for (int i = 0; i < 3; ++i)
        lms[i] = std::log10(std::max(kFloor, kRgbToLms[i][0] * rgb[0] + kRgbToLms[i][1] * rgb[1] + kRgbToLms[i][2] * rgb[2]));
    const double s3 = std::sqrt(3.0), s6 = std::sqrt(6.0), s2 = std::sqrt(2.0);
    return {(lms[0] + lms[1] + lms[2]) / s3, (lms[0] + lms[1] - 2 * lms[2]) / s6, (lms[0] - lms[1]) / s2};
}

std::array<float, 3> fromLab(const Vec3 &lab) {
    const double s3 = std::sqrt(3.0), s6 = std::sqrt(6.0), s2 = std::sqrt(2.0);
    const Vec3 logs = {lab[0] / s3 + lab[1] / s6 + lab[2] / s2, lab[0] / s3 + lab[1] / s6 - lab[2] / s2,
                       lab[0] / s3 - 2 * lab[1] / s6};
    Vec3 lms{};
    for (int i = 0; i < 3; ++i)
        lms[i] = std::pow(10.0, logs[i]);
    std::array<float, 3> out{};
    for (int i = 0; i < 3; ++i)
        out[i] = float(fromLinear(std::clamp(kLmsToRgb[i][0] * lms[0] + kLmsToRgb[i][1] * lms[1] + kLmsToRgb[i][2] * lms[2], 0.0, 1.0)));
    return out;
}

// CIELAB (D65), for Match Color: L 0..100, a and b about -128..127. Linear sRGB to XYZ and back with the sRGB matrices.
Vec3 toCielab(float r, float g, float b) {
    const double lr = toLinear(r), lg = toLinear(g), lb = toLinear(b);
    const double x = (0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb) / 0.95047;
    const double y = 0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb;
    const double z = (0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb) / 1.08883;
    const auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0; };
    return {116.0 * f(y) - 16.0, 500.0 * (f(x) - f(y)), 200.0 * (f(y) - f(z))};
}

std::array<float, 3> fromCielab(const Vec3 &lab) {
    const double fy = (lab[0] + 16.0) / 116.0, fx = fy + lab[1] / 500.0, fz = fy - lab[2] / 200.0;
    const auto inv = [](double t) { return t * t * t > 0.008856 ? t * t * t : (t - 16.0 / 116.0) / 7.787; };
    const double x = inv(fx) * 0.95047, y = inv(fy), z = inv(fz) * 1.08883;
    const double lin[3] = {3.2404542 * x - 1.5371385 * y - 0.4985314 * z, -0.9692660 * x + 1.8760108 * y + 0.0415560 * z,
                           0.0556434 * x - 0.2040259 * y + 1.0572252 * z};
    std::array<float, 3> out{};
    for (int i = 0; i < 3; ++i)
        out[i] = float(fromLinear(std::clamp(lin[i], 0.0, 1.0)));
    return out;
}

struct Stats {
    Vec3 mean{};
    Vec3 deviation{};
    double weight = 0;
};

// Weighted mean and standard deviation of each l-alpha-beta axis. `weightAt` gives a pixel's weight.
template <typename WeightAt, typename Convert>
Stats statsOf(const Pixels &p, WeightAt weightAt, Convert convert) {
    Stats s;
    Vec3 sum{}, square{};
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x) {
            const double w = weightAt(x, y);
            if (w <= 0)
                continue;
            const Vec3 lab = convert(p.at(x, y, 0), p.at(x, y, 1), p.at(x, y, 2));
            s.weight += w;
            for (int i = 0; i < 3; ++i) {
                sum[i] += w * lab[i];
                square[i] += w * lab[i] * lab[i];
            }
        }
    if (s.weight <= 0)
        return s;
    for (int i = 0; i < 3; ++i) {
        s.mean[i] = sum[i] / s.weight;
        s.deviation[i] = std::sqrt(std::max(0.0, square[i] / s.weight - s.mean[i] * s.mean[i]));
    }
    return s;
}

// The statistics of the image and the reference in the space `convert` makes, the mask's weights applied as the header says.
struct Measured {
    Stats source, target;
};

template <typename Convert>
Measured measure(const Pixels &image, const Pixels &reference, const Pixels *weights, bool *allMasked, Convert convert) {
    const bool sameSize = reference.width == image.width && reference.height == image.height;
    // A weight is 1 - m: the pixels the mask leaves count, the masked ones do not.
    const auto weightAt = [&](int x, int y) { return weights ? 1.0 - double(std::clamp(weights->at(x, y, 0), 0.0f, 1.0f)) : 1.0; };
    const auto whole = [](int, int) { return 1.0; };
    Measured m;
    m.source = statsOf(image, weightAt, convert);
    bool fallback = false;
    if (m.source.weight <= 0) {
        fallback = true;
        m.source = statsOf(image, whole, convert);
        if (allMasked)
            *allMasked = true;
    }
    m.target = (sameSize && !fallback) ? statsOf(reference, weightAt, convert) : statsOf(reference, whole, convert);
    return m;
}

} // namespace

Pixels colorMatch(const Pixels &image, const Pixels &reference, const Pixels *weights, float strength, bool *allMasked) {
    if (allMasked)
        *allMasked = false;
    if (image.width == 0 || image.height == 0 || reference.width == 0 || reference.height == 0 || !(strength > 0.0f))
        return image;
    const float amount = std::min(strength, 1.0f);
    const Measured measured = measure(image, reference, weights, allMasked, toLab);
    const Stats &source = measured.source, &target = measured.target;

    // out = (x - mean_s) * (dev_t / dev_s) + mean_t, mixed with x by strength. An axis with no
    // spread in the image has no scale to match: it is moved only.
    Vec3 scale{}, shift{};
    for (int i = 0; i < 3; ++i) {
        scale[i] = source.deviation[i] > 1e-6 ? target.deviation[i] / source.deviation[i] : 1.0;
        shift[i] = target.mean[i] - scale[i] * source.mean[i];
    }
    Pixels out = image;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            const Vec3 lab = toLab(image.at(x, y, 0), image.at(x, y, 1), image.at(x, y, 2));
            Vec3 moved{};
            for (int i = 0; i < 3; ++i)
                moved[i] = lab[i] + amount * ((scale[i] * lab[i] + shift[i]) - lab[i]);
            const std::array<float, 3> rgb = fromLab(moved);
            for (int i = 0; i < 3; ++i)
                out.at(x, y, i) = rgb[i];
        }
    return out;
}

Pixels matchColor(const Pixels &image, const Pixels &reference, const Pixels *weights, float strength,
                  const MatchColorOptions &options, bool *allMasked) {
    if (allMasked)
        *allMasked = false;
    if (image.width == 0 || image.height == 0 || reference.width == 0 || reference.height == 0 || !(strength > 0.0f))
        return image;
    const float fade = std::min(strength, 1.0f);
    // Per pixel (L, a, b): the statistics are the means of L, a and b, and the spread of L and of the chroma sqrt(var a + var b).
    const Measured measured = measure(image, reference, weights, allMasked, toCielab);
    const Stats &src = measured.source, &ref = measured.target;
    const auto ratio = [](double target, double source) { return source > 1e-6 ? target / source : 1.0; };
    const double lScale = ratio(ref.deviation[0], src.deviation[0]) * double(options.luminance);
    const double abScale = ratio(std::hypot(ref.deviation[1], ref.deviation[2]), std::hypot(src.deviation[1], src.deviation[2]));
    const double intensity = double(options.colorIntensity); // scales the whole color, as Photoshop's: 0 is gray
    const double neutral = double(options.neutralize);

    Pixels out = image;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            const Vec3 lab = toCielab(image.at(x, y, 0), image.at(x, y, 1), image.at(x, y, 2));
            // a and b keep their spread around their own mean, scaled, and move to the reference's mean (Photoshop moves the
            // colors to the source's; cubiq's code adds no reference mean, so a picture kept its own hue). Neutralize pulls
            // that target mean to gray (a = b = 0): 1 removes every cast, the reference's included.
            const Vec3 moved = {(lab[0] - src.mean[0]) * lScale + ref.mean[0],
                                ((lab[1] - src.mean[1]) * abScale + (1.0 - neutral) * ref.mean[1]) * intensity,
                                ((lab[2] - src.mean[2]) * abScale + (1.0 - neutral) * ref.mean[2]) * intensity};
            const std::array<float, 3> rgb = fromCielab(moved);
            for (int i = 0; i < 3; ++i)
                out.at(x, y, i) = std::clamp(fade * rgb[i] + (1.0f - fade) * image.at(x, y, i), 0.0f, 1.0f);
        }
    return out;
}

namespace {

Error warning(Error error) {
    error.severity = Error::Severity::Warning;
    return error;
}

class ColorMatchKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> image = pixelsOf(context.input("image"), "image");
        if (!image.ok())
            return Result<Values>(image.errors());
        const Result<Pixels> reference = pixelsOf(context.input("reference"), "reference");
        if (!reference.ok())
            return Result<Values>(reference.errors());
        const double strength = context.input("strength").number();
        if (!(strength >= 0.0 && strength <= 1.0))
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("strength"),
                                  QStringLiteral("A strength of %1 is outside 0 to 1.").arg(strength));

        Errors warnings;
        Pixels mask;
        const bool masked = context.input("mask").isMedia();
        if (masked) {
            Result<Pixels> loaded = maskOf(context.input("mask"), "mask");
            if (!loaded.ok())
                return Result<Values>(loaded.errors());
            mask = std::move(loaded.value());
            if (mask.width != image->width || mask.height != image->height) {
                warnings << warning(makeInputError(QStringLiteral("mask_resized"), QStringLiteral("mask"),
                                                   QStringLiteral("The mask is %1 x %2 and the picture %3 x %4; the mask was scaled to the picture.")
                                                       .arg(mask.width).arg(mask.height).arg(image->width).arg(image->height)));
                mask = maskResized(mask, image->width, image->height);
            }
        }
        // A saved 1.0.0 node has no method: Reinhard, as before.
        const QString method = context.input("method").text();
        const bool matchColorMethod = method == QLatin1String("match-color");
        if (!method.isEmpty() && !matchColorMethod && method != QLatin1String("reinhard"))
            return makeInputError(QStringLiteral("value_not_in_choices"), QStringLiteral("method"),
                                  QStringLiteral("\"%1\" is not a method this node knows.").arg(method));
        MatchColorOptions options;
        for (const auto &[name, target, low, high] :
             {std::tuple{"luminance", &options.luminance, 0.0, 2.0}, std::tuple{"color_intensity", &options.colorIntensity, 0.0, 2.0},
              std::tuple{"neutralize", &options.neutralize, 0.0, 1.0}}) {
            const Value &value = context.input(name);
            if (value.isEmpty())
                continue; // absent in a workflow saved before 1.1.0: the default
            const double v = value.number();
            if (!(v >= low && v <= high))
                return makeInputError(QStringLiteral("value_out_of_range"), QString::fromLatin1(name),
                                      QStringLiteral("%1 is outside %2 to %3.").arg(v).arg(low).arg(high));
            *target = float(v);
        }
        bool allMasked = false;
        Result<Values> out = imageOutput(matchColorMethod
                                             ? matchColor(image.value(), reference.value(), masked ? &mask : nullptr, float(strength), options, &allMasked)
                                             : colorMatch(image.value(), reference.value(), masked ? &mask : nullptr, float(strength), &allMasked));
        if (allMasked)
            warnings << warning(makeInputError(QStringLiteral("mask_full"), QStringLiteral("mask"),
                                               QStringLiteral("The mask covers the whole picture, so there is nothing to measure the colors from; the whole picture was measured.")));
        if (!out.ok())
            return out;
        return Result<Values>(out.value(), warnings);
    }
};

} // namespace

void addColorMatchKernels(Kernels &kernels) {
    kernels.add("fugue/color-match", std::make_shared<ColorMatchKernel>());
}

} // namespace fugue::nodes
