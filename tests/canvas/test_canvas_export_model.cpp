// The two export dialogs' model (#247 Export as…, #248 Export layers as files; epic #206): the choices, the numbers and
// the preview made in the background, and the write that never replaces a file. Seam: canvas/canvas_export_model.h with a
// store of the test's own; the work itself is tests/engine/test_canvas_export.cpp. No window.

#include <QDir>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/canvas_export_model.h"
#include "engine/canvas_store.h"
#include "engine/paint_document.h"

using namespace fugue;

namespace {

struct Fixture {
    QTemporaryDir dir;
    CanvasExportModel model;
    QString id;
    Fixture() {
        model.setStoreRoot(dir.filePath("store"));
        mask::PaintDocument document(200, 100);
        std::vector<uint8_t> red(200 * 100 * 4, 0);
        for (int y = 0; y < 50; ++y)
            for (int x = 0; x < 50; ++x) {
                const size_t at = (size_t(y) * 200 + x) * 4;
                red[at] = 200, red[at + 1] = 30, red[at + 2] = 30, red[at + 3] = 255;
            }
        document.loadLayer(mask::LayerKind::Paint, "Sky", true, 1.0, mask::MaskPaint(200, 100, red, 4));
        document.loadLayer(mask::LayerKind::Paint, "Empty", false, 1.0, mask::MaskPaint(200, 100, 4));
        const Result<canvas::CanvasInfo> made = canvas::CanvasStore(dir.filePath("store")).create("Harbour / dawn", document, "test");
        id = made->id;
    }
    void ready() { QTRY_VERIFY(model.canExport() || !model.problem().isEmpty()); }
};

} // namespace

class TestCanvasExportModel : public QObject {
    Q_OBJECT

private slots:
    void opensOnAPngAtTheCanvasSizeAndSaysWhatItWillBe() {
        Fixture f;
        QVERIFY(f.model.open(f.id, "", "picture").isEmpty());
        QCOMPARE(f.model.format(), QString("png"));
        QCOMPARE(f.model.widthText(), QString("200"));
        QCOMPARE(f.model.heightText(), QString("100"));
        QCOMPARE(f.model.percent(), 100);
        QVERIFY(f.model.busy());
        f.ready();
        QVERIFY(f.model.canExport());
        QVERIFY2(f.model.estimate().contains("200 × 100 px"), qPrintable(f.model.estimate()));
        QCOMPARE(f.model.fileName(), QString("Harbour _ dawn.png"));
        QVERIFY(f.model.preview().isLocalFile());
        QCOMPARE(QImage(f.model.preview().toLocalFile()).size(), QSize(200, 100));
    }

    void aChangeMakesTheNumbersAgainAndALaterOneWins() {
        Fixture f;
        f.model.open(f.id, "", "picture");
        f.ready();
        const QString before = f.model.estimate();
        f.model.setWidthText("100"); // keeps the proportions
        QCOMPARE(f.model.heightText(), QString("50"));
        QCOMPARE(f.model.percent(), 50);
        f.model.setPercent(25);
        QVERIFY(f.model.busy());
        QVERIFY(!f.model.canExport()); // stale: nothing is written from numbers that are not current
        QTRY_VERIFY(f.model.canExport());
        QVERIFY2(f.model.estimate().contains("50 × 25 px"), qPrintable(f.model.estimate()));
        QVERIFY(f.model.estimate() != before);
        f.model.setHeightText("200");
        QCOMPARE(f.model.widthText(), QString("400"));
        f.model.setWidthText("0"); // not a size: ignored
        f.model.setWidthText("many");
        QCOMPARE(f.model.widthText(), QString("400"));
    }

    void aSizePastTheLimitIsAProblemInWords() {
        Fixture f;
        f.model.open(f.id, "", "picture");
        f.model.setWidthText("20000");
        QTRY_VERIFY(!f.model.problem().isEmpty());
        QVERIFY(!f.model.canExport());
        QVERIFY2(f.model.problem().contains("limit"), qPrintable(f.model.problem()));
        QVERIFY(!f.model.exportTo(f.dir.filePath("big.png")));
        QVERIFY(!QFile::exists(f.dir.filePath("big.png")));
        f.model.setWidthText("100");
        QTRY_VERIFY(f.model.canExport());
        QVERIFY(f.model.problem().isEmpty());
    }

    void qualityAndLosslessFollowTheFormat() {
        Fixture f;
        f.model.open(f.id, "", "picture");
        QVERIFY(!f.model.hasQuality());
        f.model.setFormat("jpg");
        QVERIFY(f.model.hasQuality());
        QVERIFY(!f.model.canBeLossless());
        f.model.setFormat("webp");
        QVERIFY(f.model.canBeLossless());
        QVERIFY(!f.model.hasQuality()); // lossless by default
        f.model.setLossless(false);
        QVERIFY(f.model.hasQuality());
        f.model.setQuality(500);
        QCOMPARE(f.model.quality(), 100);
        f.model.setFormat("gif"); // not a format: ignored
        QCOMPARE(f.model.format(), QString("webp"));
    }

    void aJpegIsNeverTransparent() {
        Fixture f;
        f.model.open(f.id, "", "picture");
        f.model.setBackground("transparent");
        f.model.setFormat("jpg");
        QCOMPARE(f.model.background(), QString("auto")); // back to the format's own: white
        f.model.setBackground("transparent");
        QCOMPARE(f.model.background(), QString("auto"));
        f.model.setBackground("colour");
        f.model.setColour("not a colour");
        QTRY_VERIFY(!f.model.problem().isEmpty());
        f.model.setColour("#336699");
        QTRY_VERIFY(f.model.canExport());
        f.model.setFormat("ora");
        QVERIFY(!f.model.hasBackground()); // layers kept: nothing is laid under them
    }

    void writesTheShownPictureAndNeverReplacesAFile() {
        Fixture f;
        f.model.open(f.id, "", "picture");
        f.model.setFormat("jpg");
        f.model.setQuality(50);
        QTRY_VERIFY(f.model.canExport());
        const QString target = f.dir.filePath("out");
        QVERIFY(QDir().mkpath(target));
        QVERIFY(f.model.exportTo(QUrl::fromLocalFile(target + "/shot").toString())); // no extension: the format's is added
        QVERIFY(QFile::exists(target + "/shot.jpg"));
        QCOMPARE(f.model.notice(), QString("Saved shot.jpg"));
        QFile written(target + "/shot.jpg");
        QVERIFY(written.open(QIODevice::ReadOnly));
        const qint64 size = written.size();
        QVERIFY(f.model.estimate().startsWith(QLocale().formattedDataSize(size)));
        written.close();
        // The same name again is refused and says why; the file is as it was.
        QVERIFY(!f.model.exportTo(target + "/shot.jpg"));
        QVERIFY(!f.model.error().isEmpty());
        QVERIFY(f.model.notice().isEmpty());
        QCOMPARE(QFileInfo(target + "/shot.jpg").size(), size);
    }

    void layersModeListsTheFilesAndWritesThemIntoAFolder() {
        Fixture f;
        QVERIFY(f.model.open(f.id, "", "layers").isEmpty());
        QCOMPARE(f.model.mode(), QString("layers"));
        QCOMPARE(f.model.prefix(), QString("Harbour _ dawn"));
        f.model.setFormat("jpg"); // layers keep their transparency: png or webp only
        QCOMPARE(f.model.format(), QString("png"));
        f.model.setPrefix("");
        QTRY_VERIFY(f.model.canExport());
        QCOMPARE(f.model.files().size(), 2);
        QCOMPARE(f.model.files().at(0).toMap().value("fileName").toString(), QString("01-Sky.png"));
        QVERIFY(f.model.estimate().startsWith("2 files"));
        f.model.setVisibleOnly(true);
        QTRY_COMPARE(f.model.files().size(), 1); // "Empty" is hidden
        QVERIFY(f.model.estimate().startsWith("1 file ·"));
        const QString folder = f.dir.filePath("layers");
        QTRY_VERIFY(f.model.canExport());
        QVERIFY(f.model.exportTo(folder));
        QVERIFY(QFile::exists(folder + "/01-Sky.png"));
        QVERIFY(!QFile::exists(folder + "/02-Empty.png"));
        QVERIFY(!f.model.exportTo(folder)); // taken: refused, nothing replaced
        QVERIFY(f.model.error().contains("01-Sky.png"));
        f.model.setVisibleOnly(false);
        f.model.setPrefix("b");
        QTRY_VERIFY(f.model.canExport());
        QVERIFY(f.model.exportTo(QUrl::fromLocalFile(folder).toString()));
        QVERIFY(QFile::exists(folder + "/b-02-Empty.png"));
    }

    void anUnknownCanvasSaysWhy() {
        Fixture f;
        QVERIFY(!f.model.open("canvas-000000000000", "", "picture").isEmpty());
        QVERIFY(!f.model.canExport());
        QVERIFY(!f.model.exportTo(f.dir.filePath("x.png")));
    }
};

QTEST_MAIN(TestCanvasExportModel)
#include "test_canvas_export_model.moc"
