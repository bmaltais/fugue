// Compare Images (#62, spec 14.1): the difference and composite outputs.
//
// Seam: the registered kernel, called the way the scheduler calls it. Images are built in memory
// as float RGB in 0..1 and compared after the node's own file round trip, so levels are exact
// 8-bit values (k / 255).

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
    float at(int x, int y, int ch) const { return d[(size_t(y) * w + x) * c + ch]; }
};

Img flat(int w, int h, float r, float g, float b, int c = 3, float alpha = 1) {
    Img img{w, h, c, {}};
    for (int i = 0; i < w * h; ++i) {
        img.d.push_back(r);
        img.d.push_back(g);
        img.d.push_back(b);
        if (c == 4)
            img.d.push_back(alpha);
    }
    return img;
}

// Left half one color, right half another.
Img halves(int w, int h, float left, float right) {
    Img img = flat(w, h, left, left, left);
    for (int y = 0; y < h; ++y)
        for (int x = w / 2; x < w; ++x)
            for (int ch = 0; ch < 3; ++ch)
                img.d[(size_t(y) * w + x) * 3 + ch] = right;
    return img;
}

MediaHandle handleOf(const Img &img) {
    const vips::VImage image = vips::VImage::new_from_memory_copy(img.d.data(), img.d.size() * sizeof(float), img.w,
                                                                  img.h, img.c, VIPS_FORMAT_FLOAT);
    const Result<MediaHandle> handle = MediaHandle::fromImage(image);
    return handle.ok() ? handle.value() : MediaHandle();
}

Img imgOf(const Value &value) {
    Img img;
    const Result<vips::VImage> image = value.media().image();
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

} // namespace

class TestCompareImages : public QObject {
    Q_OBJECT

    Kernels m_kernels;

    Values run(const Img &a, const Img &b, const QString &mode = "slider", double position = 0.5, double gain = 4) {
        const std::shared_ptr<NodeKernel> kernel = m_kernels.node("fugue/compare-images");
        if (!kernel)
            return {};
        NodeContext context;
        context.node = "n";
        context.inputs = {{"a", handleOf(a)}, {"b", handleOf(b)}, {"mode", mode}, {"position", position}, {"gain", gain}};
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        const Result<Values> result = kernel->run(context);
        if (!result.ok())
            qWarning("%s", qPrintable(result.errors().value(0).message));
        return result.ok() ? result.value() : Values();
    }

private slots:
    void initTestCase() {
        initVips();
        nodes::addPixelKernels(m_kernels);
    }

    void identicalImagesGiveABlackDifference() {
        const Img same = halves(8, 6, 0.2f, 0.9f);
        const Img difference = imgOf(run(same, same).value("difference"));
        QCOMPARE(difference.w, 8);
        QCOMPARE(difference.h, 6);
        for (const float v : difference.d)
            QCOMPARE(v, 0.0f);
    }

    void differenceIsTheAbsoluteDifferenceTimesTheGain() {
        const Img a = flat(2, 2, 0.2f, 0.8f, 0.5f);
        const Img b = flat(2, 2, 0.5f, 0.5f, 0.5f);
        const Img once = imgOf(run(a, b, "slider", 0.5, 1).value("difference"));
        QVERIFY(std::fabs(once.at(1, 1, 0) - 0.3f) < 1e-6f);
        QVERIFY(std::fabs(once.at(1, 1, 1) - 0.3f) < 1e-6f);
        QCOMPARE(once.at(1, 1, 2), 0.0f);
        const Img amplified = imgOf(run(a, b, "slider", 0.5, 2).value("difference"));
        QVERIFY(std::fabs(amplified.at(0, 0, 0) - 0.6f) < 1e-6f);
        const Img clamped = imgOf(run(a, b, "slider", 0.5, 8).value("difference")); // 2.4 held at 1
        QCOMPARE(clamped.at(0, 0, 0), 1.0f);
    }

    void differenceIsSymmetric() {
        const Img a = halves(6, 2, 0.1f, 0.7f), b = halves(6, 2, 0.6f, 0.3f);
        const Img ab = imgOf(run(a, b).value("difference")), ba = imgOf(run(b, a).value("difference"));
        QCOMPARE(ab.d, ba.d);
    }

    void sliderTakesAOnTheLeftAndBOnTheRight() {
        const Img a = flat(10, 4, 0.1f, 0.1f, 0.1f), b = flat(10, 4, 0.9f, 0.9f, 0.9f);
        const Img out = imgOf(run(a, b, "slider", 0.3).value("composite"));
        QCOMPARE(out.w, 10);
        for (int x = 0; x < 10; ++x)
            QVERIFY2(std::fabs(out.at(x, 2, 0) - (x < 3 ? 0.1f : 0.9f)) < 1e-6f, qPrintable(QString::number(x)));
    }

    void horizontalSliderTakesAOnTopAndBBelow() {
        const Img a = flat(4, 10, 0.1f, 0.1f, 0.1f), b = flat(4, 10, 0.9f, 0.9f, 0.9f);
        const Img out = imgOf(run(a, b, "slider_horizontal", 0.5).value("composite"));
        QVERIFY(std::fabs(out.at(1, 4, 0) - 0.1f) < 1e-6f);
        QVERIFY(std::fabs(out.at(1, 5, 0) - 0.9f) < 1e-6f);
    }

    void sliderAtTheEndsIsOneImage() {
        const Img a = flat(5, 5, 0.1f, 0.1f, 0.1f), b = flat(5, 5, 0.9f, 0.9f, 0.9f);
        QVERIFY(std::fabs(imgOf(run(a, b, "slider", 0).value("composite")).at(0, 0, 0) - 0.9f) < 1e-6f);
        QVERIFY(std::fabs(imgOf(run(a, b, "slider", 1).value("composite")).at(4, 4, 0) - 0.1f) < 1e-6f);
    }

    void sideBySideIsTwiceAsWide() {
        const Img a = flat(4, 3, 0.1f, 0.1f, 0.1f), b = flat(4, 3, 0.9f, 0.9f, 0.9f);
        const Img out = imgOf(run(a, b, "side_by_side").value("composite"));
        QCOMPARE(out.w, 8);
        QCOMPARE(out.h, 3);
        QVERIFY(std::fabs(out.at(3, 1, 0) - 0.1f) < 1e-6f);
        QVERIFY(std::fabs(out.at(4, 1, 0) - 0.9f) < 1e-6f);
    }

    void modeAOrBIsThatImage() {
        const Img a = flat(3, 3, 0.1f, 0.2f, 0.3f), b = flat(3, 3, 0.9f, 0.8f, 0.7f);
        QVERIFY(std::fabs(imgOf(run(a, b, "a").value("composite")).at(1, 1, 1) - 0.2f) < 1e-6f);
        QVERIFY(std::fabs(imgOf(run(a, b, "b").value("composite")).at(1, 1, 1) - 0.8f) < 1e-6f);
    }

    void differenceModeCompositeIsTheDifference() {
        const Values out = run(flat(3, 3, 0.1f, 0.1f, 0.1f), flat(3, 3, 0.2f, 0.2f, 0.2f), "difference", 0.5, 2);
        QCOMPARE(imgOf(out.value("composite")).d, imgOf(out.value("difference")).d);
    }

    void aSmallerBIsFittedToTheSizeOfA() {
        const Img a = flat(8, 8, 0.4f, 0.4f, 0.4f), b = flat(4, 4, 0.4f, 0.4f, 0.4f);
        const Values out = run(a, b);
        QCOMPARE(imgOf(out.value("difference")).w, 8);
        for (const float v : imgOf(out.value("difference")).d)
            QVERIFY(v < 1e-4f); // the same flat color, however it is resized
    }

    void alphaIsLeftOff() {
        const Img a = flat(2, 2, 0.5f, 0.5f, 0.5f, 4, 0.0f), b = flat(2, 2, 0.5f, 0.5f, 0.5f, 3);
        const Values out = run(a, b);
        QCOMPARE(imgOf(out.value("difference")).c, 3);
        QCOMPARE(imgOf(out.value("composite")).c, 3);
    }

    void offersEveryModeItComputes() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        const NodeType *type = registry.nodeType("fugue/compare-images");
        QVERIFY(type);
        QVERIFY(type->sink);
        const InputSpec *mode = type->input("mode");
        QVERIFY(mode && mode->choices);
        QCOMPARE(*mode->choices, QStringList({"slider", "slider_horizontal", "side_by_side", "a", "b", "difference"}));
    }
};

QTEST_GUILESS_MAIN(TestCompareImages)
#include "test_compare_images.moc"
