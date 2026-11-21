// The second set of pixel nodes (spec 14.2, ticket #29): Pad, Rotate, Flip, Blur, Sharpen, Invert,
// Grow Mask, Invert Mask, Image to Mask, Solid Mask and Number Math.
//
// Seam: the registered kernel of each node, called as the scheduler calls it (NodeKernel::run
// with a NodeContext). Parity: golden/pixel_more.json holds ComfyUI's own outputs (its real
// ImagePadForOutpaint, ImageRotate, ImageFlip, Blur, Sharpen, ImageInvert and GrowMask run on
// torch and scipy, by golden/make_pixel_more.py), so the formulas are ComfyUI's, not a copy of
// ours. Tolerance 1e-5 on 0..1 floats (float32 summation order). Nodes with no ComfyUI core
// equivalent to run (Invert Mask, Image to Mask, Solid Mask, Number Math) use worked numbers.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
};

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

Img imgFromJson(const QJsonObject &o) {
    Img img{o.value("w").toInt(), o.value("h").toInt(), o.value("c").toInt(1), {}};
    for (const QJsonValue &v : o.value("d").toArray())
        img.d.push_back(float(v.toDouble()));
    return img;
}

float maxDiff(const Img &a, const Img &b) {
    if (a.w != b.w || a.h != b.h || a.c != b.c || a.d.size() != b.d.size())
        return 1e9f;
    float m = 0;
    for (size_t i = 0; i < a.d.size(); ++i)
        m = std::max(m, std::fabs(a.d[i] - b.d[i]));
    return m;
}

} // namespace

class TestPixelMore : public QObject {
    Q_OBJECT

    Kernels m_kernels;

    Result<Values> runNode(const QString &id, const Values &inputs) {
        const std::shared_ptr<NodeKernel> kernel = m_kernels.node(id);
        if (!kernel)
            return Result<Values>(Errors{makeError("internal", "no kernel " + id)});
        NodeContext context;
        context.node = "n";
        context.inputs = inputs;
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        return kernel->run(context);
    }

    // A mask-shaped worked example: the 3 x 1 mask 0, 0.25, 1.
    static Img row(std::vector<float> values) { return Img{int(values.size()), 1, 1, std::move(values)}; }

private slots:
    void initTestCase() { nodes::addBuiltinKernels(m_kernels); }

    void everyNodeMatchesComfyUI_data() {
        QTest::addColumn<QJsonObject>("golden");
        QFile file(QFINDTESTDATA("golden/pixel_more.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonArray cases = QJsonDocument::fromJson(file.readAll()).array();
        QVERIFY(cases.size() > 20);
        for (const QJsonValue &c : cases)
            QTest::newRow(qPrintable(c.toObject().value("name").toString())) << c.toObject();
    }
    void everyNodeMatchesComfyUI() {
        QFETCH(QJsonObject, golden);
        const QString node = golden.value("node").toString();
        const Img in = imgFromJson(golden.value("in").toObject());
        const QJsonObject params = golden.value("inputs").toObject();
        Values inputs{{node == "grow-mask" ? "mask" : "image", Value(handleOf(in))}};
        for (auto it = params.begin(); it != params.end(); ++it)
            inputs.insert(it.key(), Value(it.value()));
        const Result<Values> result = runNode("fugue/" + node, inputs);
        QVERIFY2(result.ok(), qPrintable(result.errors().value(0).message));
        const QString out = node == "grow-mask" ? "mask" : "image";
        const Img got = imgOf(result.value().value(out).media());
        const Img want = imgFromJson(golden.value("out").toObject());
        QVERIFY2(maxDiff(got, want) < 1e-5f, qPrintable(QString("max difference %1 (got %2x%3x%4, want %5x%6x%7)")
                                                              .arg(maxDiff(got, want)).arg(got.w).arg(got.h).arg(got.c)
                                                              .arg(want.w).arg(want.h).arg(want.c)));
        if (node == "pad") {
            const QJsonObject m = golden.value("mask").toObject();
            const Img gotMask = imgOf(result.value().value("mask").media());
            const Img wantMask{m.value("w").toInt(), m.value("h").toInt(), 1, imgFromJson(m).d};
            QVERIFY2(maxDiff(gotMask, wantMask) < 1e-5f, qPrintable(QString("mask difference %1").arg(maxDiff(gotMask, wantMask))));
        }
    }

    void padHasNoRoomToGrowPastTheLimit() {
        const Img in{4, 4, 3, std::vector<float>(48, 0.2f)};
        const Result<Values> r = runNode("fugue/pad", {{"image", Value(handleOf(in))}, {"left", 16384}, {"top", 0},
                                                       {"right", 0}, {"bottom", 0}, {"feathering", 0}});
        QVERIFY(!r.ok());
        QCOMPARE(r.errors().value(0).code, QString("value_out_of_range"));
    }

    void blurAndSharpenRefuseARadiusOutsideTheirRange() {
        const Img in{4, 4, 3, std::vector<float>(48, 0.2f)};
        for (const char *id : {"fugue/blur", "fugue/sharpen"}) {
            const Result<Values> r = runNode(id, {{"image", Value(handleOf(in))}, {"radius", 40}, {"sigma", 1.0}, {"amount", 1.0}});
            QVERIFY(!r.ok());
            QCOMPARE(r.errors().value(0).input, QString("radius"));
        }
    }

    // ---- masks ----

    void invertMaskIsOneMinus() {
        const Result<Values> r = runNode("fugue/invert-mask", {{"mask", Value(handleOf(row({0.0f, 0.25f, 1.0f})))}});
        QVERIFY(r.ok());
        const Img out = imgOf(r.value().value("mask").media());
        QCOMPARE(out.c, 1);
        QVERIFY(maxDiff(out, row({1.0f, 0.75f, 0.0f})) < 1e-6f);
    }

    void imageToMaskTakesTheChannel() {
        const Img in{2, 1, 4, {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f}};
        const std::pair<const char *, std::vector<float>> expect[] = {
            {"red", {0.1f, 0.5f}}, {"green", {0.2f, 0.6f}}, {"blue", {0.3f, 0.7f}}, {"alpha", {0.4f, 0.8f}}};
        for (const auto &[channel, values] : expect) {
            const Result<Values> r = runNode("fugue/image-to-mask", {{"image", Value(handleOf(in))}, {"channel", channel}});
            QVERIFY(r.ok());
            const Img out = imgOf(r.value().value("mask").media());
            QCOMPARE(out.c, 1);
            QVERIFY2(maxDiff(out, row(values)) < 1e-6f, channel);
        }
    }

    void imageToMaskOfAnImageWithoutAlphaIsOpaque() {
        const Img in{2, 1, 3, {0.1f, 0.2f, 0.3f, 0.5f, 0.6f, 0.7f}};
        const Result<Values> r = runNode("fugue/image-to-mask", {{"image", Value(handleOf(in))}, {"channel", "alpha"}});
        QVERIFY(r.ok());
        QVERIFY(maxDiff(imgOf(r.value().value("mask").media()), row({1.0f, 1.0f})) < 1e-6f);
    }

    void solidMaskIsOneValueAndRefusesZeroSize() {
        const Result<Values> r = runNode("fugue/solid-mask", {{"value", 0.25}, {"width", 3}, {"height", 2}});
        QVERIFY(r.ok());
        const Img out = imgOf(r.value().value("mask").media());
        QCOMPARE(out.w, 3);
        QCOMPARE(out.h, 2);
        QCOMPARE(out.c, 1);
        QVERIFY(maxDiff(out, Img{3, 2, 1, std::vector<float>(6, 0.25f)}) < 1e-6f);
        QVERIFY(!runNode("fugue/solid-mask", {{"value", 1.0}, {"width", 0}, {"height", 2}}).ok());
    }

    void maskNodesAcceptAnRgbImageAsItsFirstChannel() {
        const Img in{2, 1, 3, {0.2f, 0.9f, 0.9f, 0.6f, 0.1f, 0.1f}};
        const Result<Values> r = runNode("fugue/invert-mask", {{"mask", Value(handleOf(in))}});
        QVERIFY(r.ok());
        QVERIFY(maxDiff(imgOf(r.value().value("mask").media()), row({0.8f, 0.4f})) < 1e-6f);
    }

    // ---- Number Math ----

    void numberMathDoesPythonArithmetic_data() {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<double>("a");
        QTest::addColumn<double>("b");
        QTest::addColumn<double>("expect");
        QTest::newRow("add") << "add" << 1.5 << 2.0 << 3.5;
        QTest::newRow("subtract") << "subtract" << 1.5 << 2.0 << -0.5;
        QTest::newRow("multiply") << "multiply" << -3.0 << 2.5 << -7.5;
        QTest::newRow("divide") << "divide" << 7.0 << 2.0 << 3.5;
        QTest::newRow("modulo") << "modulo" << 7.0 << 3.0 << 1.0;
        QTest::newRow("modulo negative a (Python: -7 % 3 == 2)") << "modulo" << -7.0 << 3.0 << 2.0;
        QTest::newRow("modulo negative b (Python: 7 % -3 == -2)") << "modulo" << 7.0 << -3.0 << -2.0;
        QTest::newRow("modulo float") << "modulo" << 5.5 << 2.0 << 1.5;
        QTest::newRow("power") << "power" << 2.0 << 10.0 << 1024.0;
        QTest::newRow("min") << "min" << 2.0 << -1.0 << -1.0;
        QTest::newRow("max") << "max" << 2.0 << -1.0 << 2.0;
    }
    void numberMathDoesPythonArithmetic() {
        QFETCH(QString, operation);
        QFETCH(double, a);
        QFETCH(double, b);
        QFETCH(double, expect);
        const Result<Values> r = runNode("fugue/number-math", {{"a", a}, {"operation", operation}, {"b", b}});
        QVERIFY(r.ok());
        QCOMPARE(r.value().value("value").number(), expect);
    }

    void numberMathRefusesDivisionByZeroAndNonFiniteResults() {
        for (const char *op : {"divide", "modulo"}) {
            const Result<Values> r = runNode("fugue/number-math", {{"a", 1.0}, {"operation", op}, {"b", 0.0}});
            QVERIFY(!r.ok());
            QCOMPARE(r.errors().value(0).input, QString("b"));
        }
        QVERIFY(!runNode("fugue/number-math", {{"a", 1e15}, {"operation", "power"}, {"b", 1e15}}).ok());
        QVERIFY(!runNode("fugue/number-math", {{"a", -8.0}, {"operation", "power"}, {"b", 0.5}}).ok()); // NaN
    }

    // ---- registration ----

    void everyNodeIsInTheBuiltinPackWithItsKernel() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        for (const char *id : {"pad", "rotate", "flip", "blur", "sharpen", "invert", "grow-mask", "invert-mask",
                               "image-to-mask", "solid-mask", "number-math"}) {
            const QString type = QString("fugue/") + id;
            QVERIFY2(registry.nodeType(type), qPrintable(type));
            QVERIFY2(m_kernels.node(type), qPrintable(type));
        }
    }
};

QTEST_GUILESS_MAIN(TestPixelMore)
#include "test_pixel_more.moc"
