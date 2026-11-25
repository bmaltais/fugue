// Desaturate, Posterize and Pixelize (#551, spec 14.2): the functions (nodes/tone_ops.h) with worked numbers, and the
// registered kernels. No golden: ComfyUI_essentials' nodes are one-line torch formulas, and these are the ones the issue states.

#include <QtTest>

#include <cmath>

#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/kernels.h"
#include "nodes/tone_ops.h"

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

} // namespace

class TestToneOps : public QObject {
    Q_OBJECT

    Kernels m_kernels;

    Result<Values> run(const QString &id, const Pixels &image, const Values &more) {
        NodeContext context;
        context.node = "n";
        context.inputs = more;
        context.inputs.insert("image", Value(handleOf(image)));
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        return m_kernels.node(id)->run(context);
    }

private slots:
    void initTestCase() { nodes::addBuiltinKernels(m_kernels); }

    void desaturateMovesTowardRec709Luma() {
        Pixels p(1, 1, 4);
        p.data = {1.0f, 0.0f, 0.0f, 0.25f}; // red, alpha 0.25
        const Pixels gray = nodes::desaturate(p, 1.0f);
        for (int c = 0; c < 3; ++c)
            QVERIFY(std::fabs(gray.at(0, 0, c) - 0.2126f) < 1e-6f);
        QCOMPARE(gray.at(0, 0, 3), 0.25f);
        QCOMPARE(nodes::desaturate(p, 0.0f).data, p.data);
        QVERIFY(std::fabs(nodes::desaturate(p, 0.5f).at(0, 0, 0) - (1.0f + 0.2126f) / 2) < 1e-6f);
    }

    void posterizeSnapsToEvenLevels() {
        Pixels p(1, 1, 4);
        p.data = {0.0f, 0.3f, 0.8f, 0.4f};
        const Pixels two = nodes::posterize(p, 2);
        QCOMPARE(two.data, (std::vector<float>{0.0f, 0.0f, 1.0f, 0.4f})); // alpha untouched
        const Pixels four = nodes::posterize(p, 4); // steps of 1/3: 0.3 -> 1/3, 0.8 -> 1 (0.8 * 3 = 2.4 -> 2 -> 2/3)
        QVERIFY(std::fabs(four.at(0, 0, 1) - 1.0f / 3) < 1e-6f);
        QVERIFY(std::fabs(four.at(0, 0, 2) - 2.0f / 3) < 1e-6f);
        QCOMPARE(nodes::posterize(p, 256).at(0, 0, 0), 0.0f);
    }

    void pixelizeAveragesEachBlockAndKeepsTheSize() {
        Pixels p(3, 2, 3);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 3; ++x)
                for (int c = 0; c < 3; ++c)
                    p.at(x, y, c) = float(x + 3 * y) / 10.0f; // 0, .1, .2 / .3, .4, .5
        const Pixels out = nodes::pixelize(p, 2);
        QCOMPARE(out.width, 3);
        QCOMPARE(out.height, 2);
        QVERIFY(std::fabs(out.at(0, 0, 0) - 0.2f) < 1e-6f); // (0 + .1 + .3 + .4) / 4
        QVERIFY(std::fabs(out.at(1, 1, 2) - 0.2f) < 1e-6f);
        QVERIFY(std::fabs(out.at(2, 0, 1) - 0.35f) < 1e-6f); // the edge block is 1 x 2: (.2 + .5) / 2
        QCOMPARE(nodes::pixelize(p, 1).data, p.data);
        QCOMPARE(nodes::pixelize(p, 100).at(0, 0, 0), 0.25f); // one block: the mean of the picture
    }

    void theKernelsRunAndRejectBadValues() {
        Pixels p(2, 2, 3, 0.5f);
        QVERIFY(run("fugue/desaturate", p, {{"amount", 1.0}}).ok());
        QVERIFY(run("fugue/posterize", p, {{"levels", 4}}).ok());
        const Result<Values> pix = run("fugue/pixelize", p, {{"block_size", 2}});
        QVERIFY(pix.ok());
        QCOMPARE(pixelsFrom(pix.value().value("image").media()).width, 2);
        QCOMPARE(run("fugue/desaturate", p, {{"amount", 2.0}}).errors().value(0).code, QString("value_out_of_range"));
        QCOMPARE(run("fugue/posterize", p, {{"levels", 1}}).errors().value(0).code, QString("value_out_of_range"));
        QCOMPARE(run("fugue/pixelize", p, {{"block_size", 0}}).errors().value(0).code, QString("value_out_of_range"));
    }
};

QTEST_GUILESS_MAIN(TestToneOps)
#include "test_tone_ops.moc"
