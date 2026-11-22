// The paint window's canvas item (#181, spec 14.4): PaintLayer, the model a window draws and drives.
//
// Seam: canvas/paint_layer.h as QML calls it, with no window: strokes, shapes and layer calls in picture
// pixels, undo across layers, the .ora it stores and reads back. The drawing is looked at in
// test_canvas_paint_editor.cpp; the layers' own rules are tests/engine/test_paint_document.cpp.

#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/paint_layer.h"
#include "engine/paint_file.h"
#include "engine/vips.h"

using fugue::PaintLayer;

namespace {

QString writePicture(const QString &path, int width, int height, QRgb color) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    image.save(path, "PNG");
    return path;
}

int idOf(const PaintLayer &layer, const QString &name) {
    for (const QVariant &v : layer.layers())
        if (v.toMap().value("name").toString() == name)
            return v.toMap().value("id").toInt();
    return 0;
}

QStringList names(const PaintLayer &layer) {
    QStringList out;
    for (const QVariant &v : layer.layers())
        out << v.toMap().value("name").toString();
    return out;
}

} // namespace

class TestPaintLayer : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(fugue::initVips()); }

    void aNewCanvasIsAPictureAndAnEmptyPaintLayerWithNoHistory() {
        PaintLayer layer;
        layer.setImageSize({64, 40});
        QCOMPARE(layer.imageSize(), QSize(64, 40));
        QCOMPARE(layer.layers().size(), 2);
        // Top first: the paint layer to paint on, active; the picture under it.
        QCOMPARE(layer.layers().first().toMap().value("kind").toString(), QString("paint"));
        QVERIFY(layer.layers().first().toMap().value("active").toBool());
        QCOMPARE(layer.layers().last().toMap().value("kind").toString(), QString("picture"));
        QVERIFY(layer.canPaint());
        QVERIFY(layer.isEmpty());
        QVERIFY(!layer.canUndo());
    }

    void aColorStrokeIsOneUndoStepAndHonoursTheBrush() {
        PaintLayer layer;
        layer.setImageSize({64, 40});
        layer.setColor(QColor(200, 20, 20));
        layer.beginStroke(false, 10, 1, 10, 20);
        layer.strokeTo(40, 20);
        layer.endStroke();
        QVERIFY(!layer.isEmpty());
        QCOMPARE(layer.valueAt(25, 20), 255);
        QCOMPARE(layer.colorAt(25, 20), QColor(200, 20, 20));
        QCOMPARE(layer.valueAt(25, 5), 0);
        QVERIFY(layer.canUndo());
        layer.undo();
        QVERIFY(layer.isEmpty());
        QVERIFY(layer.canRedo());
        layer.redo();
        QCOMPARE(layer.colorAt(25, 20), QColor(200, 20, 20));
        // The eraser takes it back; the brush opacity is the strength.
        layer.beginStroke(true, 10, 1, 10, 20);
        layer.endStroke();
        QCOMPARE(layer.valueAt(10, 20), 0);
        layer.setBrushOpacity(0.5);
        layer.beginStroke(false, 6, 1, 50, 30);
        layer.endStroke();
        QVERIFY(qAbs(layer.valueAt(50, 30) - 128) <= 1);
    }

    void shapesFillInvertAndClearTheActiveLayer() {
        PaintLayer layer;
        layer.setImageSize({40, 40});
        layer.setColor(QColor(0, 0, 255));
        layer.fillRect(5, 5, 10, 10, false);
        QCOMPARE(layer.colorAt(8, 8), QColor(0, 0, 255));
        layer.fillPolygon({QVariantMap{{"x", 20}, {"y", 20}}, QVariantMap{{"x", 38}, {"y", 20}}, QVariantMap{{"x", 38}, {"y", 38}}}, false);
        QCOMPARE(layer.valueAt(35, 30), 255);
        layer.fillRect(5, 5, 10, 10, true);
        QCOMPARE(layer.valueAt(8, 8), 0);
        layer.clear();
        QVERIFY(layer.isEmpty());
        layer.fill();
        QCOMPARE(layer.valueAt(1, 1), 255);
        QCOMPARE(layer.colorAt(1, 1), QColor(0, 0, 255));
    }

    void theStackAddsRemovesMovesAndUndoesInOneHistory() {
        PaintLayer layer;
        layer.setImageSize({32, 32});
        const int paint = layer.activeLayer();
        const int mask = layer.addLayer("mask");
        QVERIFY(mask > 0);
        QCOMPARE(layer.activeLayer(), mask);
        QCOMPARE(layer.layers().first().toMap().value("kind").toString(), QString("mask"));
        QVERIFY(layer.canUndo());
        // Paint on the mask layer: it is a value, white; the color is ignored.
        layer.setColor(QColor(10, 200, 10));
        layer.fillRect(0, 0, 8, 8, false);
        QCOMPARE(layer.valueAt(4, 4), 255);
        QCOMPARE(layer.colorAt(4, 4).alpha(), 0); // the mask is not in the picture
        // Move it down under the paint layer, and select the paint layer.
        QVERIFY(layer.moveLayer(mask, -1));
        QCOMPARE(layer.layers().at(1).toMap().value("id").toInt(), mask);
        QVERIFY(!layer.moveLayer(mask, -5));
        QVERIFY(layer.selectLayer(paint));
        // Remove the paint layer; undo brings it back; the picture layer cannot go.
        const int picture = layer.layers().last().toMap().value("id").toInt();
        QVERIFY(!layer.removeLayer(picture));
        layer.fillRect(0, 0, 4, 4, false);
        QVERIFY(layer.removeLayer(paint));
        QCOMPARE(layer.layers().size(), 3 - 1);
        layer.undo();
        QCOMPARE(layer.layers().size(), 3);
        QCOMPARE(layer.valueAt(1, 1), 255); // the paint came back with it
        QCOMPARE(layer.activeLayer(), paint);
    }

    void thePictureIsDrawnButNeverPaintedOn() {
        QTemporaryDir files;
        const QString photo = writePicture(files.filePath("photo.png"), 30, 20, qRgb(10, 20, 30));
        PaintLayer layer;
        layer.setImageSize({30, 20});
        QVERIFY(layer.setPicture(photo));
        QVERIFY(layer.hasPicture());
        QCOMPARE(layer.colorAt(3, 3), QColor(10, 20, 30));
        QVERIFY(layer.isEmpty()); // the picture alone is nothing painted
        const int picture = layer.layers().last().toMap().value("id").toInt();
        QVERIFY(layer.selectLayer(picture));
        QVERIFY(!layer.canPaint());
        QCOMPARE(layer.valueAt(3, 3), -1);
        layer.beginStroke(false, 8, 1, 10, 10);
        layer.endStroke();
        QVERIFY(layer.isEmpty());
        QVERIFY(!layer.canUndo());
        // Hiding it is a change to the result, so the canvas is no longer "nothing painted".
        QVERIFY(layer.setLayerVisible(picture, false));
        QVERIFY(!layer.isEmpty());
        QCOMPARE(layer.colorAt(3, 3).alpha(), 0);
        QVERIFY(layer.setLayerVisible(picture, true));
        QVERIFY(layer.isEmpty());
        // A picture that is not there says so.
        QVERIFY(!layer.setPicture(files.filePath("gone.png")));
        QVERIFY(!layer.lastError().isEmpty());
    }

    void storeWritesOneOraThatLoadsBackAndNothingForADifferentCanvas() {
        QTemporaryDir files;
        PaintLayer layer;
        layer.setFolder(files.filePath("inputs"));
        layer.setImageSize({48, 32});
        layer.setColor(QColor(255, 128, 0));
        layer.fillRect(4, 4, 20, 10, false);
        const int mask = layer.addLayer("mask");
        layer.fillRect(30, 20, 10, 8, false);
        QVERIFY(layer.setLayerName(mask, "Sky"));
        QVERIFY(layer.setLayerOpacity(mask, 0.5));
        const QString path = layer.store("photo-paint");
        QVERIFY2(QFileInfo(path).isFile(), qPrintable(layer.lastError()));
        QVERIFY(path.startsWith(files.filePath("inputs") + "/photo-paint-"));
        QVERIFY(path.endsWith(".ora"));
        QCOMPARE(layer.store("other-name"), path); // the same layers are the same file

        PaintLayer again;
        again.setImageSize({48, 32});
        QVERIFY2(again.loadPaint(path), qPrintable(again.lastError()));
        QVERIFY(again.warning().isEmpty());
        QCOMPARE(again.colorAt(10, 8), QColor(255, 128, 0));
        QCOMPARE(names(again), QStringList({"Sky", "Layer 1", "Picture"}));
        QVERIFY(!again.canUndo()); // loading is not a step
        QCOMPARE(again.layers().first().toMap().value("opacity").toDouble(), 0.5);
        QCOMPARE(again.activeLayer(), idOf(again, "Sky")); // the top layer, a mask

        // Another canvas size scales it and says so; a file that is not one is refused.
        PaintLayer bigger;
        bigger.setImageSize({96, 64});
        QVERIFY(bigger.loadPaint(QUrl::fromLocalFile(path).toString()));
        QVERIFY(bigger.warning().contains("48 x 32"));
        QCOMPARE(bigger.colorAt(20, 16), QColor(255, 128, 0));
        QFile junk(files.filePath("junk.ora"));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("not a zip");
        junk.close();
        QVERIFY(!again.loadPaint(files.filePath("junk.ora")));
        QVERIFY(!again.lastError().isEmpty());
        QVERIFY(!again.loadPaint(files.filePath("missing.ora")));
    }

    void aFileThatEndsOnThePictureStillHasALayerToPaintOn() {
        QTemporaryDir files;
        fugue::mask::PaintDocument doc(16, 16);
        doc.loadLayer(fugue::mask::LayerKind::Paint, "Under", true, 1, fugue::mask::MaskPaint(16, 16, 4));
        doc.loadLayer(fugue::mask::LayerKind::Picture, "Picture", true, 1, fugue::mask::MaskPaint(1, 1, 4));
        QVERIFY(fugue::mask::writePaintFile(doc, files.filePath("a.ora")).isEmpty());
        PaintLayer layer;
        layer.setImageSize({16, 16});
        QVERIFY(layer.loadPaint(files.filePath("a.ora")));
        QVERIFY(layer.canPaint());
        QCOMPARE(layer.activeLayer(), idOf(layer, "Under"));
    }

    void theSameCallsAsMaskLayerWorkOnEitherOne() {
        // The editor drives both through these names: a missing one is a QML error at run time.
        const QMetaObject &paint = PaintLayer::staticMetaObject;
        for (const char *name : {"beginStroke", "strokeTo", "endStroke", "fillPolygon", "fillRect", "invert", "clear", "fill", "undo",
                                  "redo", "valueAt", "png", "store", "lastError", "uploadedCells", "reset"})
            QVERIFY2(paint.indexOfMethod(QMetaObject::normalizedSignature((QByteArray(name) + "()").constData())) >= 0 ||
                         [&] {
                             for (int i = paint.methodOffset(); i < paint.methodCount(); ++i)
                                 if (paint.method(i).name() == name)
                                     return true;
                             return false;
                         }(),
                     name);
        for (const char *name : {"imageSize", "originX", "originY", "zoom", "tint", "maskOnly", "canUndo", "canRedo", "empty", "folder"})
            QVERIFY2(paint.indexOfProperty(name) >= 0, name);
        QVERIFY(paint.indexOfSignal("edited()") >= 0);
    }
};

QTEST_MAIN(TestPaintLayer)
#include "test_paint_layer.moc"
