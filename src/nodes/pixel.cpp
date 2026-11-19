#include "nodes/kernels.h"

// Ticket #15: resize, crop, brightness-contrast, blend, text-concat (spec 14.1).
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
#include <vector>

#include "engine/vips.h"

namespace fugue::nodes {

namespace {

struct Pixels {
    int w = 0, h = 0, c = 0; // c is 3 (RGB) or 4 (RGBA)
    std::vector<float> d;
    float &at(int x, int y, int ch) { return d[(size_t(y) * w + x) * c + ch]; }
    float at(int x, int y, int ch) const { return d[(size_t(y) * w + x) * c + ch]; }
};

Error failure(const QString &message) { return makeError("node_failed", message); }

Result<Pixels> pixelsOf(const Value &value, const QString &input) {
    if (!value.isMedia())
        return Result<Pixels>(Errors{makeError("missing_input", QStringLiteral("Input %1 has no image.").arg(input))});
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
        pixels.w = image.width();
        pixels.h = image.height();
        pixels.c = image.bands();
        const float *begin = static_cast<const float *>(memory);
        pixels.d.assign(begin, begin + size / sizeof(float));
        g_free(memory);
        if (divisor != 1.0)
            for (float &v : pixels.d)
                v = float(v / divisor);
        return pixels;
    } catch (const vips::VError &error) {
        return Result<Pixels>(Errors{failure(QString::fromUtf8(error.what()))});
    }
}

Result<Values> imageOutput(Pixels pixels) {
    for (float &v : pixels.d)
        v = std::min(1.0f, std::max(0.0f, v));
    try {
        initVips();
        const vips::VImage image = vips::VImage::new_from_memory_copy(
            pixels.d.data(), pixels.d.size() * sizeof(float), pixels.w, pixels.h, pixels.c, VIPS_FORMAT_FLOAT);
        const Result<MediaHandle> handle = MediaHandle::fromImage(image);
        if (!handle.ok())
            return Result<Values>(handle.errors());
        return Values{{"image", Value(handle.value())}};
    } catch (const vips::VError &error) {
        return Result<Values>(Errors{failure(QString::fromUtf8(error.what()))});
    }
}

int toInt(const Value &value) { return int(std::lround(value.number())); }

// ---- resampling: torch.nn.functional.interpolate and PIL, per axis -----------

struct Tap {
    int index;
    float weight;
};

double cubicNear(double x, double A) { return ((A + 2) * x - (A + 3)) * x * x + 1; }
double cubicFar(double x, double A) { return ((A * x - 5 * A) * x + 8 * A) * x - 4 * A; }
double sinc(double x) { return x == 0 ? 1.0 : std::sin(M_PI * x) / (M_PI * x); }
double lanczos3(double x) { return (x > -3 && x < 3) ? sinc(x) * sinc(x / 3) : 0.0; }

// The source pixels and weights of output index `o` along an axis of `in` pixels resized to `out`.
std::vector<Tap> tapsFor(const QString &method, int in, int out, int o) {
    const double scale = double(in) / out;
    std::vector<Tap> taps;
    if (method == "nearest") { // "nearest-exact"
        taps.push_back({std::min(int(std::floor((o + 0.5) * scale)), in - 1), 1.0f});
    } else if (method == "bilinear") { // align_corners=False
        const double src = std::max(scale * (o + 0.5) - 0.5, 0.0);
        const int i0 = int(std::floor(src));
        const int i1 = i0 + (i0 < in - 1 ? 1 : 0);
        const double l = src - i0;
        taps.push_back({i0, float(1 - l)});
        taps.push_back({i1, float(l)});
    } else if (method == "bicubic") { // A = -0.75, indices clamped at the edges
        const double src = scale * (o + 0.5) - 0.5;
        const int i0 = int(std::floor(src));
        const double f = src - i0;
        const double w[4] = {cubicFar(f + 1, -0.75), cubicNear(f, -0.75), cubicNear(1 - f, -0.75),
                             cubicFar(2 - f, -0.75)};
        for (int k = 0; k < 4; ++k)
            taps.push_back({std::clamp(i0 - 1 + k, 0, in - 1), float(w[k])});
    } else if (method == "area") { // adaptive_avg_pool2d
        const int start = int(std::floor(double(o) * in / out));
        const int end = int(std::ceil(double(o + 1) * in / out));
        for (int i = start; i < end; ++i)
            taps.push_back({i, 1.0f / float(end - start)});
    } else { // lanczos: PIL's LANCZOS (a = 3), support widened when shrinking
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
    }
    return taps;
}

Pixels resample(const Pixels &in, int width, int height, const QString &method) {
    Pixels src = in;
    // PIL resizes RGBA with premultiplied color, so lanczos does too.
    const bool premultiply = method == "lanczos" && in.c == 4;
    if (premultiply)
        for (int y = 0; y < in.h; ++y)
            for (int x = 0; x < in.w; ++x)
                for (int ch = 0; ch < 3; ++ch)
                    src.at(x, y, ch) *= in.at(x, y, 3);

    std::vector<std::vector<Tap>> columns(static_cast<size_t>(width));
    std::vector<std::vector<Tap>> rows(static_cast<size_t>(height));
    for (int x = 0; x < width; ++x)
        columns[size_t(x)] = tapsFor(method, in.w, width, x);
    for (int y = 0; y < height; ++y)
        rows[size_t(y)] = tapsFor(method, in.h, height, y);

    // Horizontal pass, then vertical: the filters are separable.
    Pixels mid{width, in.h, in.c, std::vector<float>(size_t(width) * in.h * in.c)};
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < width; ++x)
            for (int ch = 0; ch < in.c; ++ch) {
                float acc = 0;
                for (const Tap &t : columns[size_t(x)])
                    acc += t.weight * src.at(t.index, y, ch);
                mid.at(x, y, ch) = acc;
            }
    Pixels out{width, height, in.c, std::vector<float>(size_t(width) * height * in.c)};
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            for (int ch = 0; ch < in.c; ++ch) {
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
    const double oldAspect = double(in.w) / in.h;
    const double newAspect = double(width) / height;
    int x = 0, y = 0;
    if (oldAspect > newAspect)
        x = int(std::nearbyint((in.w - in.w * (newAspect / oldAspect)) / 2));
    else if (oldAspect < newAspect)
        y = int(std::nearbyint((in.h - in.h * (oldAspect / newAspect)) / 2));
    Pixels out{in.w - 2 * x, in.h - 2 * y, in.c, {}};
    out.d.resize(size_t(out.w) * out.h * out.c);
    for (int j = 0; j < out.h; ++j)
        for (int i = 0; i < out.w; ++i)
            for (int ch = 0; ch < in.c; ++ch)
                out.at(i, j, ch) = in.at(i + x, j + y, ch);
    return out;
}

// ---- kernels ---------------------------------------------------------------------

class ResizeKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Pixels> loaded = pixelsOf(context.input("image"), "image");
        if (!loaded.ok())
            return Result<Values>(loaded.errors());
        const Pixels &in = loaded.value();
        int width = toInt(context.input("width"));
        int height = toInt(context.input("height"));
        const QString fit = context.input("fit").text();
        const QString method = context.input("method").text();
        if (width == 0 && height == 0)
            return Values{{"image", context.input("image")}};
        if (width == 0)
            width = std::max(1, int(std::nearbyint(double(in.w) * height / in.h)));
        else if (height == 0)
            height = std::max(1, int(std::nearbyint(double(in.h) * width / in.w)));

        if (fit == "contain") {
            // Fit inside the box: the side that limits takes the box's size exactly.
            if (double(width) / in.w <= double(height) / in.h)
                height = std::min(height, std::max(1, int(std::nearbyint(double(in.h) * width / in.w))));
            else
                width = std::min(width, std::max(1, int(std::nearbyint(double(in.w) * height / in.h))));
            return imageOutput(resample(in, width, height, method));
        }
        if (fit == "cover")
            return imageOutput(resample(centerCrop(in, width, height), width, height, method));
        return imageOutput(resample(in, width, height, method));
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
        const int x = std::min(toInt(context.input("x")), in.w - 1);
        const int y = std::min(toInt(context.input("y")), in.h - 1);
        const int width = std::min(std::max(toInt(context.input("width")), 1), in.w - x);
        const int height = std::min(std::max(toInt(context.input("height")), 1), in.h - y);
        Pixels out{width, height, in.c, std::vector<float>(size_t(width) * height * in.c)};
        for (int j = 0; j < height; ++j)
            for (int i = 0; i < width; ++i)
                for (int ch = 0; ch < in.c; ++ch)
                    out.at(i, j, ch) = in.at(i + x, j + y, ch);
        return imageOutput(std::move(out));
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
        const size_t colors = pixels.c == 4 ? 3 : size_t(pixels.c);
        for (size_t i = 0; i < pixels.d.size(); ++i)
            if (i % size_t(pixels.c) < colors)
                pixels.d[i] = (pixels.d[i] - 0.5f) * contrast + 0.5f + brightness;
        return imageOutput(std::move(pixels));
    }
};

float softLightG(float x) { return x <= 0.25f ? ((16 * x - 12) * x + 4) * x : std::sqrt(x); }

class BlendKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        Result<Pixels> baseLoaded = pixelsOf(context.input("base"), "base");
        if (!baseLoaded.ok())
            return Result<Values>(baseLoaded.errors());
        Result<Pixels> topLoaded = pixelsOf(context.input("top"), "top");
        if (!topLoaded.ok())
            return Result<Values>(topLoaded.errors());
        const Pixels base = std::move(baseLoaded.value());
        Pixels top = std::move(topLoaded.value());
        const QString mode = context.input("mode").text();
        const float opacity = float(context.input("opacity").number());

        // node_helpers.image_alpha_fix: the top takes the base's channel count; a missing alpha is 1.
        if (top.c != base.c) {
            Pixels fixed{top.w, top.h, base.c, std::vector<float>(size_t(top.w) * top.h * base.c, 1.0f)};
            for (int y = 0; y < top.h; ++y)
                for (int x = 0; x < top.w; ++x)
                    for (int ch = 0; ch < std::min(top.c, base.c); ++ch)
                        fixed.at(x, y, ch) = top.at(x, y, ch);
            top = std::move(fixed);
        }
        if (top.w != base.w || top.h != base.h)
            top = resample(centerCrop(top, base.w, base.h), base.w, base.h, "bicubic");

        Pixels out = base;
        for (size_t i = 0; i < out.d.size(); ++i) {
            const float a = base.d[i], b = top.d[i];
            float mixed;
            if (mode == "multiply")
                mixed = a * b;
            else if (mode == "screen")
                mixed = 1 - (1 - a) * (1 - b);
            else if (mode == "overlay")
                mixed = a <= 0.5f ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b);
            else if (mode == "soft_light")
                mixed = b <= 0.5f ? a - (1 - 2 * b) * a * (1 - a) : a + (2 * b - 1) * (softLightG(a) - a);
            else if (mode == "difference")
                mixed = a - b; // ComfyUI: signed, then clamped (no absolute value)
            else
                mixed = b; // normal
            out.d[i] = a * (1 - opacity) + mixed * opacity;
        }
        if (base.c == 4) // alpha stores transparency, not color
            for (int y = 0; y < base.h; ++y)
                for (int x = 0; x < base.w; ++x)
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

} // namespace

void addPixelKernels(Kernels &kernels) {
    kernels.add("fugue/resize", std::make_shared<ResizeKernel>());
    kernels.add("fugue/crop", std::make_shared<CropKernel>());
    kernels.add("fugue/brightness-contrast", std::make_shared<BrightnessContrastKernel>());
    kernels.add("fugue/blend", std::make_shared<BlendKernel>());
    kernels.add("fugue/text-concat", std::make_shared<TextConcatKernel>());
}

} // namespace fugue::nodes
