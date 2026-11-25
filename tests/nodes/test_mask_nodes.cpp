// The mask and composite nodes (spec 14.4, ticket #171): Composite Masked, Feather Mask, Threshold
// Mask, Combine Masks, Fill Masked and Overlay Mask, and the `role: mask` of the mask ports.
//
// Seam: the registered kernel of each node, called as the scheduler calls it (NodeKernel::run with
// a NodeContext); the arithmetic under them (nodes/mask_ops.h) is called directly for the
// properties and the timing. Parity: golden/mask.json holds ComfyUI's own outputs (its real
// ImageCompositeMasked, ThresholdMask and MaskComposite, run by golden/make_mask.py), so the
// formulas are ComfyUI's, not a copy of ours. Tolerance 1e-5 on 0..1 floats (float32 order of
// operations). Feather Mask and Fill Masked have no ComfyUI node to run: their tests state
// properties. Nothing here reads or writes a file other than the golden data, so the tests do not
// depend on the test home.

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/kernels.h"
#include "nodes/mask_ops.h"

using namespace fugue;
using nodes::Pixels;

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

Img row(std::vector<float> values) { return Img{int(values.size()), 1, 1, std::move(values)}; }
Img filled(int w, int h, int c, float v) { return Img{w, h, c, std::vector<float>(size_t(w) * h * c, v)}; }

// Exact 8-bit levels from a seed, for fixed pseudo-random inputs.
std::vector<float> levels(size_t n, unsigned seed) {
    std::vector<float> out(n);
    unsigned s = seed * 2654435761u + 12345u;
    for (float &v : out) {
        s = s * 1664525u + 1013904223u;
        v = float((s >> 24) & 0xFF) / 255.0f;
    }
    return out;
}

Pixels randomMask(int w, int h, unsigned seed) {
    Pixels p(w, h, 1);
    p.data = levels(size_t(w) * h, seed);
    return p;
}

float sum(const std::vector<float> &v) {
    double s = 0;
    for (float x : v)
        s += x;
    return float(s);
}

} // namespace

class TestMaskNodes : public QObject {
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

    Img runMask(const QString &id, const Values &inputs, const char *output = "mask") {
        const Result<Values> r = runNode(id, inputs);
        if (!r.ok())
            qWarning("%s", qPrintable(r.errors().value(0).message));
        return r.ok() ? imgOf(r.value().value(output).media()) : Img();
    }

private slots:
    void initTestCase() { nodes::addBuiltinKernels(m_kernels); }

    // ---- parity with ComfyUI ----

    void compositeThresholdAndCombineMatchComfyUI_data() {
        QTest::addColumn<QJsonObject>("golden");
        QFile file(QFINDTESTDATA("golden/mask.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject doc = QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!doc.value("meta").toObject().value("comfyui_commit").toString().isEmpty());
        const QJsonArray cases = doc.value("cases").toArray();
        QVERIFY(cases.size() >= 40);
        for (const QJsonValue &c : cases)
            QTest::newRow(qPrintable(c.toObject().value("node").toString() + ": " + c.toObject().value("name").toString()))
                << c.toObject();
    }
    void compositeThresholdAndCombineMatchComfyUI() {
        QFETCH(QJsonObject, golden);
        const QString node = golden.value("node").toString();
        Values inputs;
        for (const char *name : {"destination", "source", "mask"})
            if (golden.contains(name))
                inputs.insert(name, Value(handleOf(imgFromJson(golden.value(name).toObject()))));
        const QJsonObject params = golden.value("inputs").toObject();
        for (auto it = params.begin(); it != params.end(); ++it)
            inputs.insert(it.key(), Value(it.value()));
        const Result<Values> result = runNode("fugue/" + node, inputs);
        QVERIFY2(result.ok(), qPrintable(result.errors().value(0).message));
        const Img got = imgOf(result.value().value(node == "composite-masked" ? "image" : "mask").media());
        const Img want = imgFromJson(golden.value("out").toObject());
        QVERIFY2(maxDiff(got, want) < 1e-5f, qPrintable(QString("max difference %1 (got %2x%3x%4, want %5x%6x%7)")
                                                              .arg(maxDiff(got, want)).arg(got.w).arg(got.h).arg(got.c)
                                                              .arg(want.w).arg(want.h).arg(want.c)));
    }

    // ---- Composite Masked: what ComfyUI's range does not reach ----

    void compositeCutsWhatSticksOutBeforeTheEdge() {
        const Img destination = filled(4, 3, 3, 0.0f);
        const Img source = filled(2, 2, 3, 1.0f);
        const Img out = runMask("fugue/composite-masked",
                                {{"destination", Value(handleOf(destination))}, {"source", Value(handleOf(source))},
                                 {"x", -1}, {"y", -1}}, "image");
        QCOMPARE(out.w, 4);
        QCOMPARE(out.h, 3);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 4; ++x)
                QCOMPARE(out.d[(size_t(y) * 4 + x) * 3], (x == 0 && y == 0) ? 1.0f : 0.0f);
    }

    void compositeWithASourceWhollyOutsideChangesNothing() {
        const Img destination{3, 1, 3, levels(9, 1)};
        for (const QPoint at : {QPoint(-5, 0), QPoint(3, 0), QPoint(0, 1), QPoint(0, -9), QPoint(20000, 20000)}) {
            const Img out = runMask("fugue/composite-masked",
                                    {{"destination", Value(handleOf(destination))},
                                     {"source", Value(handleOf(filled(2, 1, 3, 1.0f)))}, {"x", at.x()}, {"y", at.y()}},
                                    "image");
            QCOMPARE(out.d, destination.d);
        }
    }

    void compositeIsDestinationTimesOneMinusMaskPlusSourceTimesMask() {
        const Img destination = filled(2, 1, 3, 0.2f);
        const Img source = filled(2, 1, 3, 0.8f);
        const Img out = runMask("fugue/composite-masked",
                                {{"destination", Value(handleOf(destination))}, {"source", Value(handleOf(source))},
                                 {"mask", Value(handleOf(row({0.25f, 1.0f})))}, {"x", 0}, {"y", 0}}, "image");
        QVERIFY(maxDiff(out, Img{2, 1, 3, {0.35f, 0.35f, 0.35f, 0.8f, 0.8f, 0.8f}}) < 1e-6f);
    }

    void compositeGivesTheDestinationsChannelsAndAnOpaqueMissingAlpha() {
        const Img destination = filled(2, 1, 4, 0.0f);
        const Img rgb = filled(1, 1, 3, 0.5f);
        const Img withAlpha = runMask("fugue/composite-masked", {{"destination", Value(handleOf(destination))},
                                                                 {"source", Value(handleOf(rgb))}, {"x", 1}, {"y", 0}}, "image");
        QCOMPARE(withAlpha.c, 4);
        QCOMPARE(withAlpha.d, (std::vector<float>{0, 0, 0, 0, 0.5f, 0.5f, 0.5f, 1.0f}));
        const Img rgbOut = runMask("fugue/composite-masked", {{"destination", Value(handleOf(filled(2, 1, 3, 0.0f)))},
                                                              {"source", Value(handleOf(filled(1, 1, 4, 0.5f)))}, {"x", 0}, {"y", 0}}, "image");
        QCOMPARE(rgbOut.c, 3);
    }

    void compositeAcceptsAnRgbImageAsItsMask() {
        const Img rgbMask{1, 1, 3, {1.0f, 0.0f, 0.0f}}; // the first channel is the mask
        const Img out = runMask("fugue/composite-masked",
                                {{"destination", Value(handleOf(filled(1, 1, 3, 0.0f)))},
                                 {"source", Value(handleOf(filled(1, 1, 3, 1.0f)))}, {"mask", Value(handleOf(rgbMask))},
                                 {"x", 0}, {"y", 0}}, "image");
        QCOMPARE(out.d, (std::vector<float>{1, 1, 1}));
    }

    // ---- Threshold Mask and Combine Masks ----

    void thresholdIsOneOnlyAboveTheValue() {
        const Img out = runMask("fugue/threshold-mask", {{"mask", Value(handleOf(row({0.0f, 0.5f, 0.5001f, 1.0f})))}, {"value", 0.5}});
        QCOMPARE(out.d, (std::vector<float>{0, 0, 1, 1}));
    }

    void combineWithASourceBeforeTheEdgeUsesTheOverlap() {
        const Img destination = row({0.5f, 0.5f, 0.5f});
        const Img source = row({0.2f, 0.4f, 0.6f});
        // Source at x = -1: its second and third pixels land on the destination's first and second.
        const Img out = runMask("fugue/combine-masks", {{"destination", Value(handleOf(destination))},
                                                        {"source", Value(handleOf(source))},
                                                        {"operation", "add"}, {"x", -1}, {"y", 0}});
        QVERIFY(maxDiff(out, row({0.9f, 1.0f, 0.5f})) < 1e-6f);
    }

    void combineRoundsHalfToEvenForAndOrXor() {
        // torch.round: 0.5 is 0, 1.5 would be 2, so 0.5 counts as off and 0.51 as on.
        const Img d = row({0.5f, 0.51f, 0.5f, 0.51f});
        const Img s = row({0.5f, 0.5f, 0.51f, 0.51f});
        const std::pair<const char *, std::vector<float>> expect[] = {
            {"and", {0, 0, 0, 1}}, {"or", {0, 1, 1, 1}}, {"xor", {0, 1, 1, 0}}};
        for (const auto &[op, want] : expect)
            QCOMPARE(runMask("fugue/combine-masks", {{"destination", Value(handleOf(d))}, {"source", Value(handleOf(s))},
                                                     {"operation", op}, {"x", 0}, {"y", 0}}).d, want);
    }

    void combineHoldsTheResultTo01() {
        const Img out = runMask("fugue/combine-masks", {{"destination", Value(handleOf(row({0.8f, 0.2f})))},
                                                        {"source", Value(handleOf(row({0.8f, 0.5f})))},
                                                        {"operation", "subtract"}, {"x", 0}, {"y", 0}});
        QVERIFY(maxDiff(out, row({0.0f, 0.0f})) < 1e-6f);
        const Img sum = runMask("fugue/combine-masks", {{"destination", Value(handleOf(row({0.8f})))},
                                                        {"source", Value(handleOf(row({0.8f})))}, {"operation", "add"},
                                                        {"x", 0}, {"y", 0}});
        QCOMPARE(sum.d, (std::vector<float>{1.0f}));
    }

    void combineAndThresholdRefuseAnUnknownChoice() {
        const Result<Values> r = runNode("fugue/combine-masks", {{"destination", Value(handleOf(row({0.0f})))},
                                                                 {"source", Value(handleOf(row({0.0f})))},
                                                                 {"operation", "divide"}, {"x", 0}, {"y", 0}});
        QVERIFY(!r.ok());
        QCOMPARE(r.errors().value(0).code, QString("value_not_in_choices"));
        QCOMPARE(r.errors().value(0).input, QString("operation"));
    }

    // ---- Feather Mask ----

    void featherRadiusZeroIsTheIdentity() {
        const Pixels mask = randomMask(23, 17, 3);
        for (auto direction : {nodes::FeatherDirection::Outward, nodes::FeatherDirection::Inward, nodes::FeatherDirection::Centered})
            QCOMPARE(nodes::featherMask(mask, 0, direction).data, mask.data);
    }

    void featherOutwardNeverLowersAPixelAndInwardNeverRaisesOne_data() {
        QTest::addColumn<int>("radius");
        for (int r : {1, 2, 5, 16, 40, 300}) // 300 is wider than the mask: the box is longer than the picture
            QTest::newRow(qPrintable(QString::number(r))) << r;
    }
    void featherOutwardNeverLowersAPixelAndInwardNeverRaisesOne() {
        QFETCH(int, radius);
        const Pixels mask = randomMask(61, 47, 7);
        const Pixels outward = nodes::featherMask(mask, radius, nodes::FeatherDirection::Outward);
        const Pixels inward = nodes::featherMask(mask, radius, nodes::FeatherDirection::Inward);
        const Pixels centered = nodes::featherMask(mask, radius, nodes::FeatherDirection::Centered);
        for (size_t i = 0; i < mask.data.size(); ++i) {
            QVERIFY(outward.data[i] >= mask.data[i]);
            QVERIFY(inward.data[i] <= mask.data[i]);
            QVERIFY(outward.data[i] <= 1.0f + 1e-6f && inward.data[i] >= -1e-6f);
            QVERIFY(std::fabs(centered.data[i] - std::clamp(centered.data[i], 0.0f, 1.0f)) < 1e-6f);
            // Outward and inward are the two sides of the same blur.
            QVERIFY(std::fabs(outward.data[i] - std::max(mask.data[i], centered.data[i])) < 1e-7f);
            QVERIFY(std::fabs(inward.data[i] - std::min(mask.data[i], centered.data[i])) < 1e-7f);
        }
    }

    void featherCenteredIsSymmetricBetweenPaintAndNoPaint() {
        // Feathering the inverse is the inverse of feathering: the edge sits halfway, both ways.
        const Pixels mask = randomMask(40, 33, 9);
        Pixels inverse = mask;
        for (float &v : inverse.data)
            v = 1.0f - v;
        for (int radius : {3, 11, 25}) {
            const Pixels a = nodes::featherMask(mask, radius, nodes::FeatherDirection::Centered);
            const Pixels b = nodes::featherMask(inverse, radius, nodes::FeatherDirection::Centered);
            for (size_t i = 0; i < a.data.size(); ++i)
                QVERIFY2(std::fabs(a.data[i] - (1.0f - b.data[i])) < 1e-5f, qPrintable(QString::number(radius)));
        }
    }

    void featherOfAStraightEdgeCrossesHalfOnTheEdge() {
        Pixels mask(64, 8, 1, 0.0f);
        for (int y = 0; y < 8; ++y)
            for (int x = 32; x < 64; ++x)
                mask.at(x, y, 0) = 1.0f;
        const Pixels b = nodes::featherMask(mask, 8, nodes::FeatherDirection::Centered);
        // The step between pixels 31 and 32: the blurred values straddle 0.5 symmetrically.
        QVERIFY(std::fabs((b.at(31, 3, 0) + b.at(32, 3, 0)) - 1.0f) < 1e-5f);
        QVERIFY(b.at(31, 3, 0) < 0.5f && b.at(32, 3, 0) > 0.5f);
        QVERIFY(b.at(0, 3, 0) < 1e-6f && b.at(63, 3, 0) > 1.0f - 1e-6f); // far from the edge: untouched
    }

    void featherSpreadsAPointWithTheVarianceOfAGaussianOfHalfTheRadius() {
        for (int radius : {4, 12, 24}) {
            Pixels mask(201, 201, 1, 0.0f);
            mask.at(100, 100, 0) = 1.0f;
            const Pixels b = nodes::featherMask(mask, radius, nodes::FeatherDirection::Centered);
            double total = 0, moment = 0;
            for (int x = 0; x < 201; ++x) {
                const double v = b.at(x, 100, 0);
                total += v;
                moment += v * (x - 100) * (x - 100);
            }
            const double sigma = radius / 2.0;
            QVERIFY2(std::fabs(moment / total - sigma * sigma) < 0.08 * sigma * sigma + 0.1,
                     qPrintable(QString("radius %1: variance %2, wanted %3").arg(radius).arg(moment / total).arg(sigma * sigma)));
            QVERIFY(std::fabs(sum(b.data) - 1.0f) < 1e-3f); // the paint is moved, not lost
        }
    }

    void featherKeepsAMaskThatReachesTheEdgeFull() {
        const Pixels mask(30, 20, 1, 1.0f);
        for (auto direction : {nodes::FeatherDirection::Outward, nodes::FeatherDirection::Inward, nodes::FeatherDirection::Centered})
            for (float v : nodes::featherMask(mask, 12, direction).data)
                QVERIFY(std::fabs(v - 1.0f) < 1e-6f);
    }

    void featherOfA512PixelRadiusOnA4MegapixelMaskTakesWellUnderASecond() {
        Pixels mask(2048, 2048, 1, 0.0f);
        for (int y = 700; y < 1300; ++y)
            for (int x = 800; x < 1400; ++x)
                mask.at(x, y, 0) = 1.0f;
        QElapsedTimer timer;
        timer.start();
        const Pixels out = nodes::featherMask(mask, 512, nodes::FeatherDirection::Outward);
        const qint64 ms = timer.elapsed();
        QCOMPARE(out.width, 2048);
        QVERIFY(out.at(1100, 1000, 0) == 1.0f);
        QVERIFY(out.at(2000, 1000, 0) > 0.0f); // the edge reaches far
#ifdef NDEBUG
        constexpr qint64 limitMs = 1000;
#else
        constexpr qint64 limitMs = 10000; // a Debug build on a busy machine; the bound still catches a quadratic kernel
#endif
        QVERIFY2(ms < limitMs, qPrintable(QString("took %1 ms (limit %2)").arg(ms).arg(limitMs)));
        qInfo("feather 512 px on 4 MP: %lld ms", ms);
    }

    void featherNodeRunsTheKernelAndRefusesAnOutOfRangeRadius() {
        const Pixels mask = randomMask(20, 15, 4);
        Img in{20, 15, 1, mask.data};
        const Img out = runMask("fugue/feather-mask", {{"mask", Value(handleOf(in))}, {"radius", 6}, {"direction", "inward"}});
        QCOMPARE(out.c, 1);
        QCOMPARE(out.d, nodes::featherMask(mask, 6, nodes::FeatherDirection::Inward).data);
        const Result<Values> bad = runNode("fugue/feather-mask", {{"mask", Value(handleOf(in))}, {"radius", 513}, {"direction", "outward"}});
        QVERIFY(!bad.ok());
        QCOMPARE(bad.errors().value(0).input, QString("radius"));
        QVERIFY(!runNode("fugue/feather-mask", {{"mask", Value(handleOf(in))}, {"radius", -1}, {"direction", "outward"}}).ok());
    }

    // ---- Fill Masked ----

    // An image of fixed random colors; a mask that is 1 on a rectangle, 0.5 on a ring around it and
    // 0 elsewhere.
    static void fillFixture(Img &image, Img &mask) {
        image = Img{37, 29, 3, levels(37 * 29 * 3, 5)};
        mask = filled(37, 29, 1, 0.0f);
        for (int y = 8; y < 21; ++y)
            for (int x = 10; x < 26; ++x) {
                const bool inside = y >= 10 && y < 19 && x >= 12 && x < 24;
                mask.d[size_t(y) * 37 + x] = inside ? 1.0f : 0.5f;
            }
    }

    void fillMaskedLeavesPixelsWhereTheMaskIsZeroByteIdentical_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("gray") << "gray";
        QTest::newRow("blur") << "blur";
    }
    void fillMaskedLeavesPixelsWhereTheMaskIsZeroByteIdentical() {
        QFETCH(QString, mode);
        Img image, mask;
        fillFixture(image, mask);
        const Img out = runMask("fugue/fill-masked", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(mask))}, {"mode", mode}}, "image");
        QCOMPARE(out.w, image.w);
        QCOMPARE(out.c, 3);
        int kept = 0, changed = 0;
        for (size_t i = 0; i < mask.d.size(); ++i)
            for (int ch = 0; ch < 3; ++ch) {
                if (mask.d[i] == 0.0f) {
                    QCOMPARE(out.d[i * 3 + ch], image.d[i * 3 + ch]);
                    ++kept;
                } else if (out.d[i * 3 + ch] != image.d[i * 3 + ch]) {
                    ++changed;
                }
            }
        QVERIFY(kept > 500);
        QVERIFY(changed > 100);
    }

    // ---- Extend Edges (#243) ----

    void extendEdgesStretchesTheNearestPictureIntoAOneSidedHole() {
        // 6 x 1: red red red | hole hole hole (the far side is a hole open to the edge): the hole is red, not gray.
        const Img image{6, 1, 3, {1, 0, 0, 1, 0, 0, 1, 0, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f}};
        const Img mask = row({0, 0, 0, 1, 1, 1});
        const Img out = runMask("fugue/extend-edges", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(mask))}, {"mode", "edge"}, {"blur", 0}}, "image");
        for (int x = 0; x < 6; ++x) {
            QCOMPARE(out.d[size_t(x) * 3], 1.0f);
            QCOMPARE(out.d[size_t(x) * 3 + 1], 0.0f);
        }
    }

    void extendEdgesLeavesTheMaskZeroByteIdenticalAndMixesAPartialMask_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("edge") << "edge";
        QTest::newRow("edge-blur") << "edge-blur";
    }
    void extendEdgesLeavesTheMaskZeroByteIdenticalAndMixesAPartialMask() {
        QFETCH(QString, mode);
        Img image, mask;
        fillFixture(image, mask);
        const Img out = runMask("fugue/extend-edges", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(mask))}, {"mode", mode}, {"blur", 6}}, "image");
        QCOMPARE(out.w, image.w);
        int kept = 0, changed = 0;
        for (size_t i = 0; i < mask.d.size(); ++i)
            for (int ch = 0; ch < 3; ++ch) {
                if (mask.d[i] == 0.0f) {
                    QCOMPARE(out.d[i * 3 + ch], image.d[i * 3 + ch]);
                    ++kept;
                } else if (out.d[i * 3 + ch] != image.d[i * 3 + ch]) {
                    ++changed;
                }
            }
        QVERIFY(kept > 500);
        QVERIFY(changed > 100);
    }

    void extendEdgesOfAMaskedEverythingIsGrayAndBadInputsAreRefused() {
        const Img image{3, 1, 3, {1, 0, 0, 0, 1, 0, 0, 0, 1}};
        const Img out = runMask("fugue/extend-edges", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(filled(3, 1, 1, 1.0f)))}, {"mode", "edge-blur"}, {"blur", 24}}, "image");
        for (float v : out.d)
            QCOMPARE(v, 0.5f);
        QCOMPARE(runNode("fugue/extend-edges", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(row({0, 0, 1})))}, {"mode", "gray"}, {"blur", 1}}).errors().value(0).code,
                 QString("value_not_in_choices"));
        QCOMPARE(runNode("fugue/extend-edges", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(row({0, 0, 1})))}, {"mode", "edge"}, {"blur", 5000}}).errors().value(0).code,
                 QString("value_out_of_range"));
    }

    void fillMaskedBlurStaysWithinTheRangeOfTheColorsAroundIt() {
        // A binary mask: the filled pixels are averages of the kept ones, so no channel leaves
        // the range of what is kept.
        Img image, mask;
        fillFixture(image, mask);
        for (float &v : mask.d)
            v = v > 0.0f ? 1.0f : 0.0f;
        const Img out = runMask("fugue/fill-masked", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(mask))}, {"mode", "blur"}}, "image");
        for (int ch = 0; ch < 3; ++ch) {
            float low = 1, high = 0;
            for (size_t i = 0; i < mask.d.size(); ++i)
                if (mask.d[i] == 0.0f) {
                    low = std::min(low, image.d[i * 3 + ch]);
                    high = std::max(high, image.d[i * 3 + ch]);
                }
            for (size_t i = 0; i < mask.d.size(); ++i)
                if (mask.d[i] == 1.0f) {
                    QVERIFY(out.d[i * 3 + ch] >= low - 1e-5f);
                    QVERIFY(out.d[i * 3 + ch] <= high + 1e-5f);
                }
        }
    }

    void fillMaskedBlurOfAConstantSurroundingIsThatConstant() {
        Img image = filled(31, 23, 3, 0.8f);
        Img mask = filled(31, 23, 1, 0.0f);
        for (int y = 5; y < 15; ++y)
            for (int x = 7; x < 20; ++x)
                mask.d[size_t(y) * 31 + x] = 1.0f;
        for (int y = 5; y < 15; ++y)
            for (int x = 7; x < 20; ++x)
                for (int ch = 0; ch < 3; ++ch)
                    image.d[(size_t(y) * 31 + x) * 3 + ch] = 0.1f; // what is under the mask: gone
        const Img out = runMask("fugue/fill-masked", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(mask))}, {"mode", "blur"}}, "image");
        for (float v : out.d)
            QVERIFY2(std::fabs(v - 0.8f) < 1e-5f, qPrintable(QString::number(v)));
    }

    void fillMaskedBlurSpreadsTheNearerColorMore() {
        // Left half red, right half blue, a hole in the middle: the hole's left side is redder.
        Img image = filled(40, 8, 3, 0.0f);
        Img mask = filled(40, 8, 1, 0.0f);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 40; ++x) {
                image.d[(size_t(y) * 40 + x) * 3 + (x < 20 ? 0 : 2)] = 1.0f;
                if (x >= 16 && x < 24)
                    mask.d[size_t(y) * 40 + x] = 1.0f;
            }
        const Img out = runMask("fugue/fill-masked", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(mask))}, {"mode", "blur"}}, "image");
        const auto red = [&](int x) { return out.d[(size_t(4) * 40 + x) * 3]; };
        QVERIFY(red(16) > red(23));
        QVERIFY(red(16) > 0.5f && red(23) < 0.5f);
    }

    void fillMaskedGrayIsAHalfAndAMaskedEverythingIsGrayInBothModes() {
        const Img image = filled(6, 5, 3, 0.9f);
        const Img all = filled(6, 5, 1, 1.0f);
        for (const char *mode : {"gray", "blur"}) {
            const Img out = runMask("fugue/fill-masked", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(all))}, {"mode", mode}}, "image");
            for (float v : out.d)
                QCOMPARE(v, 0.5f);
        }
    }

    void fillMaskedMixesAPartialMaskWithTheFill() {
        const Img image{2, 1, 3, {0.2f, 0.2f, 0.2f, 0.2f, 0.2f, 0.2f}};
        const Img out = runMask("fugue/fill-masked", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(row({0.0f, 0.5f})))}, {"mode", "gray"}}, "image");
        QVERIFY(maxDiff(out, Img{2, 1, 3, {0.2f, 0.2f, 0.2f, 0.35f, 0.35f, 0.35f}}) < 1e-6f);
    }

    void fillMaskedStretchesAMaskOfAnotherSize() {
        const Img image = filled(8, 8, 3, 0.9f);
        const Img out = runMask("fugue/fill-masked", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(filled(2, 2, 1, 1.0f)))}, {"mode", "gray"}}, "image");
        QCOMPARE(out.w, 8);
        for (float v : out.d)
            QCOMPARE(v, 0.5f);
    }

    void fillMaskedOfAnRgbaImageKeepsFourChannels() {
        const Img image = filled(5, 5, 4, 0.7f);
        const Img out = runMask("fugue/fill-masked", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(filled(5, 5, 1, 1.0f)))}, {"mode", "gray"}}, "image");
        QCOMPARE(out.c, 4);
        QCOMPARE(out.d[3], 1.0f); // an opaque fill
    }

    void fillMaskedOfA4MegapixelPictureIsQuick() {
        Pixels image(2048, 2048, 3, 0.4f);
        Pixels mask(2048, 2048, 1, 0.0f);
        for (int y = 500; y < 1500; ++y)
            for (int x = 500; x < 1500; ++x)
                mask.at(x, y, 0) = 1.0f;
        QElapsedTimer timer;
        timer.start();
        const Pixels out = nodes::fillMasked(image, mask, nodes::FillMode::Blur);
        qInfo("fill masked (blur) on 4 MP: %lld ms", timer.elapsed());
        QVERIFY(timer.elapsed() < 3000);
        QVERIFY(std::fabs(out.at(1000, 1000, 1) - 0.4f) < 1e-5f);
    }

    // ---- Overlay Mask ----

    void overlayMixesTheColorByMaskTimesOpacity() {
        const Img image = filled(3, 1, 3, 0.2f);
        const Img out = runMask("fugue/overlay-mask", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(row({0.0f, 0.5f, 1.0f})))},
                                                       {"color", "#ff0000"}, {"opacity", 0.5}}, "image");
        const Img want{3, 1, 3, {0.2f, 0.2f, 0.2f, 0.2f * 0.75f + 0.25f, 0.2f * 0.75f, 0.2f * 0.75f, 0.6f, 0.1f, 0.1f}};
        QVERIFY(maxDiff(out, want) < 1e-6f);
    }

    void overlayLeavesAlphaAloneAndTakesShortAndAlphaColors() {
        const Img image{1, 1, 4, {0.0f, 0.0f, 0.0f, 0.3f}};
        const Img mask = row({1.0f});
        const Img shortHex = runMask("fugue/overlay-mask", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(mask))}, {"color", "#f00"}, {"opacity", 1.0}}, "image");
        QCOMPARE(shortHex.d, (std::vector<float>{1.0f, 0.0f, 0.0f, 0.3f}));
        const Img withAlpha = runMask("fugue/overlay-mask", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(mask))}, {"color", "#ff000080"}, {"opacity", 1.0}}, "image");
        QVERIFY(std::fabs(withAlpha.d[0] - 128.0f / 255.0f) < 1e-6f);
        QCOMPARE(withAlpha.d[3], 0.3f);
    }

    void overlayRefusesAColorThatIsNotHex() {
        const Img image = filled(1, 1, 3, 0.0f);
        for (const char *color : {"red", "#12", "#gggggg", ""}) {
            const Result<Values> r = runNode("fugue/overlay-mask", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(row({1.0f})))}, {"color", color}, {"opacity", 0.5}});
            QVERIFY2(!r.ok(), color);
            QCOMPARE(r.errors().value(0).input, QString("color"));
        }
    }

    // ---- Mask to Alpha ----

    void maskToAlphaPutsTheMaskInAlphaAndKeepsTheColors() {
        const Img image{3, 1, 3, {0.2f, 0.4f, 0.6f, 0.2f, 0.4f, 0.6f, 0.9f, 0.8f, 0.7f}};
        const Img out = runMask("fugue/mask-to-alpha", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(row({0.0f, 0.25f, 1.0f})))}}, "image");
        const Img want{3, 1, 4, {0.2f, 0.4f, 0.6f, 0.0f, 0.2f, 0.4f, 0.6f, 0.25f, 0.9f, 0.8f, 0.7f, 1.0f}};
        QVERIFY2(maxDiff(out, want) < 1e-6f, "colors are kept and alpha is the mask");
    }

    void maskToAlphaMultipliesAnAlphaTheImageHasAndStretchesTheMask() {
        const Img image{2, 1, 4, {0.1f, 0.1f, 0.1f, 0.5f, 0.2f, 0.2f, 0.2f, 0.0f}};
        const Img out = runMask("fugue/mask-to-alpha", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(filled(1, 1, 1, 0.5f)))}}, "image");
        QCOMPARE(out.c, 4);
        QVERIFY(std::fabs(out.d[3] - 0.25f) < 1e-6f);
        QCOMPARE(out.d[7], 0.0f);
    }

    // The reason for the node: Composite Masked onto an RGB or a black RGBA destination bleeds the
    // destination's color into the soft edge and cannot give an alpha of the mask.
    void compositeMaskedIsNotAWayToCutOut() {
        const Img image = filled(1, 1, 3, 0.8f);
        const Img viaComposite = runMask("fugue/composite-masked", {{"destination", Value(handleOf(filled(1, 1, 4, 0.0f)))},
                                                                      {"source", Value(handleOf(image))}, {"mask", Value(handleOf(row({0.5f})))}}, "image");
        QVERIFY(std::fabs(viaComposite.d[0] - 0.4f) < 1e-6f);  // the color is darkened by the edge, not 0.8
        const Img viaAlpha = runMask("fugue/mask-to-alpha", {{"image", Value(handleOf(image))}, {"mask", Value(handleOf(row({0.5f})))}}, "image");
        QVERIFY(std::fabs(viaAlpha.d[0] - 0.8f) < 1e-6f);
        QVERIFY(std::fabs(viaAlpha.d[3] - 0.5f) < 1e-6f);
    }

    // ---- the manifests ----

    void everyNodeIsInTheBuiltinPackWithItsKernelAndItsMaskPortsHaveTheRole() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        // type id -> the inputs and outputs that carry `role: mask`; every other image port has none.
        const std::pair<const char *, std::pair<QStringList, QStringList>> expect[] = {
            {"fugue/composite-masked", {{"mask"}, {}}},
            {"fugue/feather-mask", {{"mask"}, {"mask"}}},
            {"fugue/threshold-mask", {{"mask"}, {"mask"}}},
            {"fugue/combine-masks", {{"destination", "source"}, {"mask"}}},
            {"fugue/fill-masked", {{"mask"}, {}}},
            {"fugue/extend-edges", {{"mask"}, {}}},
            {"fugue/overlay-mask", {{"mask"}, {}}},
            {"fugue/grow-mask", {{"mask"}, {"mask"}}},
            {"fugue/invert-mask", {{"mask"}, {"mask"}}},
            {"fugue/image-to-mask", {{}, {"mask"}}},
            {"fugue/mask-to-alpha", {{"mask"}, {}}},
            {"fugue/solid-mask", {{}, {"mask"}}},
            {"fugue/mask-from-color", {{}, {"mask"}}},
            {"fugue/pad", {{"mask"}, {"mask"}}},
        };
        for (const auto &[id, ports] : expect) {
            const NodeType *type = registry.nodeType(QString::fromLatin1(id));
            QVERIFY2(type, id);
            QVERIFY2(m_kernels.node(QString::fromLatin1(id)), id);
            for (const InputSpec &input : type->inputs)
                QVERIFY2((input.role == "mask") == ports.first.contains(input.name), qPrintable(QString(id) + " input " + input.name));
            for (const OutputSpec &output : type->outputs)
                QVERIFY2((output.role == "mask") == ports.second.contains(output.name), qPrintable(QString(id) + " output " + output.name));
        }
        // #29's nodes took the role at a patch version.
        for (const char *id : {"fugue/grow-mask", "fugue/invert-mask", "fugue/image-to-mask", "fugue/solid-mask"})
            QCOMPARE(registry.nodeType(QString::fromLatin1(id))->version.toString(), QString("1.0.1"));
        QCOMPARE(registry.nodeType("fugue/pad")->version.toString(), QString("1.2.0")); // target size and keep mask added (#343)
    }

    void theNewNodesHaveTheDefaultsOfTheDesign() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        const auto input = [&](const char *node, const char *name) -> const InputSpec * {
            for (const InputSpec &i : registry.nodeType(QString("fugue/") + node)->inputs)
                if (i.name == name)
                    return &i;
            return nullptr;
        };
        QCOMPARE(input("feather-mask", "radius")->defaultValue.toInt(), 16);
        QCOMPARE(input("feather-mask", "radius")->max.value_or(0), 512.0);
        QCOMPARE(input("feather-mask", "direction")->defaultValue.toString(), QString("outward"));
        QCOMPARE(input("threshold-mask", "value")->defaultValue.toDouble(), 0.5);
        QCOMPARE(input("overlay-mask", "color")->defaultValue.toString(), QString("#ff3b30"));
        QCOMPARE(input("overlay-mask", "opacity")->defaultValue.toDouble(), 0.5);
        QCOMPARE(input("fill-masked", "mode")->defaultValue.toString(), QString("blur"));
        QCOMPARE(input("extend-edges", "mode")->defaultValue.toString(), QString("edge-blur"));
        QCOMPARE(input("extend-edges", "blur")->defaultValue.toInt(), 24);
        QVERIFY(input("extend-edges", "blur")->advanced);
        QCOMPARE(input("pad", "fill")->defaultValue.toString(), QString("edge-blur"));
        QCOMPARE(input("combine-masks", "operation")->defaultValue.toString(), QString("multiply"));
        QVERIFY(input("composite-masked", "mask")->optional);
        QVERIFY(input("composite-masked", "resize_source")->advanced);
    }
};

QTEST_GUILESS_MAIN(TestMaskNodes)
#include "test_mask_nodes.moc"
