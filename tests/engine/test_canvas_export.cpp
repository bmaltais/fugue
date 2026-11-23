// The canvas exporter (#247 Export as…, #248 Export layers as files; epic #206): one flattened picture in a format, quality,
// size and background, and one picture per layer. Seam: engine/canvas_export.h. The CLI and the dialogs are its callers.

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/canvas_export.h"
#include "engine/paint_file.h"

using namespace fugue;
using namespace fugue::canvas;
using namespace fugue::mask;

namespace {

// A color buffer with `paint` called for each pixel (x, y) that returns its color.
template <typename F> MaskPaint colored(int width, int height, F paint) {
    std::vector<uint8_t> bytes(size_t(width) * height * 4, 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const Rgba c = paint(x, y);
            const size_t at = (size_t(y) * width + x) * 4;
            bytes[at] = c.r, bytes[at + 1] = c.g, bytes[at + 2] = c.b, bytes[at + 3] = c.a;
        }
    return MaskPaint(width, height, bytes, 4);
}

// 40 x 20: a red 10 x 10 square at the left of a transparent layer "Sky", and a half-transparent blue layer "Water: deep/1".
PaintDocument sample() {
    PaintDocument document(40, 20);
    document.loadLayer(LayerKind::Paint, "Sky", true, 1.0, colored(40, 20, [](int x, int y) { return x < 10 && y < 10 ? Rgba{200, 30, 30, 255} : Rgba{0, 0, 0, 0}; }));
    document.loadLayer(LayerKind::Paint, "Water: deep/1", true, 0.5, colored(40, 20, [](int x, int) { return x >= 20 ? Rgba{10, 20, 220, 128} : Rgba{0, 0, 0, 0}; }));
    return document;
}

PictureExport as(const QString &format) {
    PictureExport options;
    options.format = format;
    return options;
}

MaskPaint decode(const QByteArray &bytes) {
    const Result<MaskPaint> picture = MaskPaint::fromPicture(bytes);
    return picture ? picture.value() : MaskPaint(1, 1, 4);
}

} // namespace

class TestCanvasExport : public QObject {
    Q_OBJECT

private slots:
    void aPictureIsTheFlattenedCanvas() {
        const PaintDocument document = sample();
        const Result<ExportedPicture> png = exportPicture(document, {});
        QVERIFY(png);
        QCOMPARE(png->bytes, document.flattenToPng().value()); // the very bytes the history window's picture has
        QCOMPARE(png->format, QString("png"));
        QCOMPARE(png->background, QString("transparent"));
        const Result<ExportedPicture> jpg = exportPicture(document, as("jpg"));
        QVERIFY(jpg);
        QCOMPARE(jpg->bytes, document.flattenToPicture("jpg").value()); // the old default: quality 92 on white
        QCOMPARE(jpg->background, QString("white"));
        const Result<ExportedPicture> webp = exportPicture(document, as("webp"));
        QCOMPARE(webp->bytes, document.flattenToPicture("webp").value());
    }

    void qualityChangesTheSizeOfAJpegAndALossyWebp() {
        PaintDocument document(128, 128);
        const MaskPaint noise = colored(128, 128, [](int x, int y) { return Rgba{uint8_t((x * 7 + y * 13) % 256), uint8_t((x * y) % 256), uint8_t((x ^ y) * 3), 255}; });
        document.loadLayer(LayerKind::Paint, "Noise", true, 1.0, noise);
        for (const char *format : {"jpg", "webp"}) {
            PictureExport low = as(format);
            low.quality = 10;
            low.lossless = false;
            PictureExport high = low;
            high.quality = 95;
            const Result<ExportedPicture> small = exportPicture(document, low), large = exportPicture(document, high);
            QVERIFY2(small && large, format);
            QVERIFY2(small->bytes.size() < large->bytes.size(), format);
        }
        PictureExport bad;
        bad.format = "jpg";
        bad.quality = 0;
        QCOMPARE(exportPicture(document, bad).error().code, QString("usage"));
        bad.quality = 101;
        QCOMPARE(exportPicture(document, bad).error().code, QString("usage"));
    }

    void sizeIsAScaleAWidthOrAHeightKeepingProportionsOrBoth() {
        const auto size = [](const PictureExport &options) { return exportSize(40, 20, options).value(); };
        PictureExport options;
        options.scale = 0.5;
        QCOMPARE(size(options), QSize(20, 10));
        options = {};
        options.width = 10;
        QCOMPARE(size(options), QSize(10, 5));
        options = {};
        options.height = 40;
        QCOMPARE(size(options), QSize(80, 40));
        options.width = 7;
        QCOMPARE(size(options), QSize(7, 40)); // both: exactly that
        options = {};
        options.scale = 0.001;
        QCOMPARE(size(options), QSize(1, 1)); // never below one pixel
        options.scale = 0;
        QCOMPARE(exportSize(40, 20, options).error().code, QString("usage"));
        options = {};
        options.width = 20000;
        QCOMPARE(exportSize(40, 20, options).error().code, QString("input_invalid"));
    }

    void aScaledPictureHasTheSizeAndKeepsTheColors() {
        PictureExport options;
        options.scale = 0.5;
        const Result<ExportedPicture> half = exportPicture(sample(), options);
        QVERIFY(half);
        QCOMPARE(half->width, 20);
        QCOMPARE(half->height, 10);
        const MaskPaint picture = decode(half->bytes);
        QCOMPARE(picture.width(), 20);
        QCOMPARE(picture.height(), 10);
        const Rgba inside = picture.rgbaAt(2, 2);
        QVERIFY2(qAbs(inside.r - 200) < 6 && qAbs(inside.g - 30) < 6 && qAbs(inside.b - 30) < 6 && inside.a > 249, qPrintable(QString("%1 %2 %3 %4").arg(inside.r).arg(inside.g).arg(inside.b).arg(inside.a))); // inside the square: unchanged but for the ringing of Lanczos
        QVERIFY(picture.rgbaAt(7, 2).a < 4); // empty stays empty
        options.scale = 2;
        QCOMPARE(decode(exportPicture(sample(), options)->bytes).width(), 80);
    }

    void aBackgroundIsLaidUnderTheTransparency() {
        PictureExport options;
        options.background = "#336699";
        const MaskPaint picture = decode(exportPicture(sample(), options)->bytes);
        QCOMPARE(picture.rgbaAt(15, 15), (Rgba{0x33, 0x66, 0x99, 255}));
        QCOMPARE(picture.rgbaAt(2, 2), (Rgba{200, 30, 30, 255}));
        QCOMPARE(exportPicture(sample(), options)->background, QString("#336699"));
        options.format = "jpg";
        options.background = "transparent";
        QCOMPARE(exportPicture(sample(), options).error().code, QString("input_invalid")); // JPEG has no alpha
        options.background = "teal-ish";
        QVERIFY(!exportPicture(sample(), options));
    }

    void layeredKeepsTheLayersAtTheSize() {
        PictureExport options;
        options.format = "ora";
        options.scale = 0.5;
        const Result<ExportedPicture> layered = exportPicture(sample(), options);
        QVERIFY(layered);
        const Result<PaintDocument> back = paintFromOra(layered->bytes);
        QVERIFY(back);
        QCOMPARE(back->layerCount(), 2);
        QCOMPARE(back->width(), 20);
        QCOMPARE(exportPicture(sample(), as("ora"))->bytes, paintToOra(sample()).value());
        QCOMPARE(exportPicture(sample(), as("gif")).error().code, QString("usage"));
    }

    void oneFilePerLayerNamedFromTheLayerNames() {
        PaintDocument document = sample();
        document.loadLayer(LayerKind::Mask, "Selection", true, 1.0, MaskPaint(40, 20, 1));
        const Result<LayerFiles> made = exportLayers(document, {});
        QVERIFY(made);
        QCOMPARE(int(made->files.size()), 2);
        QCOMPARE(made->files[0].fileName, QString("01-Sky.png"));
        QCOMPARE(made->files[1].fileName, QString("02-Water_ deep_1.png"));
        QCOMPARE(made->skipped, QStringList{"Selection"});
        // The layer's own pixels: opacity is not applied.
        const MaskPaint water = decode(made->files[1].bytes);
        QCOMPARE(water.rgbaAt(30, 5), (Rgba{10, 20, 220, 128}));
        QCOMPARE(decode(made->files[0].bytes).rgbaAt(2, 2), (Rgba{200, 30, 30, 255}));
        LayersExport options;
        options.prefix = "shot";
        options.format = "webp";
        QCOMPARE(exportLayers(document, options)->files[0].fileName, QString("shot-01-Sky.webp"));
        options.format = "jpg";
        QCOMPARE(exportLayers(document, options).error().code, QString("usage"));
    }

    void hiddenLayersAreWrittenUnlessAskedNot() {
        PaintDocument document = sample();
        document.setVisible(document.layer(0).id, false);
        QCOMPARE(int(exportLayers(document, {})->files.size()), 2);
        LayersExport options;
        options.visibleOnly = true;
        const Result<LayerFiles> visible = exportLayers(document, options);
        QCOMPARE(int(visible->files.size()), 1);
        QCOMPARE(visible->files[0].name, QString("Water: deep/1"));
        QCOMPARE(visible->files[0].fileName, QString("02-Water_ deep_1.png")); // its place in the stack does not shift
        document.setVisible(document.layer(1).id, false);
        QCOMPARE(exportLayers(document, options).error().code, QString("input_invalid"));
    }

    void aLayerMaskIsAppliedUnlessAskedNot() {
        PaintDocument document = sample();
        const int sky = document.layer(0).id;
        std::vector<uint8_t> values(40 * 20, 0);
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 5; ++x)
                values[size_t(y) * 40 + x] = 255; // only the left half of the square shows
        const MaskPaint half(40, 20, values, 1);
        document.setLayerMask(sky, half);
        const MaskPaint masked = decode(exportLayers(document, {})->files[0].bytes);
        QCOMPARE(masked.rgbaAt(2, 2).a, uint8_t(255));
        QCOMPARE(masked.rgbaAt(7, 2).a, uint8_t(0));
        LayersExport options;
        options.applyMasks = false;
        QCOMPARE(decode(exportLayers(document, options)->files[0].bytes).rgbaAt(7, 2).a, uint8_t(255));
        document.setMaskEnabled(sky, false); // a mask turned off shows the layer whole, as the canvas does
        QCOMPARE(decode(exportLayers(document, {})->files[0].bytes).rgbaAt(7, 2).a, uint8_t(255));
    }

    void fileNamesAreSafe() {
        QCOMPARE(safeFileName("a/b\\c:d*e?f\"g<h>i|j"), QString("a_b_c_d_e_f_g_h_i_j"));
        QCOMPARE(safeFileName("  ..hidden. "), QString("hidden"));
        QCOMPARE(safeFileName(""), QString("Layer"));
        QCOMPARE(safeFileName("..."), QString("Layer"));
        QCOMPARE(safeFileName(QString(200, 'x')).size(), 80);
    }

    void writingNeverReplacesAndChecksEveryNameFirst() {
        QTemporaryDir dir;
        const Result<LayerFiles> made = exportLayers(sample(), {});
        const QString folder = dir.filePath("out/nested");
        QVERIFY(!hasErrors(writeLayerFiles(folder, made.value()))); // the folder is made
        QVERIFY(QFile::exists(folder + "/01-Sky.png") && QFile::exists(folder + "/02-Water_ deep_1.png"));
        // A second run is refused whole; and with one name taken, nothing of the other is written.
        const Errors again = writeLayerFiles(folder, made.value());
        QCOMPARE(again.at(0).code, QString("permission_denied"));
        QVERIFY(again.at(0).message.contains("01-Sky.png"));
        QVERIFY(QFile::remove(folder + "/02-Water_ deep_1.png"));
        QVERIFY(hasErrors(writeLayerFiles(folder, made.value())));
        QVERIFY(!QFile::exists(folder + "/02-Water_ deep_1.png"));
        QFile file(dir.filePath("plain"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(hasErrors(writeLayerFiles(dir.filePath("plain"), made.value()))); // a file where a folder is wanted
    }
};

QTEST_GUILESS_MAIN(TestCanvasExport)
#include "test_canvas_export.moc"
