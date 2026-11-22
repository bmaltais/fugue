// Load Image 1.1's `mask` output and Paint Mask (spec 14.4, ticket #174).
//
// Seam: the registered kernels, called as the scheduler calls them (NodeKernel::run with a
// NodeContext). The pictures and masks are written to a temporary folder of the test's own with
// libvips; nothing outside it is read or written, and no test depends on what an earlier run left.

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <vector>

#include "engine/file_hashes.h"
#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/kernels.h"

using namespace fugue;

namespace {

// Writes an 8-bit PNG of `bands` channels, each pixel from `at(x, y, band)` (0 to 255).
template <typename Fn>
QString writePng(const QDir &dir, const QString &name, int w, int h, int bands, Fn at) {
    std::vector<unsigned char> bytes(size_t(w) * h * bands);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int b = 0; b < bands; ++b)
                bytes[(size_t(y) * w + x) * bands + b] = static_cast<unsigned char>(at(x, y, b));
    initVips();
    vips::VImage image = vips::VImage::new_from_memory_copy(bytes.data(), bytes.size(), w, h, bands, VIPS_FORMAT_UCHAR);
    const QString path = dir.filePath(name);
    image.write_to_file(path.toUtf8().constData());
    return path;
}

// The mask output as one row of 0..255 values per pixel, plus its size.
struct Mask {
    int w = 0, h = 0;
    std::vector<int> v;
    int at(int x, int y) const { return v[size_t(y) * w + x]; }
};

Mask maskOf(const Value &value) {
    Mask m;
    const Result<vips::VImage> image = value.media().image();
    if (!image.ok())
        return m;
    // The output is 8-bit (a mask file) or float 0..1: read as 0..255 either way.
    const vips::VImage first = image.value().extract_band(0);
    const double scale = first.format() == VIPS_FORMAT_UCHAR ? 1.0 : 255.0;
    const vips::VImage levels = (first.cast(VIPS_FORMAT_DOUBLE) * scale + 0.5).floor().cast(VIPS_FORMAT_INT);
    m.w = levels.width();
    m.h = levels.height();
    size_t size = 0;
    void *memory = levels.write_to_memory(&size);
    const int *numbers = static_cast<int *>(memory);
    m.v.assign(numbers, numbers + size / sizeof(int));
    g_free(memory);
    return m;
}

} // namespace

class TestMaskInput : public QObject {
    Q_OBJECT

    Kernels m_kernels;
    QTemporaryDir m_dir;
    std::unique_ptr<FileHashes> m_hashes;

    QDir dir() const { return QDir(m_dir.path()); }

    Result<Values> run(const QString &type, const Values &inputs) {
        const std::shared_ptr<NodeKernel> kernel = m_kernels.node(type);
        if (!kernel)
            return Result<Values>(Errors{makeError("internal", "no kernel " + type)});
        NodeContext context;
        context.node = "n";
        context.inputs = inputs;
        context.workDir = m_dir.path();
        context.hashes = m_hashes.get();
        context.progress = [](double, const QString &, const QString &) {};
        context.cancelled = [] { return false; };
        return kernel->run(context);
    }

    Result<Values> loadImage(const QString &file, const QString &mask = {}) {
        Values inputs{{"file", Value(file)}};
        if (!mask.isEmpty())
            inputs.insert("mask", Value(mask));
        return run("fugue/load-image", inputs);
    }

    static bool hasWarning(const Result<Values> &result, const QString &code) {
        for (const Error &warning : result.warnings())
            if (warning.code == code)
                return true;
        return false;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        nodes::addBuiltinKernels(m_kernels);
        m_hashes = std::make_unique<FileHashes>(dir().filePath("hashes.json"));
    }

    // ---- Load Image's mask, the three cases ----

    void aPaintedMaskIsTheOutput() {
        const QString picture = writePng(dir(), "photo.png", 4, 3, 3, [](int, int, int) { return 90; });
        const QString painted = writePng(dir(), "photo-mask.png", 4, 3, 1, [](int x, int y, int) { return x == 1 && y == 2 ? 255 : x == 2 ? 128 : 0; });
        const Result<Values> result = loadImage(picture, painted);
        QVERIFY2(result.ok(), qPrintable(result.errors().value(0).message));
        const Mask mask = maskOf(result.value().value("mask"));
        QCOMPARE(mask.w, 4);
        QCOMPARE(mask.h, 3);
        QCOMPARE(mask.at(1, 2), 255);
        QCOMPARE(mask.at(2, 0), 128);
        QCOMPARE(mask.at(0, 0), 0);
        QVERIFY(!hasWarning(result, "mask_resized"));
        // The picture itself is the file as it was, with its size.
        QCOMPARE(result.value().value("width").number(), 4.0);
        QVERIFY(result.value().value("image").isMedia());
    }

    void aPaintedMaskWinsOverTheAlpha() {
        const QString picture = writePng(dir(), "alpha-wins.png", 2, 2, 4, [](int, int, int b) { return b == 3 ? 0 : 50; });
        const QString painted = writePng(dir(), "alpha-wins-mask.png", 2, 2, 1, [](int x, int, int) { return x == 0 ? 255 : 0; });
        const Mask mask = maskOf(loadImage(picture, painted).value().value("mask"));
        QCOMPARE(mask.at(0, 0), 255);
        QCOMPARE(mask.at(1, 0), 0);
    }

    void withoutAMaskTheTransparentPartIsOneMinusAlpha() {
        // The left column is transparent (alpha 0), the right opaque (255), one pixel half.
        const QString picture = writePng(dir(), "cutout.png", 2, 2, 4, [](int x, int y, int b) {
            return b < 3 ? 100 : x == 0 ? (y == 1 ? 51 : 0) : 255;
        });
        const Result<Values> result = loadImage(picture);
        QVERIFY2(result.ok(), qPrintable(result.errors().value(0).message));
        const Mask mask = maskOf(result.value().value("mask"));
        QCOMPARE(mask.w, 2);
        QCOMPARE(mask.at(0, 0), 255); // 1 - 0
        QCOMPARE(mask.at(1, 0), 0);   // 1 - 1
        QCOMPARE(mask.at(0, 1), 204); // 1 - 0.2
        QVERIFY(!hasWarning(result, "mask_resized"));
    }

    void aPictureWithoutAlphaAndNoMaskGivesAnEmptyMask() {
        const QString picture = writePng(dir(), "plain.png", 5, 2, 3, [](int, int, int) { return 200; });
        const Mask mask = maskOf(loadImage(picture).value().value("mask"));
        QCOMPARE(mask.w, 5);
        QCOMPARE(mask.h, 2);
        QCOMPARE(*std::max_element(mask.v.begin(), mask.v.end()), 0);
    }

    void anEmptyMaskLiteralCountsAsNone() {
        const QString picture = writePng(dir(), "plain-literal.png", 3, 3, 3, [](int, int, int) { return 10; });
        const Result<Values> result = run("fugue/load-image", {{"file", Value(picture)}, {"mask", Value(QString())}});
        QVERIFY2(result.ok(), qPrintable(result.errors().value(0).message));
        QCOMPARE(maskOf(result.value().value("mask")).w, 3);
    }

    // ---- a mask painted on another size ----

    void aMaskOfAnotherSizeIsScaledToThePictureWithAWarning() {
        const QString picture = writePng(dir(), "big.png", 8, 8, 3, [](int, int, int) { return 10; });
        const QString painted = writePng(dir(), "big-mask.png", 4, 4, 1, [](int x, int, int) { return x < 2 ? 255 : 0; });
        const Result<Values> result = loadImage(picture, painted);
        QVERIFY2(result.ok(), qPrintable(result.errors().value(0).message));
        const Mask mask = maskOf(result.value().value("mask"));
        QCOMPARE(mask.w, 8);
        QCOMPARE(mask.h, 8);
        QCOMPARE(mask.at(0, 4), 255);
        QCOMPARE(mask.at(7, 4), 0);
        QVERIFY(hasWarning(result, "mask_resized"));
    }

    void aMissingMaskFileIsAnErrorOnTheMaskInput() {
        const QString picture = writePng(dir(), "gone.png", 2, 2, 3, [](int, int, int) { return 1; });
        const Result<Values> result = loadImage(picture, dir().filePath("never-painted.png"));
        QVERIFY(!result.ok());
        QCOMPARE(result.errors().first().code, QStringLiteral("file_not_found"));
        QCOMPARE(result.errors().first().input, QStringLiteral("mask"));
    }

    // ---- Paint Mask ----

    void paintMaskGivesAnEmptyMaskAtThePicturesSizeWhenNothingIsPainted() {
        const QString picture = writePng(dir(), "paint-none.png", 6, 4, 3, [](int, int, int) { return 10; });
        const Result<Values> loaded = loadImage(picture);
        const Result<Values> result = run("fugue/paint-mask", {{"image", loaded.value().value("image")}});
        QVERIFY2(result.ok(), qPrintable(result.errors().value(0).message));
        const Mask mask = maskOf(result.value().value("mask"));
        QCOMPARE(mask.w, 6);
        QCOMPARE(mask.h, 4);
        QCOMPARE(*std::max_element(mask.v.begin(), mask.v.end()), 0);
        QVERIFY(!result.value().contains("image")); // one output, the mask
    }

    void paintMaskGivesWhatWasPainted() {
        const QString picture = writePng(dir(), "paint-some.png", 6, 4, 3, [](int, int, int) { return 10; });
        const QString painted = writePng(dir(), "paint-some-mask.png", 6, 4, 1, [](int x, int y, int) { return x == 5 && y == 0 ? 255 : 0; });
        const Value image = loadImage(picture).value().value("image");
        const Result<Values> result = run("fugue/paint-mask", {{"image", image}, {"mask", Value(painted)}});
        QVERIFY2(result.ok(), qPrintable(result.errors().value(0).message));
        const Mask mask = maskOf(result.value().value("mask"));
        QCOMPARE(mask.at(5, 0), 255);
        QCOMPARE(mask.at(4, 0), 0);
        QVERIFY(!hasWarning(result, "mask_resized"));
    }

    void paintMaskScalesAMaskOfAnotherSizeAndSaysSo() {
        const QString picture = writePng(dir(), "paint-resized.png", 4, 4, 3, [](int, int, int) { return 10; });
        const QString painted = writePng(dir(), "paint-resized-mask.png", 2, 2, 1, [](int, int, int) { return 255; });
        const Value image = loadImage(picture).value().value("image");
        const Result<Values> result = run("fugue/paint-mask", {{"image", image}, {"mask", Value(painted)}});
        QVERIFY2(result.ok(), qPrintable(result.errors().value(0).message));
        QCOMPARE(maskOf(result.value().value("mask")).w, 4);
        QVERIFY(hasWarning(result, "mask_resized"));
    }

    void paintMaskNeedsAPicture() {
        const Result<Values> result = run("fugue/paint-mask", {});
        QVERIFY(!result.ok());
        QCOMPARE(result.errors().first().code, QStringLiteral("missing_input"));
    }

    // ---- the manifests ----

    void theMaskPortsCarryTheRoleAndLoadImageStaysCompatible() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        const NodeType *load = registry.nodeType("fugue/load-image");
        QVERIFY(load);
        QCOMPARE(load->version.toString(), QStringLiteral("1.1.0"));
        QCOMPARE(load->output("mask")->role, QString(kMaskRole));
        QVERIFY(load->input("mask")->optional);
        QCOMPARE(load->input("mask")->widget(), QStringLiteral("mask"));
        QVERIFY(load->input("mask")->holdsInputFile());
        QVERIFY(load->input("file")->holdsInputFile());
        const NodeType *paint = registry.nodeType("fugue/paint-mask");
        QVERIFY(paint);
        QCOMPARE(paint->input("mask")->over, QStringLiteral("image"));
        QCOMPARE(paint->output("mask")->role, QString(kMaskRole));
    }
};

QTEST_GUILESS_MAIN(TestMaskInput)
#include "test_mask_input.moc"
