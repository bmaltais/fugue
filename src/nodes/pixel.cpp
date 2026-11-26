#include "nodes/kernels.h"

// Ticket #15: resize, crop, brightness-contrast, blend, text-concat (spec 14.1).
// Ticket #29: pad, rotate, flip, blur, sharpen, invert, mask tools, number math (spec 14.2).
//
// The pixel kernels work on float RGB(A) buffers in 0..1, as ComfyUI holds images. libvips
// decodes the input and holds the result (MediaHandle); the arithmetic is plain loops,
// because the point is ComfyUI's exact formulas (torch interpolate, PIL lanczos, its blend
// modes) and libvips' own resize kernels do not give the same pixels. Every result is
// clamped to 0..1 and materialised in memory, as spike 03 decided.
//
// Where each formula comes from: tests/nodes/test_pixel.cpp (ComfyUI commit 995456d).

#include <QString>

#include <algorithm>
#include <array>
#include <optional>
#include <cmath>
#include <utility>
#include <vector>

#include "engine/edge_fill.h"
#include "engine/mask_ops.h"
#include "engine/vips.h"
#include "nodes/pixel_io.h"
#include "nodes/resample.h"

namespace fugue::nodes {

namespace {

// The largest side Resize makes, as its manifest's `max` (the scheduler checks that first).
constexpr int kMaxSide = 16384;
constexpr int kMaxBlur = 512; // Pad's blur, as its manifest's `max`

// `side` scaled by to / from, rounded half to even (Python's round()), and at least 1.
int scaled(int side, int to, int from) { return std::max(1, int(std::nearbyint(double(side) * to / from))); }

// The width x height part of `in` whose top left corner is (x, y); it lies inside `in`.
Pixels region(const Pixels &in, int x, int y, int width, int height) {
    Pixels out(width, height, in.channels);
    for (int j = 0; j < height; ++j)
        for (int i = 0; i < width; ++i)
            for (int ch = 0; ch < in.channels; ++ch)
                out.at(i, j, ch) = in.at(i + x, j + y, ch);
    return out;
}

// A choice input (the compiler has already checked it is one of the manifest's) as its enum.
template <typename Enum, size_t N>
Result<Enum> choice(const NodeContext &context, const QString &input, const std::pair<const char *, Enum> (&names)[N]) {
    const QString text = context.input(input).text();
    for (const auto &[name, value] : names)
        if (text == QLatin1String(name))
            return value;
    return makeInputError(QStringLiteral("value_not_in_choices"), input,
                          QStringLiteral("\"%1\" is not a %2 this node knows.").arg(text, input));
}

// ---- resampling: torch.nn.functional.interpolate and PIL, per axis -----------

constexpr std::pair<const char *, ResizeMethod> kResizeMethods[] = {
    {"nearest", ResizeMethod::Nearest}, {"bilinear", ResizeMethod::Bilinear}, {"bicubic", ResizeMethod::Bicubic},
    {"lanczos", ResizeMethod::Lanczos}, {"area", ResizeMethod::Area},
};

// common_upscale(crop="center"): trim the long side to the target aspect. Python's round()
// goes half to even, which is what nearbyint does in the default rounding mode.
Pixels centerCrop(const Pixels &in, int width, int height) {
    const double oldAspect = double(in.width) / in.height;
    const double newAspect = double(width) / height;
    int x = 0, y = 0;
    if (oldAspect > newAspect)
        x = int(std::nearbyint((in.width - in.width * (newAspect / oldAspect)) / 2));
    else if (oldAspect < newAspect)
        y = int(std::nearbyint((in.height - in.height * (oldAspect / newAspect)) / 2));
    return region(in, x, y, in.width - 2 * x, in.height - 2 * y);
}

// ---- kernels ---------------------------------------------------------------------

class ResizeKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        const Result<ResizeMethod> method = choice(context, QStringLiteral("method"), kResizeMethods);
        if (!method)
            return method.errors();
        const Pixels &in = loaded.value();
        int width = toInt(context.input("width"));
        int height = toInt(context.input("height"));
        for (const auto &[side, value] : {std::pair{"width", width}, std::pair{"height", height}})
            if (value < 0 || value > kMaxSide)
                return makeInputError(QStringLiteral("value_out_of_range"), QString::fromLatin1(side),
                                      QStringLiteral("A %1 of %2 is outside 0 to %3.").arg(QString::fromLatin1(side)).arg(value).arg(kMaxSide));
        const QString fit = context.input("fit").text();
        if (width == 0 && height == 0)
            return Values{{"image", context.input("image")}};
        if (width == 0)
            width = scaled(in.width, height, in.height);
        else if (height == 0)
            height = scaled(in.height, width, in.width);

        if (fit == "contain") {
            // Fit inside the box: the side that limits takes the box's size exactly.
            if (double(width) / in.width <= double(height) / in.height)
                height = std::min(height, scaled(in.height, width, in.width));
            else
                width = std::min(width, scaled(in.width, height, in.height));
            return imageOutput(resample(in, width, height, *method));
        }
        if (fit == "cover")
            return imageOutput(resample(centerCrop(in, width, height), width, height, *method));
        return imageOutput(resample(in, width, height, *method));
    }
};

// ComfyUI's ImageScaleToTotalPixels (crop "disabled"): scale to megapixels * 1024 * 1024 pixels,
// each side rounded half to even to a multiple, then stretch to that size. The aspect can drift
// by the rounding. ComfyUI fails on a side that rounds to 0; here it becomes one multiple.
class ResizeMegapixelsKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        const Result<ResizeMethod> method = choice(context, QStringLiteral("method"), kResizeMethods);
        if (!method)
            return method.errors();
        const Pixels &in = loaded.value();
        const double megapixels = context.input("megapixels").number();
        const int multiple = std::max(1, toInt(context.input("multiple")));
        const double total = megapixels * 1024 * 1024;
        const double scale = std::sqrt(total / (double(in.width) * in.height));
        const double width = std::nearbyint(in.width * scale / multiple) * multiple;
        const double height = std::nearbyint(in.height * scale / multiple) * multiple;
        for (const auto &[side, value] : {std::pair{"width", width}, std::pair{"height", height}})
            if (!(value <= kMaxSide))
                return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("megapixels"),
                                      QStringLiteral("%1 megapixels makes a %2 of %3, above %4.")
                                          .arg(megapixels).arg(QString::fromLatin1(side)).arg(value).arg(kMaxSide));
        const int w = width < 1 ? multiple : int(width);
        const int h = height < 1 ? multiple : int(height);
        const Result<Values> image = imageOutput(resample(in, w, h, *method));
        if (!image.ok())
            return image;
        Values out = image.value();
        out.insert("width", Value(double(w)));
        out.insert("height", Value(double(h)));
        return out;
    }
};

// Upscale Image: Resize's resampling by a factor (ComfyUI's ImageScaleBy), each side rounded half
// to even. The kernel is Resize's `resample`, so the two give the same pixels for the same size.
class UpscaleImageKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        const Result<ResizeMethod> method = choice(context, QStringLiteral("method"), kResizeMethods);
        if (!method)
            return method.errors();
        const Pixels &in = loaded.value();
        const double scale = context.input("scale").number();
        const double width = std::max(1.0, std::nearbyint(in.width * scale));
        const double height = std::max(1.0, std::nearbyint(in.height * scale));
        for (const auto &[side, value] : {std::pair{"width", width}, std::pair{"height", height}})
            if (!(value <= kMaxSide))
                return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("scale"),
                                      QStringLiteral("A scale of %1 makes a %2 of %3, above %4.")
                                          .arg(scale).arg(QString::fromLatin1(side)).arg(value).arg(kMaxSide));
        const int w = int(width), h = int(height);
        const Result<Values> image = imageOutput(resample(in, w, h, *method));
        if (!image.ok())
            return image;
        Values out = image.value();
        out.insert("width", Value(double(w)));
        out.insert("height", Value(double(h)));
        return out;
    }
};

class CropKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        const Pixels &in = loaded.value();
        // ComfyUI ImageCrop: the origin is held inside the image, the far edge is a Python slice.
        // Also held at 0, which the manifest's minimum already asks for.
        const int x = std::clamp(toInt(context.input("x")), 0, in.width - 1);
        const int y = std::clamp(toInt(context.input("y")), 0, in.height - 1);
        const int width = std::min(std::max(toInt(context.input("width")), 1), in.width - x);
        const int height = std::min(std::max(toInt(context.input("height")), 1), in.height - y);
        return imageOutput(region(in, x, y, width, height));
    }
};

class BrightnessContrastKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        Pixels pixels = std::move(loaded.value());
        const float brightness = float(context.input("brightness").number());
        const float contrast = float(context.input("contrast").number());
        const size_t colors = pixels.channels == 4 ? 3 : size_t(pixels.channels);
        for (size_t i = 0; i < pixels.data.size(); ++i)
            if (i % size_t(pixels.channels) < colors)
                pixels.data[i] = (pixels.data[i] - 0.5f) * contrast + 0.5f + brightness;
        return imageOutput(std::move(pixels));
    }
};

// ---- blending ---------------------------------------------------------------------

enum class BlendMode { Normal, Multiply, Screen, Overlay, SoftLight, Difference, Subtract };

constexpr std::pair<const char *, BlendMode> kBlendModes[] = {
    {"normal", BlendMode::Normal},         {"multiply", BlendMode::Multiply},     {"screen", BlendMode::Screen},
    {"overlay", BlendMode::Overlay},       {"soft_light", BlendMode::SoftLight},  {"difference", BlendMode::Difference},
    {"subtract", BlendMode::Subtract},
};

float softLightG(float x) { return x <= 0.25f ? ((16 * x - 12) * x + 4) * x : std::sqrt(x); }

using Mix = float (*)(float base, float top);

// ComfyUI's blend formulas, but for difference: Photoshop's, GIMP's and CSS's |base - top|.
// ComfyUI's "difference" (signed, then clamped) is subtract.
Mix mixFor(BlendMode mode) {
    switch (mode) {
    case BlendMode::Normal:
        break;
    case BlendMode::Multiply:
        return [](float a, float b) { return a * b; };
    case BlendMode::Screen:
        return [](float a, float b) { return 1 - (1 - a) * (1 - b); };
    case BlendMode::Overlay:
        return [](float a, float b) { return a <= 0.5f ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b); };
    case BlendMode::SoftLight:
        return [](float a, float b) {
            return b <= 0.5f ? a - (1 - 2 * b) * a * (1 - a) : a + (2 * b - 1) * (softLightG(a) - a);
        };
    case BlendMode::Difference:
        return [](float a, float b) { return std::fabs(a - b); };
    case BlendMode::Subtract:
        return [](float a, float b) { return a - b; };
    }
    return [](float, float b) { return b; };
}

// node_helpers.image_alpha_fix: `top` takes the base's channel count (a missing alpha is 1); a
// different size is resized to the base's (bicubic, center crop). Blend and Compare Images share it.
Pixels matchedTo(const Pixels &base, Pixels top) {
    if (top.channels != base.channels) {
        Pixels fixed(top.width, top.height, base.channels, 1.0f);
        for (int y = 0; y < top.height; ++y)
            for (int x = 0; x < top.width; ++x)
                for (int ch = 0; ch < std::min(top.channels, base.channels); ++ch)
                    fixed.at(x, y, ch) = top.at(x, y, ch);
        top = std::move(fixed);
    }
    if (top.width != base.width || top.height != base.height)
        top = resample(centerCrop(top, base.width, base.height), base.width, base.height, ResizeMethod::Bicubic);
    return top;
}

class BlendKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        Result<Pixels> baseLoaded = pixelsOf(context.input("base"), "base");
        if (!baseLoaded.ok())
            return Result<Values>(baseLoaded.errors());
        Result<Pixels> topLoaded = pixelsOf(context.input("top"), "top");
        if (!topLoaded.ok())
            return Result<Values>(topLoaded.errors());
        const Result<BlendMode> mode = choice(context, QStringLiteral("mode"), kBlendModes);
        if (!mode)
            return mode.errors();
        const Pixels base = std::move(baseLoaded.value());
        Pixels top = std::move(topLoaded.value());
        const Mix mix = mixFor(*mode);
        const float opacity = float(context.input("opacity").number());

        top = matchedTo(base, std::move(top));

        Pixels out = base;
        for (size_t i = 0; i < out.data.size(); ++i) {
            const float a = base.data[i];
            out.data[i] = a * (1 - opacity) + mix(a, top.data[i]) * opacity;
        }
        if (base.channels == 4) // alpha stores transparency, not color
            for (int y = 0; y < base.height; ++y)
                for (int x = 0; x < base.width; ++x)
                    out.at(x, y, 3) = base.at(x, y, 3);
        return imageOutput(std::move(out));
    }
};

// ---- compare images ------------------------------------------------------------------

// The colors only: alpha is left off, so a comparison is of what the pictures show.
Pixels opaque(const Pixels &in) {
    if (in.channels == 3)
        return in;
    Pixels out(in.width, in.height, 3);
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < in.width; ++x)
            for (int ch = 0; ch < 3; ++ch)
                out.at(x, y, ch) = in.at(x, y, ch);
    return out;
}

enum class CompareMode { Slider, SliderHorizontal, SideBySide, A, B, Difference };

constexpr std::pair<const char *, CompareMode> kCompareModes[] = {
    {"slider", CompareMode::Slider},       {"slider_horizontal", CompareMode::SliderHorizontal},
    {"side_by_side", CompareMode::SideBySide}, {"a", CompareMode::A},
    {"b", CompareMode::B},                 {"difference", CompareMode::Difference},
};

// Compare Images (#62): B is fitted to A as Blend fits its top (bicubic, center crop), then the
// difference is |A - B| times the gain (Blend's difference formula) and the composite is the
// picture of `mode` at `position`. The window's viewer shows these files and the inputs; it
// flips and wipes without running again.
class CompareImagesKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        Result<Pixels> aLoaded = pixelsOf(context.input("a"), "a");
        if (!aLoaded.ok())
            return Result<Values>(aLoaded.errors());
        Result<Pixels> bLoaded = pixelsOf(context.input("b"), "b");
        if (!bLoaded.ok())
            return Result<Values>(bLoaded.errors());
        const Result<CompareMode> mode = choice(context, QStringLiteral("mode"), kCompareModes);
        if (!mode)
            return mode.errors();
        const float position = float(std::clamp(context.input("position").number(), 0.0, 1.0));
        const float gain = float(std::max(0.0, context.input("gain").number()));

        const Pixels a = opaque(aLoaded.value());
        const Pixels b = matchedTo(a, opaque(bLoaded.value()));
        const Mix mix = mixFor(BlendMode::Difference);
        Pixels difference(a.width, a.height, 3);
        for (size_t i = 0; i < difference.data.size(); ++i)
            difference.data[i] = mix(a.data[i], b.data[i]) * gain;

        Pixels composite;
        switch (*mode) {
        case CompareMode::A:
            composite = a;
            break;
        case CompareMode::B:
            composite = b;
            break;
        case CompareMode::Difference:
            composite = difference;
            break;
        case CompareMode::SideBySide:
            composite = Pixels(2 * a.width, a.height, 3);
            for (int y = 0; y < a.height; ++y)
                for (int x = 0; x < a.width; ++x)
                    for (int ch = 0; ch < 3; ++ch) {
                        composite.at(x, y, ch) = a.at(x, y, ch);
                        composite.at(a.width + x, y, ch) = b.at(x, y, ch);
                    }
            break;
        case CompareMode::Slider:
        case CompareMode::SliderHorizontal: {
            composite = Pixels(a.width, a.height, 3);
            const bool vertical = *mode == CompareMode::Slider; // a vertical divider: A left of it, B right
            const int edge = int(std::lround((vertical ? a.width : a.height) * position));
            for (int y = 0; y < a.height; ++y)
                for (int x = 0; x < a.width; ++x) {
                    const Pixels &from = (vertical ? x : y) < edge ? a : b;
                    for (int ch = 0; ch < 3; ++ch)
                        composite.at(x, y, ch) = from.at(x, y, ch);
                }
            break;
        }
        }

        Result<Values> differenceOut = imageOutput(std::move(difference));
        if (!differenceOut.ok())
            return differenceOut;
        Result<Values> compositeOut = imageOutput(std::move(composite));
        if (!compositeOut.ok())
            return compositeOut;
        return Values{{"difference", differenceOut.value().value("image")},
                      {"composite", compositeOut.value().value("image")}};
    }
};

class TextConcatKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        return Values{{"text", Value(context.input("a").text() + context.input("separator").text() +
                                     context.input("b").text())}};
    }
};


// ---- more pixel nodes (ticket #29, spec 14.2) ---------------------------------------

// ComfyUI's ImagePadForOutpaint, with a choice of what the new area holds (#243; engine/edge_fill.h, also
// the canvas resize's): gray 0.5 (alpha 1) as ComfyUI, the nearest picture stretched outward ("edge"), that
// blurred ("edge-blur", the default: the benchmark's best outpaint start), or transparent. Mask 1 over the
// new area and 0 over the original; feathering fades the mask inward from the padded edges as ((f - d) / f)^2.
// What Pad's written sizes cannot be together: a target of one side only, or a target and a side. An input not
// known (wired, not run yet) is none, and a rule that needs it is not judged.
std::optional<Error> padConflict(std::optional<int> targetWidth, std::optional<int> targetHeight, const std::array<std::optional<int>, 4> &sides) {
    static constexpr const char *names[4] = {"left", "top", "right", "bottom"};
    if (targetWidth && targetHeight && (*targetWidth > 0) != (*targetHeight > 0))
        return makeInputError(QStringLiteral("input_invalid"), QString::fromLatin1(*targetWidth > 0 ? "target_height" : "target_width"),
                              QStringLiteral("Set both target_width and target_height, or neither."));
    if (targetWidth && targetHeight && *targetWidth > 0)
        for (size_t i = 0; i < sides.size(); ++i)
            if (sides[i] && *sides[i] != 0)
                return makeInputError(QStringLiteral("input_invalid"), QString::fromLatin1(names[i]),
                                      QStringLiteral("A target size and left, top, right or bottom cannot both be set."));
    return std::nullopt;
}

class PadKernel : public NodeKernel {
public:
    // Under validate: the target size and the four sides that are written agree (what a run says of them alone).
    Errors check(const NodeContext &context) const override {
        const auto known = [&](const char *name) {
            const Value value = context.input(QString::fromLatin1(name));
            return value.isEmpty() ? std::optional<int>() : std::optional<int>(toInt(value));
        };
        const std::optional<Error> conflict = padConflict(known("target_width"), known("target_height"),
                                                          {known("left"), known("top"), known("right"), known("bottom")});
        return conflict ? Errors{*conflict} : Errors{};
    }

    Result<Values> run(const NodeContext &context) override {
        Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        Pixels in = std::move(loaded.value());
        int sides[4];
        const char *names[4] = {"left", "top", "right", "bottom"};
        for (int i = 0; i < 4; ++i) {
            sides[i] = toInt(context.input(QString::fromLatin1(names[i])));
            if (sides[i] < 0 || sides[i] > kMaxSide)
                return makeInputError(QStringLiteral("value_out_of_range"), QString::fromLatin1(names[i]),
                                      QStringLiteral("A %1 of %2 is outside 0 to %3.")
                                          .arg(QString::fromLatin1(names[i])).arg(sides[i]).arg(kMaxSide));
        }
        int left = sides[0], top = sides[1], right = sides[2], bottom = sides[3];
        // The keep-area mask (marks what to keep), as the picture's size.
        const bool userMasked = context.input("mask").isMedia();
        Pixels keep;
        if (userMasked) {
            Result<Pixels> maskLoaded = maskOf(context.input("mask"), "mask");
            if (!maskLoaded.ok())
                return Result<Values>(maskLoaded.errors());
            keep = std::move(maskLoaded.value());
            if (keep.width != in.width || keep.height != in.height)
                return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("mask"),
                                      QStringLiteral("The mask is %1 x %2 but the image is %3 x %4.")
                                          .arg(keep.width).arg(keep.height).arg(in.width).arg(in.height));
        }
        // Target size: fit inside it (only ever smaller) and center; replaces the four sides.
        const int targetWidth = toInt(context.input("target_width"));
        const int targetHeight = toInt(context.input("target_height"));
        for (const auto &[name, value] : {std::pair{"target_width", targetWidth}, std::pair{"target_height", targetHeight}})
            if (value < 0 || value > kMaxSide)
                return makeInputError(QStringLiteral("value_out_of_range"), QString::fromLatin1(name),
                                      QStringLiteral("A %1 of %2 is outside 0 to %3.")
                                          .arg(QString::fromLatin1(name)).arg(value).arg(kMaxSide));
        if (const std::optional<Error> conflict = padConflict(targetWidth, targetHeight, {sides[0], sides[1], sides[2], sides[3]}))
            return *conflict;
        if (targetWidth > 0) {
            const double scale = std::min({1.0, double(targetWidth) / in.width, double(targetHeight) / in.height});
            if (scale < 1.0) {
                const int w = std::min(targetWidth, std::max(1, int(std::lround(in.width * scale))));
                const int h = std::min(targetHeight, std::max(1, int(std::lround(in.height * scale))));
                in = resample(in, w, h, ResizeMethod::Area);
                if (userMasked)
                    keep = resample(keep, w, h, ResizeMethod::Area);
            }
            const int roomX = targetWidth - in.width, roomY = targetHeight - in.height;
            top = roomY / 2;
            bottom = roomY - top;
            left = roomX / 2;
            right = roomX - left;
        }
        const int feathering = std::max(0, toInt(context.input("feathering")));
        const int width = in.width + left + right;
        const int height = in.height + top + bottom;
        if (width > kMaxSide || height > kMaxSide)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("left"),
                                  QStringLiteral("The padded image would be %1 x %2, past %3 on a side.")
                                      .arg(width).arg(height).arg(kMaxSide));
        const QString fillName = context.input("fill").text();
        edgefill::Mode fill = edgefill::Mode::EdgeBlur;
        if (!edgefill::modeFromName(fillName, &fill))
            return makeInputError(QStringLiteral("value_not_in_choices"), QStringLiteral("fill"),
                                  QStringLiteral("\"%1\" is not a fill this node knows.").arg(fillName));
        const int blur = int(std::lround(context.input("blur").number()));
        if (blur < 0 || blur > kMaxBlur)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("blur"),
                                  QStringLiteral("A blur of %1 is outside 0 to %2.").arg(blur).arg(kMaxBlur));
        // A transparent fill needs an alpha channel to be transparent in.
        const int channels = fill == edgefill::Mode::Transparent ? 4 : in.channels;
        Pixels image(width, height, channels, 0.0f);
        Pixels mask(width, height, 1, 1.0f);
        std::vector<uint8_t> hole(size_t(width) * size_t(height), 1);
        for (int y = 0; y < in.height; ++y)
            for (int x = 0; x < in.width; ++x) {
                hole[size_t(y + top) * width + size_t(x + left)] = 0;
                for (int ch = 0; ch < channels; ++ch)
                    image.at(x + left, y + top, ch) = ch < in.channels ? in.at(x, y, ch) : 1.0f;
            }
        edgefill::Image filled{width, height, channels, std::move(image.data)};
        edgefill::fillHole(filled, hole, fill, blur);
        image.data = std::move(filled.data);
        const bool feather = !userMasked && feathering > 0 && feathering * 2 < in.height && feathering * 2 < in.width;
        for (int y = 0; y < in.height; ++y)
            for (int x = 0; x < in.width; ++x) {
                float value = 0.0f;
                if (userMasked)
                    value = std::clamp(1.0f - keep.at(x, y, 0), 0.0f, 1.0f); // 0 outside the keep area, inverted
                else if (feather) {
                    const int dt = top != 0 ? y : in.height;
                    const int db = bottom != 0 ? in.height - y : in.height;
                    const int dl = left != 0 ? x : in.width;
                    const int dr = right != 0 ? in.width - x : in.width;
                    const int d = std::min({dt, db, dl, dr});
                    if (d < feathering) {
                        const float v = float(feathering - d) / float(feathering);
                        value = v * v;
                    }
                }
                mask.at(x + left, y + top, 0) = value;
            }
        Result<Values> imageResult = imageOutput(std::move(image));
        if (!imageResult.ok())
            return imageResult;
        const Result<Values> maskResult = namedOutput(std::move(mask), "mask");
        if (!maskResult.ok())
            return maskResult;
        Values out = imageResult.value();
        out.insert("mask", maskResult.value().value("mask"));
        return out;
    }
};

// ComfyUI's ImageRotate: torch.rot90(k, dims=[2, 1]) turns clockwise.
class RotateKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        const Pixels &in = loaded.value();
        const QString rotation = context.input("rotation").text();
        const int turns = rotation.startsWith(QLatin1String("90")) ? 1
                          : rotation.startsWith(QLatin1String("180")) ? 2
                          : rotation.startsWith(QLatin1String("270")) ? 3 : 0;
        if (turns == 0)
            return Values{{"image", context.input("image")}};
        const bool swap = turns != 2;
        Pixels out(swap ? in.height : in.width, swap ? in.width : in.height, in.channels);
        for (int y = 0; y < in.height; ++y)
            for (int x = 0; x < in.width; ++x) {
                int ox = 0, oy = 0;
                if (turns == 1) {
                    ox = in.height - 1 - y;
                    oy = x;
                } else if (turns == 2) {
                    ox = in.width - 1 - x;
                    oy = in.height - 1 - y;
                } else {
                    ox = y;
                    oy = in.width - 1 - x;
                }
                for (int ch = 0; ch < in.channels; ++ch)
                    out.at(ox, oy, ch) = in.at(x, y, ch);
            }
        return imageOutput(std::move(out));
    }
};

class FlipKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        const Pixels &in = loaded.value();
        const bool vertical = context.input("direction").text() == QLatin1String("vertically");
        Pixels out(in.width, in.height, in.channels);
        for (int y = 0; y < in.height; ++y)
            for (int x = 0; x < in.width; ++x)
                for (int ch = 0; ch < in.channels; ++ch)
                    out.at(vertical ? x : in.width - 1 - x, vertical ? in.height - 1 - y : y, ch) = in.at(x, y, ch);
        return imageOutput(std::move(out));
    }
};

class InvertKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        Pixels pixels = std::move(loaded.value());
        for (size_t i = 0; i < pixels.data.size(); ++i)
            if (pixels.channels != 4 || i % 4 != 3) // alpha stores transparency, not color
                pixels.data[i] = 1.0f - pixels.data[i];
        return imageOutput(std::move(pixels));
    }
};

// ComfyUI's gaussian_kernel: a size x size grid over -1..1, weight exp(-d^2 / (2 sigma^2)),
// summing to 1. Sigma is over the kernel's half width, not in pixels.
std::vector<float> gaussianKernel(int size, double sigma) {
    std::vector<double> g(size_t(size) * size);
    double sum = 0;
    for (int i = 0; i < size; ++i)
        for (int j = 0; j < size; ++j) {
            const double x = size == 1 ? -1.0 : -1.0 + 2.0 * i / (size - 1);
            const double y = size == 1 ? -1.0 : -1.0 + 2.0 * j / (size - 1);
            g[size_t(i) * size + j] = std::exp(-(x * x + y * y) / (2.0 * sigma * sigma));
            sum += g[size_t(i) * size + j];
        }
    std::vector<float> kernel(g.size());
    for (size_t i = 0; i < kernel.size(); ++i)
        kernel[i] = float(g[i] / sum);
    return kernel;
}

// torch's 'reflect' padding index (the edge pixel is not repeated), folded for any distance.
int reflectIndex(int i, int n) {
    if (n == 1)
        return 0;
    const int period = 2 * (n - 1);
    i %= period;
    if (i < 0)
        i += period;
    return i < n ? i : period - i;
}

// Reflect-padded convolution of the first `channels` channels with a size x size kernel;
// the others pass through.
Pixels convolve(const Pixels &in, const std::vector<float> &kernel, int size, int channels) {
    const int radius = size / 2;
    Pixels out = in;
    std::vector<int> xs(size_t(in.width) * size), ys(size_t(in.height) * size);
    for (int x = 0; x < in.width; ++x)
        for (int k = 0; k < size; ++k)
            xs[size_t(x) * size + k] = reflectIndex(x + k - radius, in.width);
    for (int y = 0; y < in.height; ++y)
        for (int k = 0; k < size; ++k)
            ys[size_t(y) * size + k] = reflectIndex(y + k - radius, in.height);
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < in.width; ++x)
            for (int ch = 0; ch < channels; ++ch) {
                float acc = 0;
                for (int j = 0; j < size; ++j)
                    for (int i = 0; i < size; ++i)
                        acc += kernel[size_t(j) * size + i] *
                               in.at(xs[size_t(x) * size + i], ys[size_t(y) * size + j], ch);
                out.at(x, y, ch) = acc;
            }
    return out;
}

Result<int> radiusOf(const NodeContext &context) {
    const int radius = toInt(context.input("radius"));
    if (radius < 1 || radius > 31)
        return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("radius"),
                              QStringLiteral("A radius of %1 is outside 1 to 31.").arg(radius));
    return radius;
}

// ComfyUI's Blur (ImageBlur): every channel, alpha too.
class BlurKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        const Result<int> radius = radiusOf(context);
        if (!radius)
            return radius.errors();
        const int size = *radius * 2 + 1;
        const double sigma = std::max(context.input("sigma").number(), 1e-3);
        const Pixels &in = loaded.value();
        return imageOutput(convolve(in, gaussianKernel(size, sigma), size, in.channels));
    }
};

// ComfyUI's Sharpen (ImageSharpen), `amount` being its `alpha`; Fugue leaves an alpha channel alone.
class SharpenKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        const Result<int> radius = radiusOf(context);
        if (!radius)
            return radius.errors();
        const int size = *radius * 2 + 1;
        const double sigma = std::max(context.input("sigma").number(), 1e-3);
        const float amount = float(context.input("amount").number());
        std::vector<float> kernel = gaussianKernel(size, sigma);
        float sum = 0;
        for (float &k : kernel) {
            k *= -(amount * 10);
            sum += k;
        }
        const size_t center = size_t(size / 2) * size + size / 2;
        kernel[center] = kernel[center] - sum + 1.0f;
        const Pixels &in = loaded.value();
        return imageOutput(convolve(in, kernel, size, in.channels == 4 ? 3 : in.channels));
    }
};

// ---- masks ---------------------------------------------------------------------------

// ComfyUI's GrowMask: |expand| passes of a 3x3 grey dilation (positive) or erosion (negative),
// the footprint's corners left out when tapered. Neighbors outside the image are ignored,
// which is what scipy's reflect mode comes to for a max or a min.
class GrowMaskKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        Result<Pixels> loaded = maskOf(context.input("mask"), "mask");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        Pixels mask = std::move(loaded.value());
        const int expand = toInt(context.input("expand"));
        const bool tapered = context.input("tapered_corners").json().toBool(true);
        const bool grow = expand > 0;
        Pixels next = mask;
        for (int pass = 0; pass < std::abs(expand); ++pass) { // the pass is shared with Select > Modify (engine/mask_ops.h)
            fugue::mask::morphPass(mask.data.data(), next.data.data(), mask.width, mask.height, grow, tapered);
            std::swap(mask, next);
        }
        return namedOutput(std::move(mask), "mask");
    }
};

class InvertMaskKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        Result<Pixels> loaded = maskOf(context.input("mask"), "mask");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        Pixels mask = std::move(loaded.value());
        for (float &v : mask.data)
            v = 1.0f - v;
        return namedOutput(std::move(mask), "mask");
    }
};

// ComfyUI's ImageToMask. An image without alpha has no transparency, so its alpha is 1
// everywhere (ComfyUI fails on the missing channel).
class ImageToMaskKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        const Pixels &in = loaded.value();
        const QString channel = context.input("channel").text();
        const int index = channel == QLatin1String("red") ? 0 : channel == QLatin1String("green") ? 1
                          : channel == QLatin1String("blue") ? 2 : 3;
        Pixels mask(in.width, in.height, 1, 1.0f);
        if (index < in.channels)
            for (int y = 0; y < in.height; ++y)
                for (int x = 0; x < in.width; ++x)
                    mask.at(x, y, 0) = in.at(x, y, index);
        return namedOutput(std::move(mask), "mask");
    }
};

class SolidMaskKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const int width = toInt(context.input("width"));
        const int height = toInt(context.input("height"));
        for (const auto &[side, value] : {std::pair{"width", width}, std::pair{"height", height}})
            if (value < 1 || value > kMaxSide)
                return makeInputError(QStringLiteral("value_out_of_range"), QString::fromLatin1(side),
                                      QStringLiteral("A %1 of %2 is outside 1 to %3.")
                                          .arg(QString::fromLatin1(side)).arg(value).arg(kMaxSide));
        return namedOutput(Pixels(width, height, 1, float(context.input("value").number())), "mask");
    }
};

// ---- numbers --------------------------------------------------------------------------

// Python's operators: `%` takes the sign of b; dividing by zero is an error, as it is in Python.
// What Number Math makes of its inputs, or the error a run gives: one function for run and check.
Result<double> numberMath(const QString &op, double a, double b) {
    double result = 0;
    if (op == QLatin1String("add")) {
        result = a + b;
    } else if (op == QLatin1String("subtract")) {
        result = a - b;
    } else if (op == QLatin1String("multiply")) {
        result = a * b;
    } else if (op == QLatin1String("min")) {
        result = std::min(a, b);
    } else if (op == QLatin1String("max")) {
        result = std::max(a, b);
    } else if (op == QLatin1String("power")) {
        result = std::pow(a, b);
    } else if (op == QLatin1String("divide") || op == QLatin1String("modulo")) {
        if (b == 0)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("b"),
                                  QStringLiteral("Cannot %1 by zero.")
                                      .arg(op == QLatin1String("divide") ? QStringLiteral("divide") : QStringLiteral("take a modulo")));
        if (op == QLatin1String("divide")) {
            result = a / b;
        } else {
            result = std::fmod(a, b);
            if (result != 0 && (result < 0) != (b < 0))
                result += b;
        }
    } else {
        return makeInputError(QStringLiteral("value_not_in_choices"), QStringLiteral("operation"),
                              QStringLiteral("\"%1\" is not an operation this node knows.").arg(op));
    }
    if (!std::isfinite(result))
        return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("a"), QStringLiteral("The result is not a finite number."));
    return result;
}

class NumberMathKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<double> result = numberMath(context.input("operation").text(), context.input("a").number(), context.input("b").number());
        if (!result)
            return result.errors();
        return Values{{"value", Value(result.value())}};
    }

    // Under validate: a division by zero, or a result that is no number, from what is written. An `a` not known yet
    // is taken as 0 and only the error on `b` is kept: the result then says nothing.
    Errors check(const NodeContext &context) const override {
        if (!context.inputs.contains(QStringLiteral("b")) || !context.inputs.contains(QStringLiteral("operation")))
            return {};
        const bool haveA = context.inputs.contains(QStringLiteral("a"));
        const Result<double> result = numberMath(context.input("operation").text(), context.input("a").number(), context.input("b").number());
        if (result || (!haveA && result.error().input != QLatin1String("b")))
            return {};
        return result.errors();
    }
};

} // namespace

void addPixelKernels(Kernels &kernels) {
    kernels.add("fugue/resize", std::make_shared<ResizeKernel>());
    kernels.add("fugue/resize-megapixels", std::make_shared<ResizeMegapixelsKernel>());
    kernels.add("fugue/upscale-image", std::make_shared<UpscaleImageKernel>());
    kernels.add("fugue/crop", std::make_shared<CropKernel>());
    kernels.add("fugue/brightness-contrast", std::make_shared<BrightnessContrastKernel>());
    kernels.add("fugue/blend", std::make_shared<BlendKernel>());
    kernels.add("fugue/compare-images", std::make_shared<CompareImagesKernel>());
    kernels.add("fugue/text-concat", std::make_shared<TextConcatKernel>());
    kernels.add("fugue/pad", std::make_shared<PadKernel>());
    kernels.add("fugue/rotate", std::make_shared<RotateKernel>());
    kernels.add("fugue/flip", std::make_shared<FlipKernel>());
    kernels.add("fugue/blur", std::make_shared<BlurKernel>());
    kernels.add("fugue/sharpen", std::make_shared<SharpenKernel>());
    kernels.add("fugue/invert", std::make_shared<InvertKernel>());
    kernels.add("fugue/grow-mask", std::make_shared<GrowMaskKernel>());
    kernels.add("fugue/invert-mask", std::make_shared<InvertMaskKernel>());
    kernels.add("fugue/image-to-mask", std::make_shared<ImageToMaskKernel>());
    kernels.add("fugue/solid-mask", std::make_shared<SolidMaskKernel>());
    kernels.add("fugue/number-math", std::make_shared<NumberMathKernel>());
}

} // namespace fugue::nodes
