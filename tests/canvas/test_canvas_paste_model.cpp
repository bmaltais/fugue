// Pasting and placing a picture in the Canvas view (#236, spec 14.5), the model alone: the clipboard's picture or a dropped file
// becomes a new layer above the current one, as ONE revision (producer canvas-view:paste, history "Layer pasted: Pasted 1"),
// centered where the view says; what is not a picture, or is too big, says why and changes nothing. No window.

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/canvas_view_model.h"
#include "canvas/picture_import.h"
#include "engine/canvas_store.h"

using namespace fugue;
using namespace fugue::mask;

namespace {

MaskPaint solid(int w, int h, Rgba c) {
    MaskPaint m(w, h, 4);
    m.fill(c);
    return m;
}

void putOnClipboard(const QImage &image) {
    QGuiApplication::clipboard()->setImage(image);
    QTRY_VERIFY(QGuiApplication::clipboard()->mimeData()->hasImage());
}

} // namespace

class TestCanvasPasteModel : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_canvas, m_head;

    canvas::CanvasStore store() const { return canvas::CanvasStore(m_dir.filePath("store")); }
    void open(CanvasViewModel &model) {
        model.setStoreRoot(m_dir.filePath("store"));
        model.setCanvasId(m_canvas);
        QTRY_COMPARE(model.revisionB(), m_head);
    }
    int revisionCount() const { return int(store().log(m_canvas).value().size()); }
    static QStringList names(const QVariantList &rows) {
        QStringList out;
        for (const QVariant &row : rows)
            out << row.toMap().value("name").toString();
        return out;
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void init() {
        QDir(m_dir.filePath("store")).removeRecursively();
        PaintDocument d(200, 100);
        d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(200, 100, {30, 60, 200, 255}));
        d.loadLayer(LayerKind::Paint, "Top", true, 1, solid(200, 100, {200, 60, 30, 255}));
        const auto made = store().create("Paste", d, "new");
        QVERIFY(made);
        m_canvas = made->id;
        m_head = made->head;
    }

    void theClipboardsPictureIsOneRevisionAboveTheCurrentLayer() {
        CanvasViewModel model;
        open(model);
        model.selectLayer(0); // Base
        QImage image(40, 20, QImage::Format_RGBA8888);
        image.fill(QColor(0, 255, 0));
        putOnClipboard(image);
        const int before = revisionCount();
        QVERIFY(model.pasteLayer(50, 40));
        QCOMPARE(revisionCount(), before + 1);
        QCOMPARE(names(model.layers()), (QStringList{"Top", "Pasted 1", "Base"})); // top first
        QCOMPARE(model.selection(), (QVariantList{1})); // the new layer is the selection
        const auto head = store().info(m_canvas)->head;
        QCOMPARE(store().revision(m_canvas, head)->producer, QStringLiteral("canvas-view:paste"));
        QCOMPARE(model.stepName(head), QStringLiteral("Layer pasted: Pasted 1"));
        // Centered on (50, 40): the picture's corner is at (30, 30).
        const PaintDocument doc = store().load(m_canvas, head).value();
        const MaskPaint *pixels = doc.pixels(doc.layer(1).id);
        QCOMPARE(int(pixels->rgbaAt(30, 30).g), 255);
        QCOMPARE(int(pixels->rgbaAt(29, 30).a), 0);
        // Undo takes it back.
        QVERIFY(model.undo());
        QCOMPARE(names(model.layers()), (QStringList{"Top", "Base"}));
        // The next paste is Pasted 1 again (the first one is undone) and goes on top when nothing is picked.
        model.selectNone();
        QVERIFY(model.pasteLayer());
        QCOMPARE(names(model.layers()).first(), QStringLiteral("Pasted 1"));
    }

    void withNoPictureOnTheClipboardNothingChangesAndItSaysWhy() {
        CanvasViewModel model;
        open(model);
        QGuiApplication::clipboard()->setText("not a picture");
        QTRY_VERIFY(!QGuiApplication::clipboard()->mimeData()->hasImage());
        QVERIFY(!model.pasteLayer());
        QVERIFY(model.error().contains("no picture"));
        QCOMPARE(revisionCount(), 1);
    }

    void aDroppedFileIsPlacedNamedAfterItAndNotAPictureIsRefused() {
        CanvasViewModel model;
        open(model);
        QImage image(60, 30, QImage::Format_RGBA8888);
        image.fill(QColor(255, 0, 255));
        QVERIFY(image.save(m_dir.filePath("holiday.png")));
        QVERIFY(model.isPictureFile(QUrl::fromLocalFile(m_dir.filePath("holiday.png")).toString()));
        QVERIFY(!model.isPictureFile("/tmp/notes.txt"));
        QVERIFY(model.placeFile(QUrl::fromLocalFile(m_dir.filePath("holiday.png")).toString(), 100, 50));
        QCOMPARE(names(model.layers()).first(), QStringLiteral("holiday"));
        const auto head = store().info(m_canvas)->head;
        QCOMPARE(model.stepName(head), QStringLiteral("Layer pasted: holiday"));
        QFile text(m_dir.filePath("notes.txt"));
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("hello");
        text.close();
        QVERIFY(!model.placeFile(m_dir.filePath("notes.txt")));
        QVERIFY(model.error().contains("not a picture file"));
        // A file that says it is a picture and is not.
        QFile fake(m_dir.filePath("fake.png"));
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write("this is not a png");
        fake.close();
        QVERIFY(!model.placeFile(m_dir.filePath("fake.png")));
        QVERIFY(model.error().contains("could not be read"));
        QCOMPARE(revisionCount(), 2);
    }

    void pictureImportCapsSizeBeforeDecoding() {
        QVERIFY(!pictureFromImage(QImage()).ok());
        QVERIFY(pictureFromImage(QImage(10, 10, QImage::Format_ARGB32)).ok());
        const ImportedPicture huge = pictureFromFile(m_dir.filePath("absent.png"));
        QVERIFY(!huge.ok());
        // A PNG header that claims 40000 x 40000 is refused by its header, never decoded.
        QByteArray png;
        {
            QImage tiny(1, 1, QImage::Format_RGBA8888);
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            tiny.save(&buffer, "PNG");
        }
        auto put32 = [&png](int at, quint32 v) { for (int i = 0; i < 4; ++i) png[at + i] = char((v >> (24 - 8 * i)) & 0xff); };
        put32(16, 40000);
        put32(20, 40000);
        QFile file(m_dir.filePath("huge.png"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(png);
        file.close();
        const ImportedPicture refused = pictureFromFile(m_dir.filePath("huge.png"));
        QVERIFY(!refused.ok());
    }

    // ---- #236 slice 2: transforms ----
    void aTransformIsOneRevisionNamedForWhatItDid() {
        CanvasViewModel model;
        open(model);
        QImage image(40, 20, QImage::Format_RGBA8888);
        image.fill(QColor(0, 255, 0));
        putOnClipboard(image);
        QVERIFY(model.pasteLayer(100, 50));
        const int pasted = model.layers().first().toMap().value("position").toInt();
        const QVariantMap box = model.layerBox(pasted);
        QVERIFY(box.value("valid").toBool());
        QVERIFY(box.value("original").toBool());
        QCOMPARE(box.value("width").toInt(), 40);
        QCOMPARE(box.value("x").toInt(), 80);
        QCOMPARE(box.value("corners").toList().size(), 4);
        int count = revisionCount();
        QVERIFY(model.moveLayerBy(pasted, 10, -5));
        QCOMPARE(revisionCount(), count + 1);
        QCOMPARE(model.stepName(store().info(m_canvas)->head), QStringLiteral("Layer moved: Pasted 1"));
        QCOMPARE(store().revision(m_canvas, store().info(m_canvas)->head)->producer, QStringLiteral("canvas-view:transform"));
        QCOMPARE(model.layerBox(pasted).value("x").toInt(), 90);
        QVERIFY(model.transformLayer(pasted, QVariantMap{{"width", 80}, {"height", 40}}));
        QCOMPARE(model.stepName(store().info(m_canvas)->head), QStringLiteral("Layer scaled: Pasted 1"));
        QVERIFY(model.turnLayer(pasted, true));
        QCOMPARE(model.stepName(store().info(m_canvas)->head), QStringLiteral("Layer rotated: Pasted 1"));
        QCOMPARE(model.layerBox(pasted).value("angle").toDouble(), 90.0);
        QVERIFY(model.flipLayer(pasted, true));
        QCOMPARE(model.stepName(store().info(m_canvas)->head), QStringLiteral("Layer flipped: Pasted 1"));
        QVERIFY(model.transformLayer(pasted, QVariantMap{{"angle", 30}, {"width", 60}}));
        QCOMPARE(model.stepName(store().info(m_canvas)->head), QStringLiteral("Layer transformed: Pasted 1"));
        // Undo goes back one step at a time.
        count = revisionCount();
        QVERIFY(model.undo());
        QCOMPARE(model.layerBox(pasted).value("angle").toDouble(), 90.0 * -1);
    }

    void aFirstTransformOfAPlainLayerStartsFromItsPixelsAndRefusalsAreInWords() {
        CanvasViewModel model;
        open(model);
        // Top is a whole-canvas layer: its box is the canvas, and moving it is one revision.
        const QVariantMap box = model.layerBox(1);
        QVERIFY(box.value("valid").toBool());
        QVERIFY(!box.value("original").toBool());
        QCOMPARE(box.value("width").toInt(), 200);
        QVERIFY(model.moveLayerBy(1, 20, 0));
        QCOMPARE(model.stepName(store().info(m_canvas)->head), QStringLiteral("Layer moved: Top"));
        QVERIFY(model.layerBox(1).value("original").toBool());
        QVERIFY(!model.layerBox(9).value("valid").toBool());
        QVERIFY(!model.transformLayer(1, QVariantMap{{"width", 0}}));
        QVERIFY(model.error().contains("cannot be placed"));
        QVERIFY(!model.transformLayer(1, QVariantMap{{"filter", "sinc"}}));
        // A group is refused with a plain message.
        QVERIFY(model.groupLayers(QVariantList{1}));
        const int groupPosition = model.layers().first().toMap().value("position").toInt();
        const QVariantMap groupBox = model.layerBox(groupPosition);
        QVERIFY(!groupBox.value("valid").toBool());
        QVERIFY(groupBox.value("reason").toString().contains("group"));
        QVERIFY(!model.flipLayer(groupPosition, true));
        QVERIFY(model.error().contains("group"));
    }
};

QTEST_MAIN(TestCanvasPasteModel)
#include "test_canvas_paste_model.moc"
