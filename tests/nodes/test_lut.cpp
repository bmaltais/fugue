// Apply LUT (#548, spec 14.2): the .cube parser and the lookup (nodes/lut.h), and the registered
// kernel as the scheduler calls it. No ComfyUI golden: the .cube format is Adobe's published one, so
// the tests state worked numbers (identity tables, an inverting 1D table, a channel-swapping 3D one).

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <vector>

#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/kernels.h"
#include "nodes/lut.h"

using namespace fugue;
using nodes::Lut;
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

// Two pixels (RGB): (0.2, 0.5, 0.9) and (1, 0, 0.25).
Pixels twoPixels(int channels = 3) {
    Pixels p(2, 1, channels, 1.0f);
    const float v[6] = {0.2f, 0.5f, 0.9f, 1.0f, 0.0f, 0.25f};
    for (int x = 0; x < 2; ++x)
        for (int c = 0; c < 3; ++c)
            p.at(x, 0, c) = v[x * 3 + c];
    return p;
}

// The 2-entry 3D table of a swap of red and blue: entry (r, g, b) is (b, g, r).
const char *kSwap3d = "LUT_3D_SIZE 2\n"
                      "0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n"; // red fastest: (1,0,0) -> (0,0,1)

} // namespace

class TestLut : public QObject {
    Q_OBJECT

    Kernels m_kernels;

    Result<Values> run(const Pixels &image, const QString &path, double strength = 1.0) {
        NodeContext context;
        context.node = "n";
        context.inputs = Values{{"image", Value(handleOf(image))}, {"file", Value(path)}, {"strength", Value(strength)}};
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        return m_kernels.node("fugue/apply-lut")->run(context);
    }

private slots:
    void initTestCase() { nodes::addBuiltinKernels(m_kernels); }

    // #600 review: a header-only parse (the check) does not reserve the table the size asks for (192 MiB at 256^3).
    void aHeaderOnlyParseReservesNoTable() {
        const Result<Lut> lut = nodes::parseCube("LUT_3D_SIZE 256\n0 0 0\n", true);
        QVERIFY(lut.ok());
        QCOMPARE(lut.value().table.capacity(), size_t(0));
        QCOMPARE(lut.value().size, 256);
    }

    void a1dTableInvertsAndInterpolates() {
        const Result<Lut> lut = nodes::parseCube("TITLE \"invert\"\n# a comment\nLUT_1D_SIZE 2\n1 1 1\n0 0 0\n");
        QVERIFY(lut.ok());
        const Pixels out = nodes::applyLut(twoPixels(), lut.value(), 1.0f);
        QVERIFY(std::fabs(out.at(0, 0, 0) - 0.8f) < 1e-6f);
        QVERIFY(std::fabs(out.at(0, 0, 2) - 0.1f) < 1e-6f);
        QCOMPARE(out.at(1, 0, 0), 0.0f);
        QCOMPARE(out.at(1, 0, 1), 1.0f);
    }

    void a1dTableWithThreeEntriesIsPiecewise() {
        const Result<Lut> lut = nodes::parseCube("LUT_1D_SIZE 3\n0 0 0\n0.8 0.8 0.8\n1 1 1\n");
        QVERIFY(lut.ok());
        Pixels p(1, 1, 3, 0.25f); // halfway into the first segment: 0.4
        QVERIFY(std::fabs(nodes::applyLut(p, lut.value(), 1.0f).at(0, 0, 1) - 0.4f) < 1e-6f);
    }

    void a3dTableIsTrilinear() {
        const Result<Lut> lut = nodes::parseCube(kSwap3d);
        QVERIFY(lut.ok());
        const Pixels out = nodes::applyLut(twoPixels(), lut.value(), 1.0f);
        // The swap is linear in each channel, so the trilinear result is exact: red and blue trade places.
        for (int x = 0; x < 2; ++x) {
            QVERIFY(std::fabs(out.at(x, 0, 0) - twoPixels().at(x, 0, 2)) < 1e-6f);
            QVERIFY(std::fabs(out.at(x, 0, 1) - twoPixels().at(x, 0, 1)) < 1e-6f);
            QVERIFY(std::fabs(out.at(x, 0, 2) - twoPixels().at(x, 0, 0)) < 1e-6f);
        }
    }

    void theDomainIsHonouredAndInputsAreClamped() {
        const Result<Lut> lut = nodes::parseCube("DOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\nLUT_1D_SIZE 2\n0 0 0\n1 1 1\n");
        QVERIFY(lut.ok());
        Pixels p(1, 1, 3, 1.0f); // 1 of a 0..2 domain is halfway
        QVERIFY(std::fabs(nodes::applyLut(p, lut.value(), 1.0f).at(0, 0, 0) - 0.5f) < 1e-6f);
        // DaVinci Resolve writes the domain as LUT_3D_INPUT_RANGE min max.
        const Result<Lut> resolve = nodes::parseCube("LUT_1D_SIZE 2\nLUT_1D_INPUT_RANGE 0.0 2.0\n0 0 0\n1 1 1\n");
        QVERIFY(resolve.ok());
        QVERIFY(std::fabs(nodes::applyLut(p, resolve.value(), 1.0f).at(0, 0, 0) - 0.5f) < 1e-6f);
    }

    void strengthMixesAndAlphaIsKept() {
        const Result<Lut> lut = nodes::parseCube("LUT_1D_SIZE 2\n1 1 1\n0 0 0\n");
        const Pixels in = twoPixels(4);
        QCOMPARE(nodes::applyLut(in, lut.value(), 0.0f).data, in.data);
        const Pixels half = nodes::applyLut(in, lut.value(), 0.5f);
        QVERIFY(std::fabs(half.at(0, 0, 0) - 0.5f) < 1e-6f); // halfway between 0.2 and 0.8
        QCOMPARE(half.at(0, 0, 3), 1.0f);
    }

    void aMalformedFileNamesTheLine() {
        const struct { const char *text; const char *needle; } cases[] = {
            {"LUT_3D_SIZE 2\n0 0 0\n0 0 x\n", "line 3"},
            {"LUT_1D_SIZE 2\n0 0 0\n1 1 1\n1 1 1\n", "line 4"},
            {"BOGUS 1\nLUT_1D_SIZE 2\n", "line 1"},
            {"0 0 0\nLUT_1D_SIZE 2\n", "line 1"},
            {"LUT_1D_SIZE 1\n", "line 1"},
            {"LUT_1D_SIZE 3\n0 0 0\n1 1 1\n", "line 3"}, // short of rows: the last line read
            {"# nothing\n", "line 1"},
        };
        for (const auto &c : cases) {
            const Result<Lut> lut = nodes::parseCube(QString::fromLatin1(c.text));
            QVERIFY2(!lut.ok(), c.text);
            QVERIFY2(lut.errors().value(0).message.contains(QLatin1String(c.needle)), qPrintable(lut.errors().value(0).message));
        }
    }

    void theKernelReadsTheFileAndReportsBadOnes() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto write = [&](const QString &name, const char *text) -> QString {
            QFile f(dir.filePath(name));
            if (f.open(QIODevice::WriteOnly))
                f.write(text);
            return dir.filePath(name);
        };
        const QString good = write("swap.cube", kSwap3d);
        const Result<Values> r = run(twoPixels(), good);
        QVERIFY2(r.ok(), qPrintable(r.errors().value(0).message));
        const Pixels out = pixelsFrom(r.value().value("image").media());
        QVERIFY(std::fabs(out.at(0, 0, 0) - 0.9f) < 1e-5f);

        const Result<Values> bad = run(twoPixels(), write("bad.cube", "LUT_1D_SIZE 2\n0 0 0\n1 oops 1\n"));
        QVERIFY(!bad.ok());
        QVERIFY(bad.errors().value(0).message.contains("line 3"));
        QCOMPARE(bad.errors().value(0).input, QString("file"));
        QCOMPARE(run(twoPixels(), dir.filePath("none.cube")).errors().value(0).code, QString("file_not_found"));
        QCOMPARE(run(twoPixels(), good, 1.5).errors().value(0).code, QString("value_out_of_range"));
    }
};

QTEST_GUILESS_MAIN(TestLut)
#include "test_lut.moc"
