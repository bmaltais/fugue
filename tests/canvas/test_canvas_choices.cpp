// The list of canvases the `canvas` widget offers (#211, epic #206, spec 14.5): the rows of one store with
// name, size and time of the last change, a blank canvas or one from a picture made into the same store,
// refusals that say why, and a refresh when another process writes to the store. Seam: canvas/canvas_choices.h
// with a folder of the test's own; no window.

#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/canvas_choices.h"
#include "engine/canvas_store.h"
#include "engine/clock.h"
#include "engine/paint_file.h"

using namespace fugue;

class TestCanvasChoices : public QObject {
    Q_OBJECT
private slots:
    void anEmptyStoreHasNoRows() {
        QTemporaryDir dir;
        CanvasChoices choices;
        choices.setStoreRoot(dir.filePath("store"));
        QVERIFY(choices.rows().isEmpty());
        QVERIFY(choices.row("canvas-0123456789ab").isEmpty());
    }

    void aBlankCanvasHasANameASizeAndATime() {
        QTemporaryDir dir;
        CanvasChoices choices;
        choices.setStoreRoot(dir.filePath("store"));
        QString id;
        QCOMPARE(choices.makeBlank("Sketch", 800, 600, &id), QString());
        QVERIFY(canvas::isCanvasId(id));
        const QVariantMap row = choices.row(id);
        QCOMPARE(row.value("name").toString(), QString("Sketch"));
        QCOMPARE(row.value("width").toInt(), 800);
        QCOMPARE(row.value("height").toInt(), 600);
        QCOMPARE(row.value("size").toString(), QString("800 × 600"));
        QVERIFY(!row.value("changedText").toString().isEmpty());
        QCOMPARE(row.value("changed").toString(), canvas::CanvasStore(dir.filePath("store")).revision(id, row.value("head").toString())->time);
        // One empty paint layer, as `fugue canvas new` makes.
        const Result<mask::PaintDocument> loaded = canvas::CanvasStore(dir.filePath("store")).load(id, row.value("head").toString());
        QVERIFY(loaded);
        QCOMPARE(loaded->layerCount(), 1);
    }

    void aBlankNameIsUntitledAndSizesAreHeldToTheLimits() {
        QTemporaryDir dir;
        CanvasChoices choices;
        choices.setStoreRoot(dir.filePath("store"));
        QString id;
        QCOMPARE(choices.makeBlank("  ", 64, 64, &id), QString());
        QCOMPARE(choices.row(id).value("name").toString(), QString("Untitled"));
        for (const QSize bad : {QSize(0, 100), QSize(100, 0), QSize(16385, 10), QSize(10, 16385), QSize(9000, 9000), QSize(-5, 5)}) {
            QString none;
            const QString why = choices.makeBlank("x", bad.width(), bad.height(), &none);
            QVERIFY2(why.contains("at most 16384 px a side and 67 megapixels"), qPrintable(why));
            QVERIFY(none.isEmpty());
        }
        QCOMPARE(choices.rows().size(), 1);
        QCOMPARE(choices.makeBlank("edge", 8192, 8192, &id), QString()); // 64 MP exactly
    }

    void aCanvasFromAPictureTakesItsSizeAndItsNameUnlessGiven() {
        QTemporaryDir dir;
        const QString picture = dir.filePath("holiday.png");
        QImage image(320, 200, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        QVERIFY(image.save(picture));
        CanvasChoices choices;
        choices.setStoreRoot(dir.filePath("store"));
        QCOMPARE(choices.pictureSize(picture), QString("320 × 200"));
        QCOMPARE(choices.pictureSize(dir.filePath("nope.png")), QString());
        QString id;
        QCOMPARE(choices.makeFromFile(QUrl::fromLocalFile(picture).toString(), "", &id), QString());
        QCOMPARE(choices.row(id).value("name").toString(), QString("holiday"));
        QCOMPARE(choices.row(id).value("size").toString(), QString("320 × 200"));
        QString named;
        QCOMPARE(choices.makeFromFile(picture, "Beach", &named), QString());
        QCOMPARE(choices.row(named).value("name").toString(), QString("Beach"));
    }

    void aFileThatIsNotAPictureSaysWhyAndMakesNothing() {
        QTemporaryDir dir;
        QFile file(dir.filePath("notes.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a picture");
        file.close();
        CanvasChoices choices;
        choices.setStoreRoot(dir.filePath("store"));
        QString id;
        const QString why = choices.makeFromFile(file.fileName(), "", &id);
        QVERIFY(!why.isEmpty());
        QVERIFY(id.isEmpty());
        QCOMPARE(choices.makeFromFile(dir.filePath("missing.png"), "", &id).isEmpty(), false);
        QVERIFY(choices.rows().isEmpty());
    }

    void rowsAreNewestFirstAndAnotherRootsCanvasIsNotInTheList() {
        QTemporaryDir dir;
        // Two roots, as the Default and a project are.
        canvas::CanvasStore other(dir.filePath("project"));
        mask::PaintDocument doc(10, 10);
        doc.addLayer(mask::LayerKind::Paint, "L");
        const Result<canvas::CanvasInfo> foreign = other.create("Elsewhere", doc, "new");
        QVERIFY(foreign);
        CanvasChoices choices;
        choices.setStoreRoot(dir.filePath("store"));
        QString first, second;
        QCOMPARE(choices.makeBlank("One", 32, 32, &first), QString());
        QTest::qWait(1100); // a creation time has one-second resolution in the id order tie-break
        QCOMPARE(choices.makeBlank("Two", 32, 32, &second), QString());
        const QVariantList rows = choices.rows();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).toMap().value("id").toString(), second);
        QCOMPARE(rows.at(1).toMap().value("id").toString(), first);
        QVERIFY(choices.row(foreign->id).isEmpty());
    }

    void theListIsToldWhenAnotherProcessWritesToTheStore() {
        QTemporaryDir dir;
        const QString root = dir.filePath("store");
        CanvasChoices choices;
        choices.setStoreRoot(root);
        QSignalSpy changed(&choices, &CanvasChoices::changed);
        QVERIFY(choices.rows().isEmpty());
        // A folder that does not exist yet is not made just to be watched.
        QVERIFY(!QDir(root).exists());
        mask::PaintDocument doc(64, 48);
        doc.addLayer(mask::LayerKind::Paint, "L");
        canvas::CanvasStore writer(root); // `fugue canvas new`, another process
        const Result<canvas::CanvasInfo> made = writer.create("From the CLI", doc, "new");
        QVERIFY(made);
        // Not watched yet (the folder did not exist), so asking arms it and sees the canvas.
        QCOMPARE(choices.rows().size(), 1);
        // From now on a change is signalled: a new canvas, then a new head on it.
        changed.clear();
        const Result<canvas::CanvasInfo> second = writer.create("Second", doc, "new");
        QVERIFY(second);
        QTRY_VERIFY(!changed.isEmpty());
        changed.clear();
        doc.brush.color = mask::Rgba{255, 0, 0, 255};
        doc.fillRect(0, 0, 10, 10);
        QVERIFY(writer.commit(made->id, doc, writer.info(made->id)->head, "edit"));
        QTRY_VERIFY(!changed.isEmpty());
        QCOMPARE(choices.row(made->id).value("changed").toString(), writer.revision(made->id, writer.info(made->id)->head)->time);
    }

    void aCanvasMadeHereIsSignalled() {
        QTemporaryDir dir;
        CanvasChoices choices;
        choices.setStoreRoot(dir.filePath("store"));
        QSignalSpy changed(&choices, &CanvasChoices::changed);
        const int before = choices.revision();
        QString id;
        QCOMPARE(choices.makeBlank("Fresh", 64, 64, &id), QString());
        QTRY_VERIFY(choices.revision() > before);
        QVERIFY(!changed.isEmpty());
    }
};

QTEST_MAIN(TestCanvasChoices)
#include "test_canvas_choices.moc"
