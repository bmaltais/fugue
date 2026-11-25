// Mask from Color (#550, spec 14.4): the registered kernel and the arithmetic under it (nodes/mask_ops.h).
// Worked numbers, no golden: ComfyUI_essentials' MaskFromColor+ takes a per-channel threshold, not a distance.

#include <QtTest>

#include <cmath>

#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/kernels.h"
#include "nodes/mask_ops.h"

using namespace fugue;
using nodes::Pixels;

namespace {

MediaHandle handleOf(const Pixels &p) {
    const vips::VImage image = vips::VImage::new_from_memory_copy(p.data.data(), p.data.size() * sizeof(float), p.width,
                                                                  p.height, p.channels, VIPS_FORMAT_FLOAT);
    const Result<MediaHandle> handle = MediaHandle::fromImage(image);
    return handle.ok() ? handle.value() : MediaHandle();
}

Pixels pixelsFrom(const MediaHandle &handle) {
    Pixels p;
    const Result<vips::VImage> image = handle.image();
    if (!image.ok())
        return p;
    const vips::VImage f = image.value().cast(VIPS_FORMAT_FLOAT);
    p = Pixels(f.width(), f.height(), f.bands());
    size_t size = 0;
    void *mem = f.write_to_memory(&size);
    p.data.assign(static_cast<float *>(mem), static_cast<float *>(mem) + size / sizeof(float));
    g_free(mem);
    return p;
}

// A 4 x 1 picture: pure green, a near green (0, 0.9, 0.05), red, and white.
Pixels strip() {
    Pixels p(4, 1, 3);
    const float v[12] = {0, 1, 0, 0, 0.9f, 0.05f, 1, 0, 0, 1, 1, 1};
    p.data.assign(v, v + 12);
    return p;
}

} // namespace

class TestMaskFromColor : public QObject {
    Q_OBJECT

    Kernels m_kernels;

    Result<Values> run(const QString &id, const Values &inputs) {
        NodeContext context;
        context.node = "n";
        context.inputs = inputs;
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        return m_kernels.node(id)->run(context);
    }

    Result<Values> fromColor(const Pixels &image, double tolerance, int soften = 0, const QString &color = "#00ff00") {
        return run("fugue/mask-from-color", {{"image", Value(handleOf(image))}, {"color", Value(color)},
                                             {"tolerance", Value(tolerance)}, {"soften", Value(soften)}});
    }

private slots:
    void initTestCase() { nodes::addBuiltinKernels(m_kernels); }

    void toleranceIsADistanceInRgb() {
        // 0 takes the exact color only; the near green is 0.1118 away (/ sqrt 3 = 0.0645).
        Pixels m = nodes::maskFromColor(strip(), {0, 1, 0}, 0.0f);
        QCOMPARE(m.data, (std::vector<float>{1, 0, 0, 0}));
        m = nodes::maskFromColor(strip(), {0, 1, 0}, 0.07f);
        QCOMPARE(m.data, (std::vector<float>{1, 1, 0, 0}));
        m = nodes::maskFromColor(strip(), {0, 1, 0}, 1.0f); // everything
        QCOMPARE(m.data, (std::vector<float>{1, 1, 1, 1}));
    }

    void theKernelGivesAOneChannelMaskTheMaskInputsTake() {
        const Result<Values> r = fromColor(strip(), 0.07);
        QVERIFY2(r.ok(), qPrintable(r.errors().value(0).message));
        const Pixels mask = pixelsFrom(r.value().value("mask").media());
        QCOMPARE(mask.channels, 1);
        QCOMPARE(mask.data, (std::vector<float>{1, 1, 0, 0}));
        // Another mask node takes it as it is, and the manifest marks the port.
        const Result<Values> inverted = run("fugue/invert-mask", {{"mask", r.value().value("mask")}});
        QVERIFY(inverted.ok());
        QCOMPARE(pixelsFrom(inverted.value().value("mask").media()).data, (std::vector<float>{0, 0, 1, 1}));
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        QCOMPARE(registry.nodeType("fugue/mask-from-color")->outputs.value(0).role, QString("mask"));
    }

    void softenGivesAnEdgeAndAlphaIsIgnored() {
        Pixels p(9, 1, 4, 1.0f);
        for (int x = 0; x < 9; ++x) { // green on the left, red on the right, alpha 0 on the left
            p.at(x, 0, 0) = x < 4 ? 0.0f : 1.0f;
            p.at(x, 0, 1) = x < 4 ? 1.0f : 0.0f;
            p.at(x, 0, 2) = 0.0f;
            p.at(x, 0, 3) = x < 4 ? 0.0f : 1.0f;
        }
        const Pixels hard = pixelsFrom(fromColor(p, 0.05).value().value("mask").media());
        QCOMPARE(hard.at(0, 0, 0), 1.0f);
        QCOMPARE(hard.at(8, 0, 0), 0.0f);
        const Pixels soft = pixelsFrom(fromColor(p, 0.05, 4).value().value("mask").media());
        QVERIFY(soft.at(4, 0, 0) > 0.05f && soft.at(4, 0, 0) < 0.95f);
    }

    void badInputsAreErrors() {
        QCOMPARE(fromColor(strip(), 1.5).errors().value(0).code, QString("value_out_of_range"));
        QCOMPARE(fromColor(strip(), 0.1, 0, "green").errors().value(0).code, QString("value_out_of_range"));
        QCOMPARE(fromColor(strip(), 0.1, 513).errors().value(0).code, QString("value_out_of_range"));
    }
};

QTEST_GUILESS_MAIN(TestMaskFromColor)
#include "test_mask_from_color.moc"
