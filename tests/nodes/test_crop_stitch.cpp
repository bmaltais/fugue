// Crop to Mask, Stitch and the `fugue/stitch` handle (spec 14.4, ticket #176), and the group
// "Inpaint (crop and stitch)" the fugue pack ships.
//
// Seams: the arithmetic (nodes/crop_stitch_ops.h) called directly on float buffers for the box
// rule, the sizes and the reconstruction; the kernel of each node, run as the scheduler runs it
// (NodeKernel::run with a NodeContext, inputs that are files); the registry for the type and the
// group; and the shipped `fugue` binary running the group through the fake helper. Each test makes
// its own folder, so none depends on the test home or on an earlier run.
//
// There is no ComfyUI golden here: the size rule is Resize to Megapixels' (checked against ComfyUI
// in test_pixel_more) and the resampling and feathering are the shared kernels, both checked
// against ComfyUI or by properties in their own tests; what is new is the box and the stitch.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <vector>

#include <vips/vips8>

#include "../test_paths.h"
#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/group_library.h"
#include "engine/hash.h"
#include "engine/paths.h"
#include "engine/port_type.h"
#include "engine/media.h"
#include "engine/registry.h"
#include "engine/vips.h"
#include "nodes/builtin.h"
#include "nodes/crop_stitch_ops.h"
#include "nodes/kernels.h"
#include "nodes/pixel_io.h"

using namespace fugue;
using namespace fugue::nodes;

namespace {

// A smooth, textured picture: a gradient with a little per-pixel variation, so a resize is
// close to the original but a wrong place is easy to see.
Pixels picture(int width, int height, unsigned seed = 1, int channels = 3) {
    Pixels p(width, height, channels);
    unsigned s = seed * 2654435761u + 12345u;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < channels; ++c) {
                s = s * 1664525u + 1013904223u;
                p.at(x, y, c) = 0.2f + 0.5f * (float(x) / width * (c + 1) / channels + float(y) / height * 0.3f) +
                                0.02f * float((s >> 24) & 0xFF) / 255.0f;
            }
    return p;
}

Pixels rectMask(int width, int height, int x, int y, int w, int h, float value = 1.0f) {
    Pixels m(width, height, 1);
    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i)
            m.at(i, j, 0) = value;
    return m;
}

float maxDiff(const Pixels &a, const Pixels &b) {
    if (a.width != b.width || a.height != b.height || a.channels != b.channels)
        return 1e9f;
    float m = 0;
    for (size_t i = 0; i < a.data.size(); ++i)
        m = std::max(m, std::fabs(a.data[i] - b.data[i]));
    return m;
}

bool insideBox(const CropBox &box, int x, int y) {
    return x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
}

// A float picture as an 8-bit PNG file in `dir`, and its handle as the scheduler hands it over.
MediaHandle file(QTemporaryDir &dir, FileHashes &hashes, const QString &name, const Pixels &pixels) {
    initVips();
    std::vector<unsigned char> bytes(pixels.data.size());
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = (unsigned char)std::lround(std::clamp(pixels.data[i], 0.0f, 1.0f) * 255.0f);
    const vips::VImage image = vips::VImage::new_from_memory_copy(bytes.data(), bytes.size(), pixels.width, pixels.height,
                                                                  pixels.channels, VIPS_FORMAT_UCHAR);
    const QString path = dir.filePath(name);
    image.write_to_file(path.toUtf8().constData());
    const Result<MediaHandle> handle = MediaHandle::probe(MediaType::Image, path, hashes);
    if (!handle)
        qFatal("cannot probe %s", qPrintable(path));
    return handle.value();
}

// What the 8-bit file holds, as the nodes read it.
Pixels read(const MediaHandle &handle) {
    const Result<Pixels> p = pixelsOf(Value(handle), "image");
    if (!p)
        qFatal("cannot read %s", qPrintable(handle.path()));
    return p.value();
}

class Bench {
public:
    Bench() : hashes(dir.filePath("hashes.json")) {
        nodes::addBuiltinNodeTypes(registry);
        nodes::addBuiltinKernels(kernels);
        QDir().mkpath(workDir());
    }
    QString workDir() const { return dir.filePath("work"); }

    NodeContext context(const QString &id, Values inputs) {
        const NodeType &type = *registry.nodeType(id);
        for (const InputSpec &spec : type.inputs)
            if (!inputs.contains(spec.name) && !spec.defaultValue.isUndefined())
                inputs.insert(spec.name, Value(spec.defaultValue));
        NodeContext c;
        c.node = QStringLiteral("n");
        c.type = &type;
        c.inputs = inputs;
        c.workDir = workDir();
        c.progress = [](double, const QString &, const QString &) {};
        c.cancelled = [] { return false; };
        c.hashes = &hashes;
        c.registry = &registry;
        return c;
    }
    Result<Values> run(const QString &id, const Values &inputs) { return kernels.node(id)->run(context(id, inputs)); }

    MediaHandle save(const QString &name, const Pixels &p) { return file(dir, hashes, name, p); }

    QTemporaryDir dir;
    FileHashes hashes;
    Registry registry;
    Kernels kernels;
};

QByteArray readBytes(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TestCropStitch : public QObject {
    Q_OBJECT

private slots:
    // ---- the box rule ----

    void theBoundsHoldEveryPixelAboveZero() {
        QVERIFY(!maskBounds(Pixels(8, 8, 1)));
        Pixels one(40, 30, 1);
        one.at(5, 7, 0) = 1.0f / 255.0f; // the faintest paint still counts
        QCOMPARE(*maskBounds(one), (CropBox{5, 7, 1, 1}));
        Pixels two = rectMask(40, 30, 3, 4, 2, 2);
        two.at(30, 25, 0) = 0.5f;
        QCOMPARE(*maskBounds(two), (CropBox{3, 4, 28, 22}));
    }

    void thePaddedBoxIsGrownThenMovedInside_data() {
        QTest::addColumn<CropBox>("bounds");
        QTest::addColumn<int>("padding");
        QTest::addColumn<CropBox>("want");
        // A 200 x 100 picture.
        QTest::newRow("grown on every side") << CropBox{80, 40, 20, 10} << 10 << CropBox{70, 30, 40, 30};
        QTest::newRow("no padding is the bounds") << CropBox{80, 40, 20, 10} << 0 << CropBox{80, 40, 20, 10};
        QTest::newRow("moved right off the left edge") << CropBox{4, 40, 20, 10} << 10 << CropBox{0, 30, 40, 30};
        QTest::newRow("moved left off the right edge") << CropBox{176, 40, 20, 10} << 10 << CropBox{160, 30, 40, 30};
        QTest::newRow("moved down off the top") << CropBox{80, 2, 20, 10} << 10 << CropBox{70, 0, 40, 30};
        QTest::newRow("moved up off the bottom") << CropBox{80, 90, 10, 10} << 10 << CropBox{70, 70, 30, 30};
        QTest::newRow("a corner") << CropBox{0, 0, 5, 5} << 10 << CropBox{0, 0, 25, 25};
        QTest::newRow("a one pixel mask in a corner") << CropBox{0, 0, 1, 1} << 64 << CropBox{0, 0, 129, 100};
        QTest::newRow("a mask touching the right border") << CropBox{190, 50, 10, 10} << 5 << CropBox{180, 45, 20, 20};
        QTest::newRow("cut where the picture is smaller") << CropBox{20, 10, 160, 80} << 64 << CropBox{0, 0, 200, 100};
        QTest::newRow("a mask larger than half") << CropBox{10, 5, 150, 90} << 30 << CropBox{0, 0, 200, 100};
        QTest::newRow("the whole picture") << CropBox{0, 0, 200, 100} << 64 << CropBox{0, 0, 200, 100};
        QTest::newRow("a negative padding is none") << CropBox{80, 40, 20, 10} << -5 << CropBox{80, 40, 20, 10};
    }
    void thePaddedBoxIsGrownThenMovedInside() {
        QFETCH(CropBox, bounds);
        QFETCH(int, padding);
        QFETCH(CropBox, want);
        QCOMPARE(paddedBox(bounds, padding, 200, 100), want);
    }

    void aPaddedBoxAlwaysHoldsTheBoundsAndStaysInside() {
        unsigned s = 7;
        const auto next = [&](int n) {
            s = s * 1664525u + 1013904223u;
            return int((s >> 8) % unsigned(n));
        };
        for (int i = 0; i < 2000; ++i) {
            const int W = 1 + next(300), H = 1 + next(300);
            const int x = next(W), y = next(H);
            const CropBox bounds{x, y, 1 + next(W - x), 1 + next(H - y)};
            const int padding = next(200);
            const CropBox box = paddedBox(bounds, padding, W, H);
            QVERIFY(box.x >= 0 && box.y >= 0 && box.x + box.width <= W && box.y + box.height <= H);
            QVERIFY(box.x <= bounds.x && box.y <= bounds.y);
            QVERIFY(box.x + box.width >= bounds.x + bounds.width && box.y + box.height >= bounds.y + bounds.height);
            QCOMPARE(box.width, std::min(W, bounds.width + 2 * padding));
            QCOMPARE(box.height, std::min(H, bounds.height + 2 * padding));
        }
    }

    // ---- sizes ----

    void theCropSizeIsResizeToMegapixelsSize_data() {
        QTest::addColumn<CropBox>("box");
        QTest::addColumn<double>("megapixels");
        QTest::addColumn<int>("multiple");
        QTest::addColumn<CropSize>("want");
        QTest::newRow("a square at 1 MP") << CropBox{0, 0, 300, 300} << 1.0 << 16 << CropSize{1024, 1024};
        QTest::newRow("a small square is enlarged") << CropBox{0, 0, 64, 64} << 1.0 << 16 << CropSize{1024, 1024};
        QTest::newRow("a huge square is reduced") << CropBox{0, 0, 6000, 6000} << 1.0 << 16 << CropSize{1024, 1024};
        QTest::newRow("3:2") << CropBox{0, 0, 600, 400} << 1.0 << 16 << CropSize{1248, 848};
        QTest::newRow("multiples of 8") << CropBox{0, 0, 600, 400} << 1.0 << 8 << CropSize{1256, 832};
        QTest::newRow("half a megapixel") << CropBox{0, 0, 100, 100} << 0.25 << 16 << CropSize{512, 512};
        QTest::newRow("0 keeps the size") << CropBox{0, 0, 333, 211} << 0.0 << 16 << CropSize{333, 211};
        QTest::newRow("a sliver keeps one step") << CropBox{0, 0, 1, 4000} << 1.0 << 16 << CropSize{16, 4096};
    }
    void theCropSizeIsResizeToMegapixelsSize() {
        QFETCH(CropBox, box);
        QFETCH(double, megapixels);
        QFETCH(int, multiple);
        QFETCH(CropSize, want);
        QCOMPARE(cropSizeFor(box, megapixels, multiple), want);
    }

    void theCropHasTheSizeOfItsBoxResized() {
        const Pixels image = picture(300, 200);
        const Pixels mask = rectMask(300, 200, 100, 80, 40, 30);
        const std::optional<Cropped> crop = cropToMask(image, mask, 20, 1.0, 16);
        QVERIFY(crop);
        QCOMPARE(crop->box, (CropBox{80, 60, 80, 70}));
        QCOMPARE(crop->size, cropSizeFor(crop->box, 1.0, 16));
        QCOMPARE(crop->image.width, crop->size.width);
        QCOMPARE(crop->image.height, crop->size.height);
        QCOMPARE(crop->image.channels, 3);
        QCOMPARE(crop->mask.width, crop->size.width);
        QCOMPARE(crop->mask.channels, 1);
        for (float v : crop->mask.data)
            QVERIFY(v >= 0.0f && v <= 1.0f);
        QVERIFY(!cropToMask(image, Pixels(300, 200, 1), 20, 1.0, 16)); // nothing painted
    }

    // ---- crop then stitch ----

    void anUntouchedCropStitchesBackTheOriginal_data() {
        QTest::addColumn<int>("blend");
        QTest::addColumn<double>("megapixels");
        QTest::newRow("same size, no blend") << 0 << 0.0;
        QTest::newRow("same size, blend 16") << 16 << 0.0;
        QTest::newRow("resized, no blend") << 0 << 0.25;
        QTest::newRow("resized, blend 16") << 16 << 0.25;
        QTest::newRow("1 MP, blend 16") << 16 << 1.0;
    }
    void anUntouchedCropStitchesBackTheOriginal() {
        QFETCH(int, blend);
        QFETCH(double, megapixels);
        const Pixels image = picture(300, 200);
        const Pixels mask = rectMask(300, 200, 100, 80, 40, 30);
        const std::optional<Cropped> crop = cropToMask(image, mask, 64, megapixels, 16);
        QVERIFY(crop);
        const Pixels out = stitch(image, mask, crop->box, crop->image, blend);
        const Pixels reach = featherMask(mask, blend, FeatherDirection::Outward);
        float inside = 0;
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x)
                for (int c = 0; c < 3; ++c) {
                    const float got = out.at(x, y, c), was = image.at(x, y, c);
                    if (reach.at(x, y, 0) == 0.0f)
                        QCOMPARE(got, was); // byte for byte where nothing of the crop is laid
                    else
                        inside = std::max(inside, std::fabs(got - was));
                }
        // Same size: the arithmetic only. Resized: the resampling of a smooth picture.
        QVERIFY2(inside < (megapixels == 0.0 ? 1e-6f : 0.03f), qPrintable(QString::number(inside)));
    }

    void withNoBlendAndNoResizeTheOriginalComesBackExactly() {
        const Pixels image = picture(120, 90, 3, 4);
        const Pixels mask = rectMask(120, 90, 30, 20, 25, 25);
        const std::optional<Cropped> crop = cropToMask(image, mask, 10, 0.0, 16);
        QVERIFY(crop);
        QCOMPARE(maxDiff(stitch(image, mask, crop->box, crop->image, 0), image), 0.0f);
    }

    void outsideTheBoxIsAlwaysTheOriginalWhateverTheCropHolds() {
        const Pixels image = picture(300, 200);
        // A soft, wide mask and a blend wider than the padding: the feather must stop at the box.
        Pixels mask = rectMask(300, 200, 100, 80, 40, 30);
        mask.at(101, 81, 0) = 0.4f;
        const std::optional<Cropped> crop = cropToMask(image, mask, 4, 1.0, 16);
        QVERIFY(crop);
        const Pixels redrawn = picture(crop->size.width, crop->size.height, 99); // a crop that is not the picture's
        for (int blend : {0, 1, 16, 60, 512}) {
            const Pixels out = stitch(image, mask, crop->box, redrawn, blend);
            for (int y = 0; y < image.height; ++y)
                for (int x = 0; x < image.width; ++x)
                    if (!insideBox(crop->box, x, y))
                        for (int c = 0; c < 3; ++c)
                            QCOMPARE(out.at(x, y, c), image.at(x, y, c));
        }
    }

    void theEdgeIsFeatheredOutwardByTheBlend() {
        // A box over the whole picture, the left half masked, a white crop on a black picture: the
        // result is the feathered mask itself.
        const Pixels original(64, 8, 3, 0.0f);
        const Pixels mask = rectMask(64, 8, 0, 0, 32, 8);
        const Pixels white(64, 8, 3, 1.0f);
        const Pixels out = stitch(original, mask, CropBox{0, 0, 64, 8}, white, 8);
        const Pixels want = featherMask(mask, 8, FeatherDirection::Outward);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 64; ++x)
                for (int c = 0; c < 3; ++c)
                    QVERIFY(std::fabs(out.at(x, y, c) - want.at(x, y, 0)) < 1e-6f);
        QCOMPARE(out.at(0, 0, 0), 1.0f);   // a mask reaching the picture's edge stays full there
        QCOMPARE(out.at(31, 4, 0), 1.0f);
        QVERIFY(out.at(34, 4, 0) > 0.0f && out.at(34, 4, 0) < 1.0f);
        QCOMPARE(out.at(63, 4, 0), 0.0f);
    }

    void aMaskTouchingTheBorderCropsAndStitches() {
        const Pixels image = picture(100, 100);
        const Pixels mask = rectMask(100, 100, 90, 90, 10, 10);
        const std::optional<Cropped> crop = cropToMask(image, mask, 64, 0.0, 16);
        QVERIFY(crop);
        QCOMPARE(crop->box, (CropBox{0, 0, 100, 100})); // 10 + 2 x 64 is wider than the picture
        QVERIFY(maxDiff(stitch(image, mask, crop->box, crop->image, 16), image) < 1e-6f);
    }

    void aOnePixelMaskAndAHugeMaskWork() {
        const Pixels image = picture(200, 120);
        Pixels one(200, 120, 1);
        one.at(100, 60, 0) = 1.0f;
        const std::optional<Cropped> small = cropToMask(image, one, 64, 1.0, 16);
        QVERIFY(small);
        QCOMPARE(small->box, (CropBox{36, 0, 129, 120}));
        QVERIFY(maxDiff(stitch(image, one, small->box, small->image, 16), image) < 0.03f);

        const Pixels all(200, 120, 1, 1.0f);
        const std::optional<Cropped> full = cropToMask(image, all, 64, 1.0, 16);
        QVERIFY(full);
        QCOMPARE(full->box, (CropBox{0, 0, 200, 120}));
        QVERIFY(maxDiff(stitch(image, all, full->box, full->image, 16), image) < 0.03f);
    }

    void aRedrawnCropOfAnotherSizeIsResizedToTheBox() {
        const Pixels image = picture(120, 80);
        const Pixels mask = rectMask(120, 80, 40, 30, 20, 20);
        const CropBox box{30, 20, 40, 40};
        const Pixels redrawn(100, 100, 3, 1.0f); // larger than the box
        const Pixels out = stitch(image, mask, box, redrawn, 0);
        QCOMPARE(out.at(50, 40, 0), 1.0f);
        QCOMPARE(out.at(35, 25, 0), image.at(35, 25, 0)); // inside the box, outside the mask
        QCOMPARE(out.at(0, 0, 0), image.at(0, 0, 0));
    }

    void theShapeFitsWithinTwoPercent() {
        const CropSize size{1024, 1024};
        QVERIFY(shapeFits(size, 1024, 1024));
        QVERIFY(shapeFits(size, 512, 512));
        QVERIFY(shapeFits(size, 2048, 2048));
        QVERIFY(shapeFits(size, 1024, 1010)); // 1.4%
        QVERIFY(!shapeFits(size, 1024, 1000)); // 2.4%
        QVERIFY(!shapeFits(size, 1264, 832));
        QVERIFY(!shapeFits(size, 0, 10));
        QVERIFY(!shapeFits(CropSize{}, 10, 10));
    }

    // ---- the nodes ----

    void cropToMaskOutputsThePictureTheMaskAndTheHandle() {
        Bench bench;
        const Pixels image = picture(300, 200);
        const MediaHandle original = bench.save("original.png", image);
        const MediaHandle mask = bench.save("mask.png", rectMask(300, 200, 100, 80, 40, 30));
        const Result<Values> out = bench.run("fugue/crop-to-mask", {{"image", Value(original)}, {"mask", Value(mask)},
                                                                       {"padding", 20}, {"megapixels", 0.25}});
        QVERIFY2(out.ok(), qPrintable(out.errors().value(0).message));
        const int width = int(out->value("width").number()), height = int(out->value("height").number());
        QCOMPARE(out->value("image").media().width(), width);
        QCOMPARE(out->value("image").media().height(), height);
        QCOMPARE(out->value("mask").media().width(), width);
        QCOMPARE(out->value("mask").media().channels(), 1);
        QCOMPARE(QSize(width, height), QSize(cropSizeFor(CropBox{80, 60, 80, 70}, 0.25, 16).width, cropSizeFor(CropBox{80, 60, 80, 70}, 0.25, 16).height));

        const QJsonObject handle = out->value("stitch").json().toObject();
        QCOMPARE(handle.value("data").toString(), QStringLiteral("fugue/stitch"));
        QVERIFY(QFile::exists(handle.value("path").toString()));
        const QJsonObject meta = handle.value("meta").toObject();
        QCOMPARE(meta.value("image").toObject(), original.toJson());
        QCOMPARE(meta.value("mask").toObject(), mask.toJson());
        QCOMPARE(meta.value("box").toObject(), (QJsonObject{{"x", 80}, {"y", 60}, {"width", 80}, {"height", 70}}));
        QCOMPARE(meta.value("size").toObject(), (QJsonObject{{"width", width}, {"height", height}}));
        // The file holds the meta and nothing else, so what the scheduler hashes is the meta.
        QCOMPARE(QJsonDocument::fromJson(readBytes(handle.value("path").toString())).object(), meta);
    }

    void theHandleIsStableAndChangesWithTheOriginalOrTheMask() {
        Bench bench;
        const MediaHandle original = bench.save("original.png", picture(300, 200));
        const MediaHandle mask = bench.save("mask.png", rectMask(300, 200, 100, 80, 40, 30));
        const auto handleOf = [&](const MediaHandle &image, const MediaHandle &m) {
            const Result<Values> out = bench.run("fugue/crop-to-mask", {{"image", Value(image)}, {"mask", Value(m)}});
            if (!out)
                qFatal("%s", qPrintable(out.errors().value(0).message));
            return readBytes(out->value("stitch").json().toObject().value("path").toString());
        };
        const QByteArray first = handleOf(original, mask);
        QVERIFY(!first.isEmpty());
        QCOMPARE(handleOf(original, mask), first);
        QCOMPARE(sha256Hex(first), sha256Hex(handleOf(original, mask)));
        QVERIFY(handleOf(bench.save("other.png", picture(300, 200, 2)), mask) != first);
        QVERIFY(handleOf(original, bench.save("mask2.png", rectMask(300, 200, 100, 80, 41, 30))) != first);
    }

    void cropToMaskRefusesAnEmptyMask() {
        Bench bench;
        const Result<Values> out = bench.run("fugue/crop-to-mask", {{"image", Value(bench.save("a.png", picture(64, 64)))},
                                                                       {"mask", Value(bench.save("m.png", Pixels(64, 64, 1)))}});
        QVERIFY(!out.ok());
        QCOMPARE(out.error().code, QStringLiteral("mask_empty"));
        QCOMPARE(out.error().input, QStringLiteral("mask"));
    }

    void aMaskOfAnotherSizeIsScaledWithAWarning() {
        Bench bench;
        const MediaHandle original = bench.save("a.png", picture(200, 100));
        const Result<Values> out = bench.run("fugue/crop-to-mask",
                                             {{"image", Value(original)},
                                              {"mask", Value(bench.save("m.png", rectMask(100, 50, 40, 20, 20, 10)))},
                                              {"padding", 0}, {"megapixels", 0}});
        QVERIFY2(out.ok(), qPrintable(out.errors().value(0).message));
        QCOMPARE(out.warnings().value(0).code, QStringLiteral("mask_resized"));
        QCOMPARE(out.warnings().value(0).severity, Error::Severity::Warning);
        const QJsonObject box = out->value("stitch").json().toObject().value("meta").toObject().value("box").toObject();
        QVERIFY(box.value("x").toInt() <= 80 && box.value("x").toInt() >= 76);
        QVERIFY(box.value("width").toInt() >= 40);
    }

    void cropThenStitchGivesTheOriginalBack() {
        Bench bench;
        const Pixels pixels = picture(300, 200);
        const MediaHandle original = bench.save("original.png", pixels);
        const MediaHandle mask = bench.save("mask.png", rectMask(300, 200, 100, 80, 40, 30));
        const Result<Values> cropped = bench.run("fugue/crop-to-mask", {{"image", Value(original)}, {"mask", Value(mask)},
                                                                          {"megapixels", 0}});
        QVERIFY(cropped.ok());
        const Result<Values> out = bench.run("fugue/stitch", {{"stitch", cropped->value("stitch")},
                                                              {"image", cropped->value("image")}, {"blend", 0}});
        QVERIFY2(out.ok(), qPrintable(out.errors().value(0).message));
        const Pixels got = read(out->value("image").media());
        QCOMPARE(maxDiff(got, read(original)), 0.0f);

        // Resized to a megapixel and blended: byte for byte outside, near inside.
        const Result<Values> big = bench.run("fugue/crop-to-mask", {{"image", Value(original)}, {"mask", Value(mask)}});
        const Result<Values> again = bench.run("fugue/stitch", {{"stitch", big->value("stitch")}, {"image", big->value("image")}});
        QVERIFY2(again.ok(), qPrintable(again.errors().value(0).message));
        QVERIFY(maxDiff(read(again->value("image").media()), read(original)) < 0.04f);
    }

    void stitchTakesAnotherMaskWhenGiven() {
        Bench bench;
        const MediaHandle original = bench.save("original.png", picture(300, 200));
        const MediaHandle mask = bench.save("mask.png", rectMask(300, 200, 100, 80, 40, 30));
        const Result<Values> cropped = bench.run("fugue/crop-to-mask", {{"image", Value(original)}, {"mask", Value(mask)},
                                                                          {"megapixels", 0}});
        const Pixels white(cropped->value("image").media().width(), cropped->value("image").media().height(), 3, 1.0f);
        const MediaHandle redrawn = bench.save("redrawn.png", white);
        // Only a 10 x 10 corner of the mask is laid.
        const MediaHandle narrow = bench.save("narrow.png", rectMask(300, 200, 100, 80, 10, 10));
        const Result<Values> out = bench.run("fugue/stitch", {{"stitch", cropped->value("stitch")}, {"image", Value(redrawn)},
                                                              {"blend", 0}, {"mask", Value(narrow)}});
        QVERIFY2(out.ok(), qPrintable(out.errors().value(0).message));
        const Pixels got = read(out->value("image").media());
        QCOMPARE(got.at(105, 85, 0), 1.0f);
        QCOMPARE(got.at(130, 100, 0), read(original).at(130, 100, 0)); // masked in the handle, not in this mask
    }

    void stitchRefusesAPictureOfAnotherShape() {
        Bench bench;
        const MediaHandle original = bench.save("original.png", picture(300, 200));
        const MediaHandle mask = bench.save("mask.png", rectMask(300, 200, 100, 80, 40, 30));
        const Result<Values> cropped = bench.run("fugue/crop-to-mask", {{"image", Value(original)}, {"mask", Value(mask)}});
        QVERIFY(cropped.ok());
        const int w = cropped->value("width").number(), h = cropped->value("height").number();
        const Result<Values> wrong = bench.run("fugue/stitch", {{"stitch", cropped->value("stitch")},
                                                                {"image", Value(bench.save("wrong.png", Pixels(w, h / 2, 3, 0.5f)))}});
        QVERIFY(!wrong.ok());
        QCOMPARE(wrong.error().code, QStringLiteral("stitch_mismatch"));
        QCOMPARE(wrong.error().input, QStringLiteral("image"));
        // The same shape at another size is fine (an edit model may return a larger picture).
        const Result<Values> scaled = bench.run("fugue/stitch", {{"stitch", cropped->value("stitch")},
                                                                 {"image", Value(bench.save("half.png", Pixels(w / 2, h / 2, 3, 0.5f)))}});
        QVERIFY2(scaled.ok(), qPrintable(scaled.errors().value(0).message));
    }

    void stitchRefusesAWrongHandleOrAChangedOriginal() {
        Bench bench;
        const MediaHandle original = bench.save("original.png", picture(100, 100));
        const MediaHandle mask = bench.save("mask.png", rectMask(100, 100, 40, 40, 20, 20));
        const Result<Values> cropped = bench.run("fugue/crop-to-mask", {{"image", Value(original)}, {"mask", Value(mask)},
                                                                          {"megapixels", 0}});
        QVERIFY(cropped.ok());
        const Value crop = cropped->value("image");
        const Result<Values> notAHandle = bench.run("fugue/stitch", {{"stitch", Value(QJsonObject{{"data", "fugue/latent"}})}, {"image", crop}});
        QCOMPARE(notAHandle.error().code, QStringLiteral("input_invalid"));
        // The picture the handle names has changed since Crop to Mask read it.
        bench.save("original.png", picture(100, 100, 5));
        const Result<Values> stale = bench.run("fugue/stitch", {{"stitch", cropped->value("stitch")}, {"image", crop}});
        QCOMPARE(stale.error().code, QStringLiteral("file_not_found"));
        QCOMPARE(stale.error().input, QStringLiteral("stitch"));
    }

    // ---- the type and the group ----

    void theStitchTypeIsDeclaredAndTheNodesUseIt() {
        Bench bench;
        const DeclaredPortType *type = bench.registry.portType(*PortType::parse(QStringLiteral("fugue/stitch")));
        QVERIFY(type);
        QCOMPARE(type->kind, PortKind::Data);
        QCOMPARE(type->name, QStringLiteral("Stitch"));
        QCOMPARE(bench.registry.nodeType("fugue/crop-to-mask")->output("stitch")->type.toString(), QStringLiteral("fugue/stitch"));
        QCOMPARE(bench.registry.nodeType("fugue/stitch")->input("stitch")->type.toString(), QStringLiteral("fugue/stitch"));
        QCOMPARE(bench.registry.nodeType("fugue/crop-to-mask")->input("mask")->role, QStringLiteral("mask"));
        QCOMPARE(bench.registry.nodeType("fugue/crop-to-mask")->output("mask")->role, QStringLiteral("mask"));
        QCOMPARE(bench.registry.nodeType("fugue/stitch")->input("mask")->role, QStringLiteral("mask"));
        QVERIFY(bench.registry.nodeType("fugue/stitch")->input("mask")->optional);
    }

    void theBuiltInPackShipsTheGroup() {
        Bench bench;
        const NodeType *group = bench.registry.nodeType("fugue/inpaint-crop-stitch");
        QVERIFY(group);
        QCOMPARE(group->runtime.value("kind").toString(), QStringLiteral("group"));
        QCOMPARE(group->name, QStringLiteral("Inpaint (crop and stitch)"));
        for (const char *name : {"model", "image", "mask", "prompt", "seed", "steps", "guidance", "denoise", "padding", "megapixels", "blend"})
            QVERIFY2(group->input(QLatin1String(name)), name);
        QVERIFY(group->output("image"));
        QCOMPARE(group->input("padding")->defaultValue.toInt(), 64);
        QCOMPARE(group->input("blend")->defaultValue.toInt(), 16);
        QCOMPARE(group->input("denoise")->defaultValue.toInt(), 1);

        const Result<Document> file = loadLibraryGroup(*group);
        QVERIFY2(file.ok(), qPrintable(file.errors().value(0).message));
        const GroupDefinition *definition = file->group("fugue/inpaint-crop-stitch");
        QVERIFY(definition);
        QCOMPARE(definition->nodes.size(), 6);

        // An instance brings its definition in as a paste does, and compiles.
        Document doc;
        Node instance;
        instance.type = group->id;
        instance.version = group->version;
        doc.graph.nodes.insert("inpaint", instance);
        QCOMPARE(embedGroupTypes(doc, bench.registry), QStringList{"fugue/inpaint-crop-stitch"});
        const Compilation compiled = compile(doc, bench.registry);
        for (const Error &error : compiled.errors())
            QVERIFY2(error.severity != Error::Severity::Error || error.code == QLatin1String("missing_input"),
                     qPrintable(error.code + ": " + error.message));
    }

    // ---- end to end: the shipped binary, the fake helper (no inference here) ----

    // The group on the fake helper. A square mask crops to a square, which the fake decode's 1 x 1
    // picture fits; everything outside the box is the original's. A mask of another shape is
    // `stitch_mismatch` with that picture.
    void theGroupRunsOnTheFakeHelper() {
        Bench bench;
        const Pixels pixels = picture(300, 300);
        QDir().mkpath(bench.dir.filePath("inputs")); // the folder the run may read
        bench.save("inputs/picture.png", pixels);
        bench.save("inputs/mask.png", rectMask(300, 300, 120, 120, 40, 40));
        bench.save("inputs/wide.png", rectMask(300, 300, 100, 120, 100, 20));
        const auto model = [&](const QString &type, const QString &file) {
            const QByteArray content = file.toUtf8() + " weights";
            QDir().mkpath(QFileInfo(dataDir() + "/models/" + file).absolutePath());
            QFile f(dataDir() + "/models/" + file);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(content) != content.size())
                qFatal("cannot write %s", qPrintable(f.fileName()));
            return QJsonObject{{"resource", type}, {"hash", sha256(content)}, {"name", QFileInfo(file).fileName()}};
        };
        const auto flow = [&](const QString &mask) {
            const auto from = [](const QString &output) { return QJsonObject{{"from", output}}; };
            const QJsonObject nodes{
                {"models", QJsonObject{{"type", "fugue/load-models"}, {"version", "1.0.0"},
                                       {"inputs", QJsonObject{{"diffusion_model", model("fugue/diffusion-model", "diffusion_models/z-image-turbo.gguf")},
                                                              {"text_encoder", model("fugue/text-encoder", "clip/Qwen3-4B.gguf")},
                                                              {"vae", model("fugue/vae", "vae/ae.safetensors")}}}}},
                {"picture", QJsonObject{{"type", "fugue/load-image"}, {"version", "1.0.0"}, {"inputs", QJsonObject{{"file", bench.dir.filePath("inputs/picture.png")}}}}},
                {"paint", QJsonObject{{"type", "fugue/load-image"}, {"version", "1.0.0"}, {"inputs", QJsonObject{{"file", bench.dir.filePath("inputs/" + mask)}}}}},
                {"inpaint", QJsonObject{{"type", "fugue/inpaint-crop-stitch"}, {"version", "1.0.0"},
                                        {"inputs", QJsonObject{{"model", from("models.model")}, {"image", from("picture.image")},
                                                               {"mask", from("paint.image")}, {"prompt", "a red umbrella"},
                                                               {"seed", 1}, {"steps", 4}}}}}};
            return QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}};
        };
        const auto run = [&](const QString &mask) {
            const QString path = bench.dir.filePath("flow.fugue");
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                qFatal("cannot write the flow");
            f.write(QJsonDocument(flow(mask)).toJson(QJsonDocument::Compact));
            f.close();
            // As the canvas does when a group is added: the definition comes into the workflow.
            Result<Document> document = Document::loadFile(path);
            if (!document)
                qFatal("%s", qPrintable(document.errors().value(0).message));
            embedGroupTypes(document.value(), bench.registry);
            if (hasErrors(document->saveFile(path)))
                qFatal("cannot save the flow");
            return fugue(bench, {"run", path, "--to", "inpaint/stitch", "--wait", "--json"});
        };

        const QList<QJsonObject> square = run("mask.png");
        QVERIFY(!square.isEmpty());
        QCOMPARE(square.last().value("event").toString(), QStringLiteral("run.done"));
        QCOMPARE(square.last().value("status").toString(), QStringLiteral("ok"));
        QJsonObject outputs;
        for (const QJsonObject &event : square)
            if (event.value("event").toString() == QLatin1String("node.done") && event.value("node").toString() == QLatin1String("inpaint/stitch"))
                outputs = event.value("outputs").toObject();
        const QJsonObject image = outputs.value("image").toObject();
        QCOMPARE(image.value("width").toInt(), 300);
        QCOMPARE(image.value("height").toInt(), 300);
        const Result<MediaHandle> result = MediaHandle::fromJson(image);
        QVERIFY(result.ok());
        const Pixels out = read(result.value());
        const Pixels was = read(bench.save("inputs/picture.png", pixels));
        // The box: 40 px of mask and 64 of padding on each side, the 1 x 1 picture stretched over it.
        const CropBox box{56, 56, 168, 168};
        int outside = 0;
        for (int y = 0; y < 300; ++y)
            for (int x = 0; x < 300; ++x)
                if (!insideBox(box, x, y)) {
                    ++outside;
                    QCOMPARE(out.at(x, y, 0), was.at(x, y, 0));
                }
        QVERIFY(outside > 0);
        QVERIFY(std::fabs(out.at(140, 140, 0) - was.at(140, 140, 0)) > 0.0f); // the middle of the mask was redrawn

        const QList<QJsonObject> wide = run("wide.png");
        QCOMPARE(wide.last().value("status").toString(), QStringLiteral("failed"));
        QVERIFY(QJsonDocument(QJsonArray::fromVariantList({QVariant(wide.last())})).toJson().contains("stitch_mismatch"));
    }

private:
    static QByteArray sha256Hex(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }

    // The shipped binary with the fake helper as this machine's helper; its events.
    static QList<QJsonObject> fugue(const Bench &bench, const QStringList &args) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *name : {"QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "FUGUE_NODE_MANIFESTS"})
            env.remove(QString::fromLatin1(name));
        // Its own cache and runs; the models are in the data folder this test wrote them to.
        env.insert("XDG_CACHE_HOME", bench.dir.filePath("cache"));
        env.insert("XDG_STATE_HOME", bench.dir.filePath("state"));
        env.insert("FUGUE_HELPER", fugueFakeHelper());
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(bench.dir.path()); // inside the write roots (13.6)
        process.start(fugueBinary(), args);
        if (!process.waitForFinished(120000)) {
            process.kill();
            process.waitForFinished();
            qWarning("fugue timed out: %s", qPrintable(args.join(' ')));
            return {};
        }
        const QByteArray out = process.readAllStandardOutput();
        if (process.exitCode() != 0)
            qWarning("fugue %s: %s %s", qPrintable(args.join(' ')), out.constData(), process.readAllStandardError().constData());
        QList<QJsonObject> events;
        for (const QByteArray &line : out.trimmed().split('\n'))
            events.append(QJsonDocument::fromJson(line).object());
        return events;
    }
};

QTEST_GUILESS_MAIN(TestCropStitch)
#include "test_crop_stitch.moc"
