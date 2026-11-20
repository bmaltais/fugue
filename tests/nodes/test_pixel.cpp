// The pixel nodes (spec 14.1): resize, crop, brightness-contrast, blend, text-concat.
//
// Seam: the registered kernel of each node, called the way the scheduler calls it
// (NodeKernel::run with a NodeContext), plus one test that drives the real `fugue`
// binary. Images are built in memory as float RGB(A) in 0..1, as ComfyUI holds them.
//
// Parity: each test compares the kernel with a small reference written here in plain
// C++ over float buffers, per output pixel and without libvips, from the formulas in
// ComfyUI at commit 995456d0188ffb51032c28f41527cfa82d87d8a9 (master, 2026-11-19):
//   - Resize:  nodes.py ImageScale.upscale, comfy/utils.py common_upscale (center crop,
//              torch.nn.functional.interpolate nearest-exact, bilinear, bicubic, area;
//              lanczos through comfy/utils.py lanczos = PIL Image.resize LANCZOS).
//   - Crop:    comfy_extras/nodes_images.py ImageCrop.execute.
//   - Blend:   comfy_extras/nodes_post_processing.py Blend (ImageBlend), and
//              node_helpers.py image_alpha_fix. ComfyUI's "difference" is base - top,
//              clamped: Fugue calls that `subtract`, and its `difference` is |base - top|
//              (Photoshop, GIMP, CSS mix-blend-mode), checked against worked numbers.
//   - Text concat: comfy_extras/nodes_string.py StringConcatenate.
//   - Brightness and contrast: ComfyUI core has no such node. The reference is the
//     mid-gray-pivot form used by GIMP's legacy tool and the CSS/libvips idiom,
//     out = clamp((in - 0.5) * contrast + 0.5 + brightness). It is Fugue's own and is
//     checked against worked numbers, not against ComfyUI.
//
// Tolerances (absolute, on 0..1 floats): 0 for nearest, crop and text; 1e-5 for
// bilinear, bicubic, area, lanczos, blend and brightness-contrast (float32 rounding).
// PIL runs lanczos on uint8 (truncating), so ComfyUI's own lanczos differs from the
// float reference by up to about 2/255; the node does not reproduce that quantization.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <vector>

#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/kernels.h"

using namespace fugue;

namespace {

struct Img {
    int w = 0, h = 0, c = 0;
    std::vector<float> d;
    float &at(int x, int y, int ch) { return d[(size_t(y) * w + x) * c + ch]; }
    float at(int x, int y, int ch) const { return d[(size_t(y) * w + x) * c + ch]; }
};

// Values that are exact 8-bit levels, so a file round trip would not change them.
Img synthetic(int w, int h, int c, unsigned seed) {
    Img img{w, h, c, std::vector<float>(size_t(w) * h * c)};
    unsigned s = seed * 2654435761u + 12345u;
    for (float &v : img.d) {
        s = s * 1664525u + 1013904223u;
        v = float((s >> 24) & 0xff) / 255.0f;
    }
    return img;
}

MediaHandle handleOf(const Img &img) {
    const vips::VImage image = vips::VImage::new_from_memory_copy(img.d.data(), img.d.size() * sizeof(float), img.w,
                                                                  img.h, img.c, VIPS_FORMAT_FLOAT);
    const Result<MediaHandle> handle = MediaHandle::fromImage(image);
    return handle.ok() ? handle.value() : MediaHandle();
}

Img imgOf(const MediaHandle &handle) {
    Img img;
    const Result<vips::VImage> image = handle.image();
    if (!image.ok())
        return img;
    const vips::VImage f = image.value().cast(VIPS_FORMAT_FLOAT);
    img.w = f.width();
    img.h = f.height();
    img.c = f.bands();
    size_t size = 0;
    void *mem = f.write_to_memory(&size);
    img.d.assign(static_cast<float *>(mem), static_cast<float *>(mem) + size / sizeof(float));
    g_free(mem);
    return img;
}

// The first channel of an image, as a one-channel image.
Img gray(const Img &img) {
    Img out{img.w, img.h, 1, {}};
    for (size_t i = 0; i < img.d.size(); i += size_t(img.c))
        out.d.push_back(img.d[i]);
    return out;
}

float maxDiff(const Img &a, const Img &b) {
    if (a.w != b.w || a.h != b.h || a.c != b.c)
        return 1e9f;
    float m = 0;
    for (size_t i = 0; i < a.d.size(); ++i)
        m = std::max(m, std::fabs(a.d[i] - b.d[i]));
    return m;
}

// ---- ComfyUI reference: resampling, one output pixel at a time -----------------

struct Tap {
    int index;
    double weight;
};
using Taps = std::vector<Tap>;

int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

double cubic1(double x, double A) { return ((A + 2) * x - (A + 3)) * x * x + 1; }
double cubic2(double x, double A) { return ((A * x - 5 * A) * x + 8 * A) * x - 4 * A; }
double sinc(double x) { return x == 0 ? 1.0 : std::sin(M_PI * x) / (M_PI * x); }
double lanczos3(double x) { return (x > -3 && x < 3) ? sinc(x) * sinc(x / 3) : 0.0; }

// The taps of output index `o` on one axis: torch upsample_*2d (align_corners=False,
// no antialias), adaptive_avg_pool2d for area, PIL's precompute_coeffs for lanczos.
Taps tapsFor(const QString &method, int in, int out, int o) {
    const double scale = double(in) / out;
    Taps t;
    if (method == "nearest") {
        t.push_back({std::min(int(std::floor((o + 0.5) * scale)), in - 1), 1.0});
    } else if (method == "bilinear") {
        const double src = std::max(scale * (o + 0.5) - 0.5, 0.0);
        const int i0 = int(std::floor(src));
        const int i1 = i0 + (i0 < in - 1 ? 1 : 0);
        const double l = src - i0;
        t.push_back({i0, 1 - l});
        t.push_back({i1, l});
    } else if (method == "bicubic") {
        const double src = scale * (o + 0.5) - 0.5;
        const int i0 = int(std::floor(src));
        const double f = src - i0;
        const double w[4] = {cubic2(f + 1, -0.75), cubic1(f, -0.75), cubic1(1 - f, -0.75), cubic2(2 - f, -0.75)};
        for (int k = 0; k < 4; ++k)
            t.push_back({clampi(i0 - 1 + k, 0, in - 1), w[k]});
    } else if (method == "area") {
        const int start = int(std::floor(double(o) * in / out));
        const int end = int(std::ceil(double(o + 1) * in / out));
        for (int i = start; i < end; ++i)
            t.push_back({i, 1.0 / (end - start)});
    } else { // lanczos
        const double fs = std::max(scale, 1.0);
        const double support = 3.0 * fs;
        const double center = (o + 0.5) * scale;
        const int lo = std::max(int(center - support + 0.5), 0);
        const int hi = std::min(int(center + support + 0.5), in);
        double sum = 0;
        for (int i = lo; i < hi; ++i) {
            const double w = lanczos3((i - center + 0.5) / fs);
            t.push_back({i, w});
            sum += w;
        }
        for (Tap &tap : t)
            tap.weight /= sum;
    }
    return t;
}

Img resample(const Img &in, int W, int H, const QString &method, bool clamp = true) {
    Img src = in;
    const bool premultiply = method == "lanczos" && in.c == 4; // PIL resizes RGBA premultiplied
    if (premultiply)
        for (int y = 0; y < in.h; ++y)
            for (int x = 0; x < in.w; ++x)
                for (int ch = 0; ch < 3; ++ch)
                    src.at(x, y, ch) *= in.at(x, y, 3);
    Img out{W, H, in.c, std::vector<float>(size_t(W) * H * in.c)};
    for (int y = 0; y < H; ++y) {
        const Taps ty = tapsFor(method, in.h, H, y);
        for (int x = 0; x < W; ++x) {
            const Taps tx = tapsFor(method, in.w, W, x);
            for (int ch = 0; ch < in.c; ++ch) {
                double acc = 0;
                for (const Tap &a : ty)
                    for (const Tap &b : tx)
                        acc += a.weight * b.weight * src.at(b.index, a.index, ch);
                out.at(x, y, ch) = float(acc);
            }
            if (premultiply) {
                const float alpha = out.at(x, y, 3);
                for (int ch = 0; ch < 3; ++ch)
                    out.at(x, y, ch) = alpha > 0 ? out.at(x, y, ch) / alpha : 0;
            }
        }
    }
    if (clamp) // the Resize node clamps its output; ComfyUI leaves bicubic overshoot, and Blend sees it
        for (float &v : out.d)
            v = std::min(1.0f, std::max(0.0f, v));
    return out;
}

// common_upscale with crop="center": narrow to the target aspect (Python round is half-to-even).
Img centerCrop(const Img &in, int W, int H) {
    const double oldAspect = double(in.w) / in.h, newAspect = double(W) / H;
    int x = 0, y = 0;
    if (oldAspect > newAspect)
        x = int(std::nearbyint((in.w - in.w * (newAspect / oldAspect)) / 2));
    else if (oldAspect < newAspect)
        y = int(std::nearbyint((in.h - in.h * (oldAspect / newAspect)) / 2));
    Img out{in.w - 2 * x, in.h - 2 * y, in.c, {}};
    out.d.resize(size_t(out.w) * out.h * out.c);
    for (int j = 0; j < out.h; ++j)
        for (int i = 0; i < out.w; ++i)
            for (int ch = 0; ch < in.c; ++ch)
                out.at(i, j, ch) = in.at(i + x, j + y, ch);
    return out;
}

// ---- ComfyUI reference: blend ------------------------------------------------

float g(float x) { return x <= 0.25f ? ((16 * x - 12) * x + 4) * x : std::sqrt(x); }

float blendMode(const QString &mode, float a, float b) {
    if (mode == "normal")
        return b;
    if (mode == "multiply")
        return a * b;
    if (mode == "screen")
        return 1 - (1 - a) * (1 - b);
    if (mode == "overlay")
        return a <= 0.5f ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b);
    if (mode == "soft_light")
        return b <= 0.5f ? a - (1 - 2 * b) * a * (1 - a) : a + (2 * b - 1) * (g(a) - a);
    if (mode == "difference")
        return std::fabs(a - b);
    if (mode == "subtract")
        return a - b; // ComfyUI's "difference": signed; the clamp does the rest
    qFatal("no reference for blend mode %s", qPrintable(mode));
}

Img blendRef(const Img &base, Img top, const QString &mode, float opacity) {
    // image_alpha_fix: match the base's channel count (a missing alpha is 1).
    if (top.c > base.c || top.c < base.c) {
        Img fixed{top.w, top.h, base.c, std::vector<float>(size_t(top.w) * top.h * base.c, 1.0f)};
        for (int y = 0; y < top.h; ++y)
            for (int x = 0; x < top.w; ++x)
                for (int ch = 0; ch < std::min(top.c, base.c); ++ch)
                    fixed.at(x, y, ch) = top.at(x, y, ch);
        top = fixed;
    }
    if (top.w != base.w || top.h != base.h)
        top = resample(centerCrop(top, base.w, base.h), base.w, base.h, "bicubic", false);
    Img out = base;
    for (size_t i = 0; i < out.d.size(); ++i) {
        const float v = base.d[i] * (1 - opacity) + blendMode(mode, base.d[i], top.d[i]) * opacity;
        out.d[i] = std::min(1.0f, std::max(0.0f, v));
    }
    if (base.c == 4)
        for (int y = 0; y < base.h; ++y)
            for (int x = 0; x < base.w; ++x)
                out.at(x, y, 3) = base.at(x, y, 3);
    return out;
}

} // namespace

class TestPixel : public QObject {
    Q_OBJECT

    Kernels m_kernels;

    Result<MediaHandle> run(const QString &id, const Values &inputs) {
        const std::shared_ptr<NodeKernel> kernel = m_kernels.node(id);
        if (!kernel)
            return Result<MediaHandle>(Errors{makeError("internal", "no kernel " + id)});
        NodeContext context;
        context.node = "n";
        context.inputs = inputs;
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        const Result<Values> result = kernel->run(context);
        if (!result.ok())
            return Result<MediaHandle>(result.errors());
        return result.value().value("image").media();
    }

    Img resized(const Img &in, int W, int H, const QString &fit, const QString &method) {
        const Result<MediaHandle> r = run("fugue/resize", {{"image", handleOf(in)},
                                                           {"width", W},
                                                           {"height", H},
                                                           {"fit", fit},
                                                           {"method", method}});
        if (!r.ok())
            qWarning("%s", qPrintable(r.errors().value(0).message));
        return r.ok() ? imgOf(r.value()) : Img();
    }

private slots:
    void initTestCase() {
        initVips();
        nodes::addPixelKernels(m_kernels);
    }

    // ---- text concat ----

    void textConcatJoinsWithTheSeparator() {
        NodeContext context;
        context.inputs = {{"a", "foo"}, {"b", "bar"}, {"separator", ", "}};
        const Result<Values> r = m_kernels.node("fugue/text-concat")->run(context);
        QVERIFY(r.ok());
        QCOMPARE(r.value().value("text").text(), QString("foo, bar")); // delimiter.join((a, b))
        context.inputs = {{"a", "x"}, {"b", ""}, {"separator", ""}};
        QCOMPARE(m_kernels.node("fugue/text-concat")->run(context).value().value("text").text(), QString("x"));
    }

    // ---- resize: worked numbers, then parity ----

    void nearestPicksTheExactSourcePixels() {
        Img in{4, 1, 1, {0.0f, 0.25f, 0.5f, 0.75f}};
        const Img out = resized(in, 2, 1, "stretch", "nearest"); // floor((o + 0.5) * 2) = 1, 3
        QCOMPARE(out.c, 3); // a gray image comes back as RGB
        QCOMPARE(out.d, (std::vector<float>{0.25f, 0.25f, 0.25f, 0.75f, 0.75f, 0.75f}));
    }

    void bilinearUpsampleUsesHalfPixelCenters() {
        Img in{2, 1, 1, {0.0f, 1.0f}};
        const Img out = resized(in, 4, 1, "stretch", "bilinear"); // src = -.25 (clamped to 0), .25, .75, 1.25
        QVERIFY(maxDiff(gray(out), Img{4, 1, 1, {0.0f, 0.25f, 0.75f, 1.0f}}) < 1e-6f);
    }

    void areaAveragesTheWindow() {
        Img in{4, 1, 1, {0.0f, 0.25f, 0.5f, 0.75f}};
        const Img out = resized(in, 2, 1, "stretch", "area");
        QVERIFY(maxDiff(gray(out), Img{2, 1, 1, {0.125f, 0.625f}}) < 1e-6f);
    }

    void resizeMatchesComfyForEveryMethodAndSize_data() {
        QTest::addColumn<QString>("method");
        QTest::addColumn<int>("channels");
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::addColumn<double>("tolerance");
        const char *methods[] = {"nearest", "bilinear", "bicubic", "area", "lanczos"};
        for (const char *m : methods) {
            const double tol = QString(m) == "nearest" ? 0.0 : 1e-5;
            // shrink and grow, odd sizes, RGB and RGBA
            QTest::addRow("%s rgb 7x5 to 3x2", m) << m << 3 << 3 << 2 << tol;
            QTest::addRow("%s rgba 7x5 to 11x9", m) << m << 4 << 11 << 9 << tol;
            QTest::addRow("%s rgba 7x5 to 7x5", m) << m << 4 << 7 << 5 << tol;
            QTest::addRow("%s rgb 7x5 to 4x9", m) << m << 3 << 4 << 9 << tol;
        }
    }
    void resizeMatchesComfyForEveryMethodAndSize() {
        QFETCH(QString, method);
        QFETCH(int, channels);
        QFETCH(int, w);
        QFETCH(int, h);
        QFETCH(double, tolerance);
        const Img in = synthetic(7, 5, channels, 3);
        const Img out = resized(in, w, h, "stretch", method);
        const Img expected = resample(in, w, h, method);
        QVERIFY2(maxDiff(out, expected) <= float(tolerance), qPrintable(QString::number(maxDiff(out, expected))));
    }

    void zeroWidthOrHeightKeepsTheAspect() {
        const Img in = synthetic(8, 4, 3, 5);
        QCOMPARE(resized(in, 4, 0, "stretch", "bilinear").h, 2); // round(4 * 4 / 8)
        QCOMPARE(resized(in, 0, 8, "stretch", "bilinear").w, 16);
        const Img same = resized(in, 0, 0, "stretch", "bilinear"); // both zero: unchanged
        QCOMPARE(maxDiff(same, in), 0.0f);
        QCOMPARE(resized(synthetic(3, 100, 3, 1), 1, 0, "stretch", "area").h, 33); // round(100 / 3)
    }

    void coverCropsTheCenterLikeComfyThenResizes() {
        // 4x2 into 2x2: x = round((4 - 4 * 0.5) / 2) = 1, so columns 1 and 2 remain.
        const Img in = synthetic(4, 2, 3, 9);
        const Img out = resized(in, 2, 2, "cover", "nearest");
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                for (int ch = 0; ch < 3; ++ch)
                    QCOMPARE(out.at(x, y, ch), in.at(x + 1, y, ch));
        // An odd margin rounds half to even, as Python's round does: 7 into 4 wide at
        // height 4: (7 - 7 * 4/7) / 2 = 1.5 -> 2.
        const Img odd = synthetic(7, 4, 4, 2);
        QVERIFY(maxDiff(resized(odd, 3, 4, "cover", "bicubic"), resample(centerCrop(odd, 3, 4), 3, 4, "bicubic")) <
                1e-5f);
    }

    void containFitsInsideTheBoxKeepingTheAspect() {
        const Img wide = synthetic(4, 2, 3, 4);
        Img out = resized(wide, 2, 2, "contain", "bilinear");
        QCOMPARE(out.w, 2);
        QCOMPARE(out.h, 1);
        QVERIFY(maxDiff(out, resample(wide, 2, 1, "bilinear")) < 1e-5f);
        out = resized(synthetic(2, 4, 3, 4), 3, 8, "contain", "bilinear"); // scale 2 on height, 1.5 on width
        QCOMPARE(out.w, 3);
        QCOMPARE(out.h, 6);
        out = resized(wide, 8, 8, "contain", "bicubic"); // contain also enlarges
        QCOMPARE(out.w, 8);
        QCOMPARE(out.h, 4);
    }

    void resizeKeepsAlphaAsAChannel() {
        QCOMPARE(resized(synthetic(5, 5, 4, 1), 3, 3, "stretch", "bicubic").c, 4);
        QCOMPARE(resized(synthetic(5, 5, 3, 1), 3, 3, "stretch", "bicubic").c, 3);
    }

    // ---- crop ----

    void cropSlicesLikeComfyAndClampsAtTheEdges() {
        const Img in = synthetic(7, 5, 4, 8);
        auto crop = [&](int x, int y, int w, int h) {
            const Result<MediaHandle> r =
                run("fugue/crop", {{"image", handleOf(in)}, {"x", x}, {"y", y}, {"width", w}, {"height", h}});
            return r.ok() ? imgOf(r.value()) : Img();
        };
        Img out = crop(2, 1, 3, 2);
        QCOMPARE(out.w, 3);
        QCOMPARE(out.h, 2);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 3; ++x)
                for (int ch = 0; ch < 4; ++ch)
                    QCOMPARE(out.at(x, y, ch), in.at(x + 2, y + 1, ch));
        out = crop(5, 3, 10, 10); // python slicing stops at the edge
        QCOMPARE(out.w, 2);
        QCOMPARE(out.h, 2);
        out = crop(50, 50, 4, 4); // x = min(x, w - 1), y = min(y, h - 1)
        QCOMPARE(out.w, 1);
        QCOMPARE(out.h, 1);
        QCOMPARE(out.at(0, 0, 2), in.at(6, 4, 2));
    }

    // The scheduler refuses values outside an input's range; the kernels still never read
    // outside the image when handed one directly (defence in depth).
    void cropHeldInsideTheImageWhateverItIsGiven() {
        const Img in = synthetic(7, 5, 3, 9);
        const Result<MediaHandle> negative =
            run("fugue/crop", {{"image", handleOf(in)}, {"x", -100000}, {"y", -3}, {"width", 2}, {"height", 2}});
        QVERIFY(negative.ok());
        const Img corner = imgOf(negative.value());
        QCOMPARE(corner.w, 2);
        QCOMPARE(corner.h, 2);
        QCOMPARE(corner.at(1, 1, 0), in.at(1, 1, 0));
        const Result<MediaHandle> huge =
            run("fugue/crop", {{"image", handleOf(in)}, {"x", 1e300}, {"y", 0}, {"width", 1e300}, {"height", -1e300}});
        QVERIFY(huge.ok());
        QCOMPARE(imgOf(huge.value()).w, 1);
        QCOMPARE(imgOf(huge.value()).h, 1);
    }

    void resizeRefusesANegativeOrHugeSize() {
        const Img in = synthetic(4, 4, 3, 10);
        for (const double width : {-5.0, 1e300}) {
            const Result<MediaHandle> r = run("fugue/resize", {{"image", handleOf(in)}, {"width", width}, {"height", 4},
                                                               {"fit", "stretch"}, {"method", "nearest"}});
            QVERIFY(!r.ok());
            QCOMPARE(r.error().code, QString("value_out_of_range"));
            QCOMPARE(r.error().input, QString("width"));
        }
    }

    // ---- brightness and contrast ----

    void brightnessContrastPivotsOnMidGrayAndKeepsAlpha() {
        Img in{4, 1, 4, {0.5f, 0.25f, 1.0f, 0.3f, 0.0f, 0.5f, 0.75f, 1.0f, 0.5f, 0.5f, 0.5f, 0.2f, 0.1f, 0.9f, 0.5f, 0.0f}};
        const Result<MediaHandle> r = run("fugue/brightness-contrast",
                                          {{"image", handleOf(in)}, {"brightness", 0.1}, {"contrast", 2.0}});
        QVERIFY(r.ok());
        const Img out = imgOf(r.value());
        // (v - .5) * 2 + .5 + .1, clamped; alpha (4th channel) untouched
        const std::vector<float> expect{0.6f, 0.1f, 1.0f, 0.3f, 0.0f, 0.6f, 1.0f, 1.0f,
                                        0.6f, 0.6f, 0.6f, 0.2f, 0.0f, 1.0f, 0.6f, 0.0f};
        QVERIFY(maxDiff(out, Img{4, 1, 4, expect}) < 1e-6f);
    }

    void brightnessContrastDefaultsChangeNothing() {
        const Img in = synthetic(5, 3, 3, 6);
        const Result<MediaHandle> r =
            run("fugue/brightness-contrast", {{"image", handleOf(in)}, {"brightness", 0.0}, {"contrast", 1.0}});
        QVERIFY(r.ok());
        QVERIFY(maxDiff(imgOf(r.value()), in) < 1e-6f);
    }

    // ---- blend ----

    void blendMatchesComfyForEveryMode_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("baseChannels");
        QTest::addColumn<int>("topChannels");
        QTest::addColumn<double>("opacity");
        for (const char *m : {"normal", "multiply", "screen", "overlay", "soft_light", "difference", "subtract"}) {
            QTest::addRow("%s rgb rgb 0.7", m) << m << 3 << 3 << 0.7;
            QTest::addRow("%s rgba rgba 1", m) << m << 4 << 4 << 1.0;
            QTest::addRow("%s rgba rgb 0.4", m) << m << 4 << 3 << 0.4;
            QTest::addRow("%s rgb rgba 0.5", m) << m << 3 << 4 << 0.5;
        }
    }
    void blendMatchesComfyForEveryMode() {
        QFETCH(QString, mode);
        QFETCH(int, baseChannels);
        QFETCH(int, topChannels);
        QFETCH(double, opacity);
        const Img base = synthetic(7, 5, baseChannels, 11);
        const Img top = synthetic(7, 5, topChannels, 12);
        const Result<MediaHandle> r = run("fugue/blend", {{"base", handleOf(base)},
                                                          {"top", handleOf(top)},
                                                          {"mode", mode},
                                                          {"opacity", opacity}});
        QVERIFY(r.ok());
        const Img expected = blendRef(base, top, mode, float(opacity));
        QVERIFY2(maxDiff(imgOf(r.value()), expected) < 1e-5f, qPrintable(QString::number(maxDiff(imgOf(r.value()), expected))));
    }

    void blendResizesTheTopToTheBaseWithBicubicCenterCrop() {
        const Img base = synthetic(6, 4, 3, 21);
        const Img top = synthetic(9, 9, 3, 22);
        const Result<MediaHandle> r = run("fugue/blend", {{"base", handleOf(base)},
                                                          {"top", handleOf(top)},
                                                          {"mode", "screen"},
                                                          {"opacity", 0.6}});
        QVERIFY(r.ok());
        QVERIFY(maxDiff(imgOf(r.value()), blendRef(base, top, "screen", 0.6f)) < 1e-5f);
    }

    // Photoshop's, GIMP's and CSS's difference: the absolute value.
    void blendDifferenceIsTheAbsoluteDifference() {
        Img base{1, 1, 3, {0.2f, 0.8f, 0.5f}};
        Img top{1, 1, 3, {0.5f, 0.5f, 0.5f}};
        const Result<MediaHandle> r = run("fugue/blend", {{"base", handleOf(base)},
                                                          {"top", handleOf(top)},
                                                          {"mode", "difference"},
                                                          {"opacity", 1.0}});
        QVERIFY(r.ok());
        QVERIFY(maxDiff(imgOf(r.value()), Img{1, 1, 3, {0.3f, 0.3f, 0.0f}}) < 1e-6f);
    }

    // ComfyUI's "difference" under its true name.
    void blendSubtractIsComfysDifference() {
        Img base{1, 1, 3, {0.2f, 0.8f, 0.5f}};
        Img top{1, 1, 3, {0.5f, 0.5f, 0.5f}};
        const Result<MediaHandle> r = run("fugue/blend", {{"base", handleOf(base)},
                                                          {"top", handleOf(top)},
                                                          {"mode", "subtract"},
                                                          {"opacity", 1.0}});
        QVERIFY(r.ok());
        QVERIFY(maxDiff(imgOf(r.value()), Img{1, 1, 3, {0.0f, 0.3f, 0.0f}}) < 1e-6f); // base - top, clamped at 0
    }

    void blendOffersEveryModeItComputes() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        const InputSpec *mode = registry.nodeType("fugue/blend")->input("mode");
        QVERIFY(mode && mode->choices);
        QCOMPARE(*mode->choices,
                 QStringList({"normal", "multiply", "screen", "overlay", "soft_light", "difference", "subtract"}));
    }

    // ---- the real binary ----

    void realBinaryRunsTextConcatFromAWorkflow() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile flow(dir.filePath("flow.fugue"));
        QVERIFY(flow.open(QIODevice::WriteOnly));
        flow.write("{\"fugue\": 1, \"graph\": {\"nodes\": {\"c\": {\"type\": \"fugue/text-concat\", "
                   "\"version\": \"1.0.0\", \"inputs\": {\"a\": \"left\", \"b\": \"right\", \"separator\": \" + \"}}}}}");
        flow.close();
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("QT_QPA_PLATFORM");
        env.remove("DISPLAY");
        env.remove("WAYLAND_DISPLAY");
        env.insert("XDG_CACHE_HOME", dir.filePath("cache"));
        env.insert("XDG_STATE_HOME", dir.filePath("state"));
        env.insert("XDG_CONFIG_HOME", dir.filePath("config"));
        env.insert("HOME", dir.filePath("home")); // not the developer's Omarchy state (8.2)
        QProcess process;
        process.setProcessEnvironment(env);
        process.start(FUGUE_BINARY, {"run", dir.filePath("flow.fugue"), "--to", "c", "--wait", "--json"});
        QVERIFY(process.waitForFinished(30000));
        QCOMPARE(process.exitCode(), 0);
        QString text;
        QString status;
        for (const QByteArray &line : process.readAllStandardOutput().trimmed().split('\n')) {
            const QJsonObject event = QJsonDocument::fromJson(line).object();
            if (event.value("event").toString() == "node.done")
                text = event.value("outputs").toObject().value("text").toString();
            if (event.value("event").toString() == "run.done")
                status = event.value("status").toString();
        }
        QCOMPARE(status, QString("ok"));
        QCOMPARE(text, QString("left + right"));
    }
};

QTEST_GUILESS_MAIN(TestPixel)
#include "test_pixel.moc"
