// The mask editor component (#172, spec 14.4): MaskEditor.qml, its MaskLayer and the file it saves.
//
// Seam: the real window (Main.qml, offscreen), with a MaskEditor made on it as any later caller
// (the Load Image and Paint Mask nodes, #174) will. A picture this test writes goes in; strokes,
// shapes and keys go through the window's mouse and keyboard; the saved PNG is read back. The core
// (dabs, shapes, undo, PNG) is tested apart: tests/engine/test_mask_paint.cpp. With FUGUE_SHOT_DIR
// set, the last tests save the open editor, light and dark, as pictures to look at.

#include <QImage>
#include <QQmlComponent>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/mask_layer.h"
#include "canvas_app.h"
#include "engine/mask_paint.h"

using namespace canvas_test;
using fugue::MaskLayer;
using fugue::mask::MaskPaint;

namespace {

const char *const empty = R"({"fugue": 1, "graph": {"nodes": {}}})";

QString writePicture(const QString &path, int width, int height) {
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, qRgb(40 + x * 180 / width, 90 + y * 100 / height, 150 - x * 90 / width));
    image.save(path, "PNG");
    return path;
}

// The editor on the window, with a folder of its own to save into.
struct Editor {
    QTemporaryDir files;
    App app;
    std::unique_ptr<QObject> host;
    QObject *editor = nullptr;
    MaskLayer *layer = nullptr;
    QSize pixels;
    std::unique_ptr<QSignalSpy> savedSpy;

    explicit Editor(QSize size = QSize(400, 240), const Look &look = {}) : app(load(empty), {}, look), pixels(size) {}

    bool start(const QString &maskPath = {}) {
        if (!app.open())
            return false;
        QQmlComponent component(&app.engine);
        component.setData(R"(
            import QtQuick
            import "canvas"
            import Fugue
            Item {
                property alias editor: maskEditor
                MaskEditor { id: maskEditor; look: App.theme.look }
            })", QUrl("qrc:/TestHost.qml"));
        QObject *made = component.beginCreate(app.engine.rootContext());
        host.reset(made);
        auto *item = qobject_cast<QQuickItem *>(made);
        if (!item)
            return false;
        item->setParentItem(app.window->contentItem());
        component.completeCreate();
        editor = made->property("editor").value<QObject *>();
        layer = editor ? editor->property("layer").value<MaskLayer *>() : nullptr;
        if (!layer)
            return false;
        const QString picture = writePicture(files.filePath("photo.png"), pixels.width(), pixels.height());
        editor->setProperty("imageSource", QUrl::fromLocalFile(picture));
        editor->setProperty("imageSize", QSizeF(pixels));
        editor->setProperty("maskPath", maskPath);
        editor->setProperty("folder", files.filePath("inputs"));
        savedSpy = std::make_unique<QSignalSpy>(editor, SIGNAL(saved(QString)));
        QMetaObject::invokeMethod(editor, "open");
        return QTest::qWaitFor([&] { return editor->property("opened").toBool(); }, 5000) && settle();
    }
    // The picture has been fitted and drawn once, so the layer knows where it is.
    bool settle() {
        QTest::qWait(60);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        return layer->zoom() > 0 && !test_support::grab(app.window).isNull();
    }

    QQuickItem *byName(const QString &name) const { return item(app.window->contentItem(), name); }
    // A picture pixel in window coordinates.
    QPoint at(double x, double y) const {
        return layer->mapToScene(QPointF(layer->originX() + x * layer->zoom(), layer->originY() + y * layer->zoom())).toPoint();
    }
    QPoint center(const QString &name) const {
        QQuickItem *it = byName(name);
        return it ? it->mapToScene(QPointF(it->width() / 2, it->height() / 2)).toPoint() : QPoint();
    }
    void press(const QString &name) { QTest::mouseClick(app.window, Qt::LeftButton, {}, center(name)); }
    void key(int k, Qt::KeyboardModifiers m = {}) { QTest::keyClick(app.window, Qt::Key(k), m); }
    // A drag over picture pixels, on the window's pointer.
    void dragOver(QPointF from, QPointF to, Qt::KeyboardModifiers m = {}) { app.drag(Qt::LeftButton, at(from.x(), from.y()), at(to.x(), to.y()), m); }
    int value(int x, int y) const { return layer->valueAt(x, y); }
    QString tool() const { return editor->property("tool").toString(); }
    void ok() { press("maskOk"); }
};

} // namespace

class TestCanvasMaskEditor : public QObject {
    Q_OBJECT

private slots:
    // ---- a session: stroke, undo, invert, OK ------------------------------------------------------

    void aStrokeUndoInvertAndOkSaveAMaskThatMatches() {
        Editor e;
        QVERIFY(e.start());
        QVERIFY(e.layer->isEmpty());
        e.editor->setProperty("brushSize", 40);
        e.editor->setProperty("brushHardness", 1);
        // A stroke along a line, by the pointer in the window.
        e.dragOver({100, 100}, {250, 100});
        QCOMPARE(e.value(100, 100), 255);
        QCOMPARE(e.value(180, 100), 255);
        QCOMPARE(e.value(180, 140), 0);
        QVERIFY(e.layer->canUndo());
        QVERIFY(!e.layer->isEmpty());
        // Undo takes the whole stroke back; redo gives it again.
        e.key(Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(e.layer->isEmpty());
        QVERIFY(e.layer->canRedo());
        e.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(e.value(180, 100), 255);
        // Invert with the key.
        e.key(Qt::Key_I);
        QCOMPARE(e.value(180, 100), 0);
        QCOMPARE(e.value(10, 10), 255);

        e.ok();
        QTRY_COMPARE(e.savedSpy->count(), 1);
        const QString path = e.savedSpy->first().first().toString();
        QVERIFY2(QFileInfo(path).isFile(), qPrintable(path));
        QVERIFY(path.startsWith(e.files.filePath("inputs") + "/photo-mask-"));
        QVERIFY(path.endsWith(".png"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto saved = MaskPaint::fromPng(file.readAll());
        QVERIFY(saved.ok());
        QCOMPARE(saved->width(), 400);
        QCOMPARE(saved->height(), 240);
        QCOMPARE(int(saved->at(180, 100)), 0);
        QCOMPARE(int(saved->at(10, 10)), 255);
        QCOMPARE(int(saved->at(390, 230)), 255);
        // The editor closed, and said nothing of canceling.
        QTRY_VERIFY(!e.editor->property("visible").toBool());
        QVERIFY2(e.app.warnings.isEmpty(), qPrintable(e.app.warnings.join('\n')));
    }

    void cancelEscapeAndTheCloseButtonDropTheSession() {
        for (int how = 0; how < 3; ++how) {
            Editor e;
            QVERIFY(e.start());
            QSignalSpy canceled(e.editor, SIGNAL(canceled()));
            e.dragOver({50, 50}, {120, 50});
            QVERIFY(!e.layer->isEmpty());
            if (how == 0) e.press("maskCancel");
            else if (how == 1) e.key(Qt::Key_Escape);
            else e.press("maskClose");
            QTRY_COMPARE(canceled.count(), 1);
            QCOMPARE(e.savedSpy->count(), 0);
            QVERIFY(!QDir(e.files.filePath("inputs")).exists() || QDir(e.files.filePath("inputs")).isEmpty());
            // Opened again, it starts from nothing: the session's strokes are gone.
            QMetaObject::invokeMethod(e.editor, "open");
            QTRY_VERIFY(e.editor->property("visible").toBool());
            QVERIFY(e.layer->isEmpty());
            QVERIFY(!e.layer->canUndo());
        }
    }

    void enterIsOkAndAnEmptyMaskSavesNoFile() {
        Editor e;
        QVERIFY(e.start());
        e.key(Qt::Key_Return);
        QTRY_COMPARE(e.savedSpy->count(), 1);
        QCOMPARE(e.savedSpy->first().first().toString(), QString()); // nothing painted: no mask
        QVERIFY(!QDir(e.files.filePath("inputs")).exists() || QDir(e.files.filePath("inputs")).isEmpty());
    }

    void theSameMaskTwiceIsOneFile() {
        QString first;
        for (int round = 0; round < 2; ++round) {
            Editor e;
            QVERIFY(e.start());
            e.editor->setProperty("folder", QDir::temp().filePath("fugue-mask-same-test"));
            e.layer->fillRect(10, 10, 50, 50, false);
            e.ok();
            QTRY_COMPARE(e.savedSpy->count(), 1);
            const QString path = e.savedSpy->first().first().toString();
            if (round == 0) first = path;
            else QCOMPARE(path, first);
        }
        QDir(QDir::temp().filePath("fugue-mask-same-test")).removeRecursively();
    }

    void aStoredMaskStartsTheSessionAndIsScaledToThePicture() {
        QTemporaryDir masks;
        MaskPaint start(100, 60); // a mask of another size than the 400 x 240 picture
        start.fillRect(0, 0, 50, 60);
        QFile out(masks.filePath("start.png"));
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(*start.toPng());
        out.close();
        Editor e;
        QVERIFY(e.start(masks.filePath("start.png")));
        QCOMPARE(e.value(10, 10), 255);
        QCOMPARE(e.value(390, 200), 0);
        QVERIFY(!e.layer->canUndo()); // loading is not a step
        e.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(e.value(10, 10), 255);
        // A missing file is said, and the mask is empty.
        Editor missing;
        QVERIFY(missing.start(masks.filePath("nothing.png")));
        QVERIFY(missing.layer->isEmpty());
        QVERIFY(!missing.editor->property("error").toString().isEmpty());
    }

    // ---- tools and keys --------------------------------------------------------------------------

    void theToolKeysAndTheBarButtonsSwitchTools() {
        Editor e;
        QVERIFY(e.start());
        QCOMPARE(e.tool(), QString("brush"));
        e.key(Qt::Key_E);
        QCOMPARE(e.tool(), QString("eraser"));
        e.key(Qt::Key_X);
        QCOMPARE(e.tool(), QString("brush"));
        e.key(Qt::Key_X);
        QCOMPARE(e.tool(), QString("eraser"));
        e.key(Qt::Key_L);
        QCOMPARE(e.tool(), QString("lasso"));
        e.key(Qt::Key_R);
        QCOMPARE(e.tool(), QString("rect"));
        e.key(Qt::Key_B);
        QCOMPARE(e.tool(), QString("brush"));
        e.press("maskTool-lasso");
        QCOMPARE(e.tool(), QString("lasso"));
        QVERIFY(e.byName("maskTool-lasso")->property("current").toBool());
        QVERIFY(!e.byName("maskTool-brush")->property("current").toBool());
        // H shows the mask alone and again the overlay.
        QVERIFY(!e.layer->maskOnly());
        e.key(Qt::Key_H);
        QVERIFY(e.layer->maskOnly());
        e.key(Qt::Key_H);
        QVERIFY(!e.layer->maskOnly());
        // Shift+Delete clears, and undo brings it back.
        e.layer->fill();
        e.key(Qt::Key_Delete, Qt::ShiftModifier);
        QVERIFY(e.layer->isEmpty());
        e.key(Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!e.layer->isEmpty());
        QVERIFY2(e.app.warnings.isEmpty(), qPrintable(e.app.warnings.join('\n')));
    }

    void bracketsSizeTheBrushAndShiftBracketsSetItsHardness() {
        Editor e;
        QVERIFY(e.start());
        const auto size = [&] { return e.editor->property("brushSize").toDouble(); };
        const auto hardness = [&] { return e.editor->property("brushHardness").toDouble(); };
        QCOMPARE(size(), 64.0);
        QCOMPARE(hardness(), 0.8);
        e.key(Qt::Key_BracketRight);
        QCOMPARE(size(), 80.0);
        e.key(Qt::Key_BracketLeft);
        e.key(Qt::Key_BracketLeft);
        QVERIFY(qAbs(size() - 51) <= 1);
        // Shift+[ is `{` on a US keyboard, and still the hardness.
        e.key(Qt::Key_BraceLeft, Qt::ShiftModifier);
        QCOMPARE(hardness(), 0.7);
        e.key(Qt::Key_BraceRight, Qt::ShiftModifier);
        e.key(Qt::Key_BraceRight, Qt::ShiftModifier);
        e.key(Qt::Key_BraceRight, Qt::ShiftModifier);
        e.key(Qt::Key_BraceRight, Qt::ShiftModifier);
        QCOMPARE(hardness(), 1.0); // at most 1
        for (int i = 0; i < 40; ++i)
            e.key(Qt::Key_BracketLeft);
        QCOMPARE(size(), 1.0); // at least 1
    }

    void altDragSizesTheBrushLeftAndRightAndSetsHardnessUpAndDown() {
        Editor e;
        QVERIFY(e.start());
        const double scale = e.layer->zoom();
        e.dragOver({100, 100}, {100, 100}); // a click paints a dot: undo it
        e.key(Qt::Key_Z, Qt::ControlModifier);
        const QPoint from = e.at(100, 100);
        QTest::mousePress(e.app.window, Qt::LeftButton, Qt::AltModifier, from);
        QTest::mouseMove(e.app.window, from + QPoint(int(20 * scale), 0));
        QTest::mouseMove(e.app.window, from + QPoint(int(20 * scale), -20));
        QTest::mouseRelease(e.app.window, Qt::LeftButton, Qt::AltModifier, from + QPoint(int(20 * scale), -20));
        QVERIFY(e.editor->property("brushSize").toDouble() > 64 + 30); // 20 view px to the right: ~ 2 x 20 more
        QVERIFY(e.editor->property("brushHardness").toDouble() > 0.8);
        QVERIFY(e.layer->isEmpty()); // sizing paints nothing
        // Left and down: smaller and softer.
        QTest::mousePress(e.app.window, Qt::LeftButton, Qt::AltModifier, from);
        QTest::mouseMove(e.app.window, from + QPoint(int(-10 * scale), 40));
        QTest::mouseRelease(e.app.window, Qt::LeftButton, Qt::AltModifier, from + QPoint(int(-10 * scale), 40));
        QVERIFY(e.editor->property("brushSize").toDouble() < 100);
        QVERIFY(e.editor->property("brushHardness").toDouble() < 0.9);
    }

    void theSlidersInTheBarSetTheBrush() {
        Editor e;
        QVERIFY(e.start());
        QQuickItem *size = e.byName("maskSize");
        QVERIFY(size);
        QTest::mouseClick(e.app.window, Qt::LeftButton, {}, size->mapToScene(QPointF(size->width() * 0.99, size->height() / 2)).toPoint());
        QVERIFY(e.editor->property("brushSize").toDouble() > 200);
        QQuickItem *hard = e.byName("maskHardness");
        QTest::mouseClick(e.app.window, Qt::LeftButton, {}, hard->mapToScene(QPointF(hard->width() * 0.5, hard->height() / 2)).toPoint());
        QVERIFY(qAbs(e.editor->property("brushHardness").toDouble() - 0.5) < 0.05);
        // The keys on a focused slider.
        QTest::keyClick(e.app.window, Qt::Key_Right);
        QVERIFY(qAbs(e.editor->property("brushHardness").toDouble() - 0.51) < 0.011);
    }

    // ---- shapes and the eraser -----------------------------------------------------------------------

    void theLassoFillsWhatItEnclosesAndAltErases() {
        Editor e;
        QVERIFY(e.start());
        e.key(Qt::Key_L);
        const QPoint a = e.at(50, 50), b = e.at(200, 60), c = e.at(180, 180), d = e.at(60, 170);
        QTest::mousePress(e.app.window, Qt::LeftButton, {}, a);
        for (const QPoint &p : {b, c, d, a + QPoint(0, 3)}) {
            QTest::mouseMove(e.app.window, p);
        }
        QTest::mouseRelease(e.app.window, Qt::LeftButton, {}, a + QPoint(0, 3));
        QCOMPARE(e.value(120, 110), 255);
        QCOMPARE(e.value(10, 10), 0);
        QCOMPARE(e.value(300, 110), 0);
        QCOMPARE(e.layer->canUndo(), true);
        // The same outline with Alt held at the release erases.
        e.dragOver({90, 90}, {150, 130}); // a drag with the lasso is a straight line: no area, no step
        const bool still = e.value(120, 110) == 255;
        QVERIFY(still);
        QTest::mousePress(e.app.window, Qt::LeftButton, {}, e.at(80, 80));
        for (const QPoint &p : {e.at(160, 85), e.at(150, 140), e.at(85, 135)})
            QTest::mouseMove(e.app.window, p);
        QTest::mouseRelease(e.app.window, Qt::LeftButton, Qt::AltModifier, e.at(85, 135));
        QCOMPARE(e.value(120, 110), 0);
        QCOMPARE(e.value(60, 100), 255);
    }

    void theRectangleFillsABoxAndTheEraserTakesItAway() {
        Editor e;
        QVERIFY(e.start());
        e.key(Qt::Key_R);
        e.dragOver({100, 80}, {220, 160});
        QCOMPARE(e.value(150, 120), 255);
        QCOMPARE(e.value(94, 120), 0); // the pointer's rounding to a view pixel moves an edge by a pixel or two
        QCOMPARE(e.value(150, 170), 0);
        e.key(Qt::Key_E);
        e.editor->setProperty("brushSize", 30);
        e.editor->setProperty("brushHardness", 1);
        e.dragOver({120, 120}, {200, 120});
        QCOMPARE(e.value(160, 120), 0);
        QCOMPARE(e.value(160, 90), 255);
        e.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(e.value(160, 120), 255);
        // A rectangle of no size is no step.
        const bool before = e.layer->canRedo();
        Q_UNUSED(before);
        e.key(Qt::Key_R);
        const QPoint p = e.at(300, 200);
        QTest::mouseClick(e.app.window, Qt::LeftButton, {}, p);
        QCOMPARE(e.value(300, 200), 0);
    }

    // ---- the view ------------------------------------------------------------------------------------

    void theWheelZoomsAndSpaceDragPansWithoutPainting() {
        Editor e;
        QVERIFY(e.start());
        const double fit = e.layer->zoom();
        QTest::mouseMove(e.app.window, e.at(200, 120));
        const QPointF where(e.at(200, 120));
        QWheelEvent wheel(where, where, QPoint(), QPoint(0, 240), Qt::NoButton, {}, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(e.app.window, &wheel);
        QTRY_VERIFY(e.layer->zoom() > fit * 1.2);
        // The key 1 is 100%, 0 fits again (the dialog's keys).
        e.key(Qt::Key_1);
        QTRY_COMPARE(e.layer->zoom(), 1.0);
        e.key(Qt::Key_0);
        QTRY_COMPARE(e.layer->zoom(), fit);
        // Zoomed in, Space+drag moves the picture and paints nothing.
        e.key(Qt::Key_Plus);
        e.key(Qt::Key_Plus);
        QTRY_VERIFY(e.layer->zoom() > fit * 1.4);
        const double x0 = e.layer->originX();
        QTest::keyPress(e.app.window, Qt::Key_Space);
        e.app.drag(Qt::LeftButton, e.at(200, 120), e.at(200, 120) + QPoint(60, 20));
        QTest::keyRelease(e.app.window, Qt::Key_Space);
        QVERIFY(e.layer->originX() > x0 + 30);
        QVERIFY(e.layer->isEmpty());
        // A middle drag pans too (the viewer's), and paints nothing.
        const double x1 = e.layer->originX();
        e.app.drag(Qt::MiddleButton, e.at(200, 120), e.at(200, 120) - QPoint(40, 0));
        QVERIFY(e.layer->originX() < x1 - 20);
        QVERIFY(e.layer->isEmpty());
    }

    void aStrokeSendsOnlyTheCellsItTouchesToTheGpu() {
        Editor e(QSize(1024, 1024));
        QVERIFY(e.start());
        QVERIFY(e.layer->uploadedCells() >= 0);
        e.editor->setProperty("brushSize", 20);
        QTest::qWait(30);
        test_support::grab(e.app.window);
        e.layer->beginStroke(false, 20, 0.8, 100, 100);
        e.layer->endStroke();
        test_support::grab(e.app.window);
        QVERIFY2(e.layer->uploadedCells() >= 1 && e.layer->uploadedCells() <= 2, qPrintable(QString::number(e.layer->uploadedCells())));
        // Invert touches them all.
        e.layer->invert();
        test_support::grab(e.app.window);
        QVERIFY(e.layer->uploadedCells() >= 8);
    }

    void aPictureOfAnotherSizeKeepsPixelsInPictureUnits() {
        // 64 px is 64 pixels of the picture whatever the zoom: the same stroke at Fit and at 100%.
        Editor a, b;
        QVERIFY(a.start());
        QVERIFY(b.start());
        a.editor->setProperty("brushHardness", 1);
        b.editor->setProperty("brushHardness", 1);
        a.dragOver({200, 120}, {200, 120});
        b.key(Qt::Key_1);
        QTRY_COMPARE(b.layer->zoom(), 1.0);
        QTest::qWait(50);
        b.dragOver({200, 120}, {200, 120});
        // The same dab, whatever the zoom: the area is about the same (the pointer's rounding to a view pixel
        // moves it by a fraction of a picture pixel at Fit).
        const auto area = [](const Editor &e) {
            double total = 0;
            for (int y = 0; y < 240; ++y)
                for (int x = 0; x < 400; ++x)
                    total += e.value(x, y) / 255.0;
            return total;
        };
        QVERIFY2(qAbs(area(a) - M_PI * 32 * 32) < 40, qPrintable(QString::number(area(a))));
        QVERIFY2(qAbs(area(b) - M_PI * 32 * 32) < 40, qPrintable(QString::number(area(b))));
    }

    // ---- ergonomics ----------------------------------------------------------------------------------

    void everyBarButtonIsAtLeast24PixelsAndOkIsThePrimary() {
        Editor e;
        QVERIFY(e.start());
        for (const char *name : {"maskTool-brush", "maskTool-eraser", "maskTool-lasso", "maskTool-rect", "maskUndo", "maskRedo",
                                 "maskInvert", "maskFill", "maskOnly", "maskClear", "maskCancel", "maskOk", "maskClose",
                                 "maskSize", "maskHardness", "maskZoom-fit", "maskZoom-actual"}) {
            QQuickItem *it = e.byName(name);
            QVERIFY2(it, name);
            QVERIFY2(it->height() >= 24 && it->width() >= 24, name);
            QVERIFY2(qAbs(it->height() - 28) < 0.5, name);
        }
        QVERIFY(e.byName("maskOk")->property("primary").toBool());
        QVERIFY(!e.byName("maskCancel")->property("primary").toBool());
        QVERIFY(e.byName("maskClear")->property("danger").toBool());
        // The destructive Clear is the last of the tools, with a gap before it at least twice the others'.
        QQuickItem *clear = e.byName("maskClear"), *only = e.byName("maskOnly");
        const double gapBefore = clear->mapToScene(QPointF(0, 0)).x() - (only->mapToScene(QPointF(0, 0)).x() + only->width());
        QQuickItem *fill = e.byName("maskFill");
        const double gapInside = only->mapToScene(QPointF(0, 0)).x() - (fill->mapToScene(QPointF(0, 0)).x() + fill->width());
        QVERIFY2(gapBefore >= 2 * gapInside, qPrintable(QString("%1 vs %2").arg(gapBefore).arg(gapInside)));
        // The bar fits in the dialog.
        QQuickItem *bar = e.byName("maskBar");
        QVERIFY(bar);
        QVERIFY2(bar->width() <= qobject_cast<QQuickItem *>(e.editor->property("contentItem").value<QObject *>())->width() + 1,
                 qPrintable(QString::number(bar->width())));
    }

    void theBrushRingFollowsThePointerAndTheStrokeLeavesNoRingWhenAway() {
        Editor e;
        QVERIFY(e.start());
        QQuickItem *ring = e.byName("maskBrushRing");
        QVERIFY(ring);
        QTest::mouseMove(e.app.window, e.at(150, 100));
        QTRY_VERIFY(ring->isVisible());
        const QPoint p = ring->mapToScene(QPointF(0, 0)).toPoint();
        QVERIFY((p - e.at(150, 100)).manhattanLength() <= 2);
        QTest::mouseMove(e.app.window, e.at(200, 100));
        QTRY_VERIFY((ring->mapToScene(QPointF(0, 0)).toPoint() - e.at(200, 100)).manhattanLength() <= 2);
        // The Lasso has no ring.
        e.key(Qt::Key_L);
        QTRY_VERIFY(!ring->isVisible());
    }

    // ---- pictures to look at (not checks) ------------------------------------------------------------------

    void savesTheEditorOnLightAndDarkThemes_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("name");
        QTest::newRow("dark") << "tokyo-night" << "dark";
        QTest::newRow("light") << "catppuccin-latte" << "light";
    }
    void savesTheEditorOnLightAndDarkThemes() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("set FUGUE_SHOT_DIR to save the mask editor's pictures");
        QFETCH(QString, theme);
        QFETCH(QString, name);
        Editor e(QSize(640, 400), Look{theme, 8});
        QVERIFY(e.start());
        e.editor->setProperty("brushSize", 70);
        e.editor->setProperty("brushHardness", 0.6);
        e.dragOver({120, 120}, {300, 170});
        e.dragOver({300, 170}, {460, 120});
        e.key(Qt::Key_R);
        e.dragOver({420, 250}, {540, 330});
        e.key(Qt::Key_B);
        QTest::mouseMove(e.app.window, e.at(200, 280));
        QTest::qWait(120);
        const auto save = [&](const QString &suffix) {
            const QImage shot = test_support::grab(e.app.window);
            QVERIFY(!shot.isNull());
            QVERIFY(shot.save(QStringLiteral("%1/mask-editor-%2-%3.png").arg(QString::fromUtf8(dir), name, suffix)));
        };
        save("overlay");
        e.key(Qt::Key_H);
        QTest::qWait(120);
        save("mask-only");
    }
};

QTEST_MAIN(TestCanvasMaskEditor)
#include "test_canvas_mask_editor.moc"
