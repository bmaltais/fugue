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
#include <cmath>
#include <utility>
#include <vector>

#include "engine/vips.h"

namespace fugue::nodes {

namespace {

struct Pixels {
    int width = 0;
    int height = 0;
    int channels = 0; // 3 (RGB) or 4 (RGBA)
    std::vector<float> data;

    Pixels() = default;
    Pixels(int width, int height, int channels, float fill = 0.0f)
        : width(width), height(height), channels(channels), data(size_t(width) * height * channels, fill) {}

    float &at(int x, int y, int channel) { return data[(size_t(y) * width + x) * channels + channel]; }
    float at(int x, int y, int channel) const { return data[(size_t(y) * width + x) * channels + channel]; }
};

Result<Pixels> pixelsOf(const Value &value, const QString &input) {
    if (!value.isMedia())
        return makeInputError(QStringLiteral("missing_input"), input, QStringLiteral("Input %1 has no image.").arg(input));
    const Result<vips::VImage> loaded = value.media().image();
    if (!loaded.ok())
        return Result<Pixels>(loaded.errors());
    try {
        initVips();
        vips::VImage image = loaded.value();
        // Gray (with alpha) becomes RGB (with alpha); more than four bands keep the first three.
        if (image.bands() == 1) {
            image = vips::VImage::bandjoin(std::vector<vips::VImage>{image, image, image});
        } else if (image.bands() == 2) {
            const vips::VImage gray = image.extract_band(0);
            image = vips::VImage::bandjoin(std::vector<vips::VImage>{gray, gray, gray, image.extract_band(1)});
        } else if (image.bands() > 4) {
            image = image.extract_band(0, vips::VImage::option()->set("n", 3));
        }
        double divisor = 1.0;
        switch (image.format()) {
        case VIPS_FORMAT_UCHAR:
        case VIPS_FORMAT_CHAR:
            divisor = 255.0;
            break;
        case VIPS_FORMAT_USHORT:
        case VIPS_FORMAT_SHORT:
            divisor = 65535.0;
            break;
        default:
            break;
        }
        image = image.cast(VIPS_FORMAT_FLOAT);
        size_t size = 0;
        void *memory = image.write_to_memory(&size);
        Pixels pixels;
        pixels.width = image.width();
        pixels.height = image.height();
        pixels.channels = image.bands();
        const float *begin = static_cast<const float *>(memory);
        pixels.data.assign(begin, begin + size / sizeof(float));
        g_free(memory);
        if (divisor != 1.0)
            for (float &v : pixels.data)
                v = float(v / divisor);
        return pixels;
    } catch (const vips::VError &error) {
        Error unreadable = vipsError(error, QStringLiteral("media_unreadable"));
        unreadable.input = input;
        return unreadable;
    }
}

Result<Values> imageOutput(Pixels pixels) {
    for (float &v : pixels.data)
        v = std::min(1.0f, std::max(0.0f, v));
    try {
        initVips();
        const vips::VImage image =
            vips::VImage::new_from_memory_copy(pixels.data.data(), pixels.data.size() * sizeof(float), pixels.width,
                                               pixels.height, pixels.channels, VIPS_FORMAT_FLOAT);
        const Result<MediaHandle> handle = MediaHandle::fromImage(image);
        if (!handle.ok())
            return Result<Values>(handle.errors());
        return Values{{"image", Value(handle.value())}};
    } catch (const vips::VError &error) {
        return vipsError(error);
    }
}

// A number input as a whole number, held within plus or minus a billion so a huge value cannot
// wrap around (lround past the int range is undefined); not a number is 0.
int toInt(const Value &value) {
    const double rounded = std::round(value.number());
    return std::isfinite(rounded) ? int(std::clamp(rounded, -1e9, 1e9)) : 0;
}

// The largest side Resize makes, as its manifest's `max` (the scheduler checks that first).
constexpr int kMaxSide = 16384;

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

enum class ResizeMethod { Nearest, Bilinear, Bicubic, Lanczos, Area };

constexpr std::pair<const char *, ResizeMethod> kResizeMethods[] = {
    {"nearest", ResizeMethod::Nearest}, {"bilinear", ResizeMethod::Bilinear}, {"bicubic", ResizeMethod::Bicubic},
    {"lanczos", ResizeMethod::Lanczos}, {"area", ResizeMethod::Area},
};

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

Pixels resample(const Pixels &in, int width, int height, ResizeMethod method) {
    Pixels src = in;
    // PIL resizes RGBA with premultiplied color, so lanczos does too.
    const bool premultiply = method == ResizeMethod::Lanczos && in.channels == 4;
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

        // node_helpers.image_alpha_fix: the top takes the base's channel count; a missing alpha is 1.
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

class TextConcatKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        return Values{{"text", Value(context.input("a").text() + context.input("separator").text() +
                                     context.input("b").text())}};
    }
};


// ---- more pixel nodes (ticket #29, spec 14.2) ---------------------------------------

// A mask input as one channel: an image's first channel (a gray mask is its own).
Result<Pixels> maskOf(const Value &value, const QString &input) {
    Result<Pixels> loaded = pixelsOf(value, input);
    if (!loaded.ok())
        return loaded;
    const Pixels &in = loaded.value();
    Pixels mask(in.width, in.height, 1);
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < in.width; ++x)
            mask.at(x, y, 0) = in.at(x, y, 0);
    return mask;
}

// A result image under the output name `name` ("image", or "mask" for a one-channel mask).
Result<Values> namedOutput(Pixels pixels, const char *name) {
    Result<Values> made = imageOutput(std::move(pixels));
    if (!made.ok() || QLatin1String(name) == QLatin1String("image"))
        return made;
    Values out;
    out.insert(QString::fromLatin1(name), made.value().value("image"));
    return out;
}

// ComfyUI's ImagePadForOutpaint: new area 0.5 gray (alpha 1), mask 1 over it and 0 over the
// original; feathering fades the mask inward from the padded edges as ((f - d) / f)^2.
class PadKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        const Pixels &in = loaded.value();
        int sides[4];
        const char *names[4] = {"left", "top", "right", "bottom"};
        for (int i = 0; i < 4; ++i) {
            sides[i] = toInt(context.input(QString::fromLatin1(names[i])));
            if (sides[i] < 0 || sides[i] > kMaxSide)
                return makeInputError(QStringLiteral("value_out_of_range"), QString::fromLatin1(names[i]),
                                      QStringLiteral("A %1 of %2 is outside 0 to %3.")
                                          .arg(QString::fromLatin1(names[i])).arg(sides[i]).arg(kMaxSide));
        }
        const int left = sides[0], top = sides[1], right = sides[2], bottom = sides[3];
        const int feathering = std::max(0, toInt(context.input("feathering")));
        const int width = in.width + left + right;
        const int height = in.height + top + bottom;
        if (width > kMaxSide || height > kMaxSide)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("left"),
                                  QStringLiteral("The padded image would be %1 x %2, past %3 on a side.")
                                      .arg(width).arg(height).arg(kMaxSide));
        Pixels image(width, height, in.channels, 0.5f);
        Pixels mask(width, height, 1, 1.0f);
        if (in.channels == 4)
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    image.at(x, y, 3) = 1.0f;
        const bool feather = feathering > 0 && feathering * 2 < in.height && feathering * 2 < in.width;
        for (int y = 0; y < in.height; ++y)
            for (int x = 0; x < in.width; ++x) {
                for (int ch = 0; ch < in.channels; ++ch)
                    image.at(x + left, y + top, ch) = in.at(x, y, ch);
                float value = 0.0f;
                if (feather) {
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
        const int w = mask.width, h = mask.height;
        Pixels next = mask;
        for (int pass = 0; pass < std::abs(expand); ++pass) {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    float best = mask.at(x, y, 0);
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (tapered && dx != 0 && dy != 0)
                                continue;
                            const int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                                continue;
                            const float v = mask.at(nx, ny, 0);
                            best = grow ? std::max(best, v) : std::min(best, v);
                        }
                    next.at(x, y, 0) = best;
                }
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
class NumberMathKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const double a = context.input("a").number();
        const double b = context.input("b").number();
        const QString op = context.input("operation").text();
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
                                          .arg(op == QLatin1String("divide") ? QStringLiteral("divide")
                                                                             : QStringLiteral("take a modulo")));
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
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("a"),
                                  QStringLiteral("The result is not a finite number."));
        return Values{{"value", Value(result)}};
    }
};

} // namespace

void addPixelKernels(Kernels &kernels) {
    kernels.add("fugue/resize", std::make_shared<ResizeKernel>());
    kernels.add("fugue/resize-megapixels", std::make_shared<ResizeMegapixelsKernel>());
    kernels.add("fugue/crop", std::make_shared<CropKernel>());
    kernels.add("fugue/brightness-contrast", std::make_shared<BrightnessContrastKernel>());
    kernels.add("fugue/blend", std::make_shared<BlendKernel>());
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
