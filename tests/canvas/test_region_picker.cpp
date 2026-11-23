// The region picker window of a Canvas Region (#213, epic #206, spec 14.5, 12.7), offscreen in the real
// window: the Pick region… entry of the canvas drop-down opens the canvas at the node's revision with a
// rectangle on it; a drag moves or resizes it, the numbers follow, the dashed context box and the model
// size are the node's own; Apply writes x, y, width and height as one undo step and Cancel nothing. The
// store is the test's own folder; the rectangle is dragged with QTest mouse events on the offscreen window
// (no window outside the test process, no real pointer).

#include <QImage>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QUrl>
#include <cmath>

#include "canvas/canvas_choices.h"
#include "canvas/region_pick.h"
#include "canvas/widget_edit.h"
#include "canvas_app.h"
#include "../window_grab.h"
#include "engine/canvas_store.h"
#include "engine/paint_file.h"
#include "nodes/canvas_region_ops.h"

using namespace fugue;
using canvas_test::App;
using canvas_test::item;
using canvas_test::load;

namespace {

QByteArray fixture(const QString &canvas, const QString &inputs) {
    return QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {"region": {"type": "fugue/canvas-region", "version": "1.0.0", "inputs": {"canvas": "%1"%2}}}},
  "layout": {"nodes": {"region": {"pos": [20, 20]}}, "view": {"center": [200, 200], "zoom": 1}}
})").arg(canvas, inputs).toUtf8();
}

QQuickItem *seen(QQuickItem *from, const QString &name) {
    for (QQuickItem *child : from->childItems()) {
        if (!child->isVisible())
            continue;
        if (child->objectName() == name)
            return child;
        if (QQuickItem *found = seen(child, name))
            return found;
    }
    return nullptr;
}

struct RegionApp {
    QTemporaryDir store;
    QString canvasId, head;
    std::unique_ptr<App> app;

    // A canvas of width x height with a red block in it, and a node whose inputs are `inputs` (JSON members after canvas).
    explicit RegionApp(int width = 400, int height = 300, const QString &inputs = R"(, "x": 100, "y": 60, "width": 160, "height": 96, "padding": 20)",
                       bool withCanvas = true, const canvas_test::Look &look = {}) {
        if (withCanvas) {
            mask::PaintDocument doc(width, height);
            doc.addLayer(mask::LayerKind::Paint, "L");
            doc.brush.color = mask::Rgba{200, 20, 20, 255};
            doc.fillRect(50, 40, 120, 80);
            canvas::CanvasStore s = writer();
            const auto made = s.create("Sketch", doc, "new");
            canvasId = made->id;
            head = made->head;
        }
        app = std::make_unique<App>(load(fixture(canvasId, inputs)), std::function<void(fugue::Registry &)>{}, look);
        app->model.widgets()->canvasChoices()->setStoreRoot(store.filePath("store"));
    }
    canvas::CanvasStore writer() const { return canvas::CanvasStore(store.filePath("store")); }

    QQuickItem *top() const {
        QQuickItem *root = app->window->contentItem();
        while (root->parentItem())
            root = root->parentItem();
        return root;
    }
    QQuickItem *find(const QString &name) const { return seen(top(), name); }
    QObject *dialog() const { return app->canvas->property("regionDialog").value<QObject *>(); }
    bool dialogOpen() const { return dialog() && dialog()->property("opened").toBool(); }
    void openDialog() { QVERIFY(QMetaObject::invokeMethod(app->canvas, "openRegion", Q_ARG(QVariant, QString("region")))); }
    QJsonValue stored(const QString &input) const { return app->model.document().graph.nodes.value("region").inputs.value(input).toJson(); }
    int steps() const { return app->model.undoStack()->count(); }
    QVariantMap draft() const { return dialog()->property("draft").toMap(); }
    QRectF frame() const {
        QQuickItem *f = find("cropFrame");
        return f ? QRectF(f->mapToScene(QPointF(1, 1)), QSizeF(f->width() - 2, f->height() - 2)) : QRectF();
    }
    // The dashed box, in window coordinates.
    QRectF dashed() const {
        QQuickItem *s = find("regionContext");
        if (!s)
            return {};
        const QRectF box = s->property("box").toRectF(); // ContextBox: in the item's own coordinates
        return QRectF(s->mapToScene(box.topLeft()), s->mapToScene(box.bottomRight()));
    }
    // A drag with the pointer first moved onto the start, as a person's is (the popup sees it arrive).
    void drag(QPoint from, QPoint to) {
        QTest::mouseMove(app->window, from);
        QTest::qWait(30);
        app->drag(Qt::LeftButton, from, to);
    }
    // Clicks the button once it has stopped moving: a popup is still sliding in for a moment after it opens, and a click on
    // a slower machine (CI's) would land where the button was.
    void click(const QString &name) {
        QPoint at;
        int still = 0;
        for (int i = 0; still < 3 && i < 500; ++i) {
            QTest::qWait(20);
            QQuickItem *now = find(name);
            if (!now) {
                still = 0;
                continue;
            }
            const QPoint here = now->mapToScene(QPointF(now->width() / 2, now->height() / 2)).toPoint();
            still = here == at ? still + 1 : 0;
            at = here;
        }
        QVERIFY2(still >= 3, qPrintable(name));
        QTest::mouseClick(app->window, Qt::LeftButton, {}, at);
    }
    QString infoText() const { return find("regionInfo")->property("text").toString(); }
};

} // namespace

class TestRegionPicker : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void theFrameIsTheNodesRectangleOnTheCanvasAndTheDashedBoxIsItsPaddedContext() {
        RegionApp r;
        QVERIFY(r.app->open());
        r.openDialog();
        QTRY_VERIFY2(r.dialogOpen() && r.frame().width() > 0, qPrintable(r.app->warnings.join('\n')));
        const QRectF frame = r.frame();
        const qreal scale = frame.width() / 160;
        QVERIFY(qAbs(frame.height() - 96 * scale) < 1.5);
        // The picture is the canvas: 400 x 300 fills the frame's coordinates.
        QCOMPARE(r.dialog()->property("picker").value<QObject *>()->property("canvasSize").toSize(), QSize(400, 300));
        // The dashed box is the node's context: 20 px of padding each side.
        const QRectF dashed = r.dashed();
        QVERIFY2(qAbs(dashed.left() - (frame.left() - 20 * scale)) < 2.5, qPrintable(QString::number(dashed.left() - frame.left())));
        QVERIFY(qAbs(dashed.top() - (frame.top() - 20 * scale)) < 2.5);
        QVERIFY(qAbs(dashed.width() - (160 + 40) * scale) < 2.5);
        QVERIFY(qAbs(dashed.height() - (96 + 40) * scale) < 2.5);
        // The size the model gets, as the node computes it: (160 + 40) x (96 + 40) at scale 1, multiple 16.
        const nodes::CropSize size = nodes::regionSize({80, 40, 200, 136}, 1, 1.5, 16);
        QVERIFY2(r.infoText().contains(QString("The model gets %1 × %2").arg(size.width).arg(size.height)), qPrintable(r.infoText()));
        QVERIFY2(r.infoText().contains("400 × 300 canvas at head"), qPrintable(r.infoText()));
        QCOMPARE(r.steps(), 0); // looking changes nothing
        QVERIFY2(r.app->warnings.isEmpty(), qPrintable(r.app->warnings.join('\n')));
    }

    void draggingMovesAndResizesAndApplyWritesTheFourNumbersAsOneUndoStep() {
        RegionApp r;
        QVERIFY(r.app->open());
        r.openDialog();
        QTRY_VERIFY(r.frame().width() > 0);
        const QRectF frame = r.frame();
        const qreal scale = frame.width() / 160;
        // Move it by (20, 10) pixels.
        r.drag(frame.center().toPoint(), frame.center().toPoint() + QPoint(qRound(20 * scale), qRound(10 * scale)));
        QTRY_VERIFY(qAbs(r.draft().value("x").toInt() - 120) <= 1);
        QVERIFY(qAbs(r.draft().value("y").toInt() - 70) <= 1);
        const int movedX = r.draft().value("x").toInt(), movedY = r.draft().value("y").toInt();
        // The dashed box followed.
        QTRY_VERIFY(qAbs(r.dashed().left() - (r.frame().left() - 20 * scale)) < 2.5);
        // The bottom right handle: 30 px out each way.
        const QRectF moved = r.frame();
        r.drag(moved.bottomRight().toPoint(), moved.bottomRight().toPoint() + QPoint(qRound(30 * scale), qRound(30 * scale)));
        QTRY_VERIFY(qAbs(r.draft().value("width").toInt() - 190) <= 2);
        QVERIFY(qAbs(r.draft().value("height").toInt() - 126) <= 2);
        QCOMPARE(r.draft().value("x").toInt(), movedX); // the far corner moved, not the origin
        const int width = r.draft().value("width").toInt(), height = r.draft().value("height").toInt();
        // Nothing is written until OK (named as the Crop window's and the mask editor's commit, the cohesion audit's N7).
        QCOMPARE(r.find("regionApply")->property("text").toString(), QString("OK"));
        QCOMPARE(r.steps(), 0);
        QCOMPARE(r.stored("x"), QJsonValue(100));
        // The numbers show the rectangle.
        QCOMPARE(r.find("regionField-width")->property("text").toString(), QString::number(width));
        r.click("regionApply");
        QTRY_VERIFY(!r.dialogOpen());
        QCOMPARE(r.stored("x"), QJsonValue(movedX));
        QCOMPARE(r.stored("y"), QJsonValue(movedY));
        QCOMPARE(r.stored("width"), QJsonValue(width));
        QCOMPARE(r.stored("height"), QJsonValue(height));
        QCOMPARE(r.steps(), 1); // all four, one step
        r.app->model.undo();
        QCOMPARE(r.stored("x"), QJsonValue(100));
        QCOMPARE(r.stored("y"), QJsonValue(60));
        QCOMPARE(r.stored("width"), QJsonValue(160));
        QCOMPARE(r.stored("height"), QJsonValue(96));
        QVERIFY2(r.app->warnings.isEmpty(), qPrintable(r.app->warnings.join('\n')));
    }

    void cancelAndEscapeChangeNothing() {
        RegionApp r;
        QVERIFY(r.app->open());
        r.openDialog();
        QTRY_VERIFY(r.frame().width() > 0);
        const QRectF frame = r.frame();
        r.drag(frame.center().toPoint(), frame.center().toPoint() + QPoint(30, 20));
        QTRY_VERIFY(r.draft().value("x").toInt() != 100);
        r.click("regionCancel");
        QTRY_VERIFY(!r.dialogOpen());
        QCOMPARE(r.steps(), 0);
        QCOMPARE(r.stored("x"), QJsonValue(100));
        // Escape, after another drag, likewise; and opening again starts from the node.
        r.openDialog();
        QTRY_VERIFY(r.frame().width() > 0);
        QCOMPARE(r.draft().value("x").toInt(), 100);
        r.app->key(Qt::Key_Escape);
        QTRY_VERIFY(!r.dialogOpen());
        QCOMPARE(r.steps(), 0);
    }

    void aNumberTypedMovesTheFrameAndSnapPutsEdgesOnTheMultiple() {
        RegionApp r;
        QVERIFY(r.app->open());
        r.openDialog();
        QTRY_VERIFY(r.frame().width() > 0);
        QQuickItem *x = r.find("regionField-x");
        QVERIFY(x);
        const qreal before = r.frame().left();
        x->forceActiveFocus();
        x->setProperty("text", "130");
        x->setFocus(false); // entering or leaving the field ends the edit
        QTRY_COMPARE(r.draft().value("x").toInt(), 130);
        QTRY_VERIFY(r.frame().left() > before + 5);
        // Snap to 16: 130 -> 128 and every edge on a multiple.
        r.click("regionSnap");
        QTRY_COMPARE(r.draft().value("x").toInt(), 128);
        for (const char *key : {"x", "y", "width", "height"})
            QVERIFY2(r.draft().value(key).toInt() % 16 == 0, key);
        // A drag now snaps too.
        const QRectF frame = r.frame();
        const qreal scale = frame.width() / r.draft().value("width").toInt();
        r.drag(frame.center().toPoint(), frame.center().toPoint() + QPoint(qRound(21 * scale), qRound(7 * scale)));
        QTRY_VERIFY(r.draft().value("x").toInt() != 128);
        QCOMPARE(r.draft().value("x").toInt() % 16, 0);
        QCOMPARE(r.draft().value("y").toInt() % 16, 0);
        QVERIFY2(r.app->warnings.isEmpty(), qPrintable(r.app->warnings.join('\n')));
    }

    void aRatioLockHoldsWhileResizing() {
        RegionApp r;
        QVERIFY(r.app->open());
        r.openDialog();
        QTRY_VERIFY(r.frame().width() > 0);
        r.click("regionRatio-1:1");
        QTRY_COMPARE(r.draft().value("width").toInt(), r.draft().value("height").toInt());
        const QRectF frame = r.frame();
        const qreal scale = frame.width() / r.draft().value("width").toInt();
        r.drag(frame.bottomRight().toPoint(), frame.bottomRight().toPoint() + QPoint(qRound(20 * scale), qRound(5 * scale)));
        QTRY_VERIFY(r.draft().value("width").toInt() > 96);
        QCOMPARE(r.draft().value("width").toInt(), r.draft().value("height").toInt());
    }

    void theCapShowsInTheSizeLine() {
        RegionApp r(4000, 3000, R"(, "x": 0, "y": 0, "width": 3000, "height": 2000, "padding": 0, "max_megapixels": 1)");
        QVERIFY(r.app->open());
        r.openDialog();
        QTRY_VERIFY(r.find("regionInfo"));
        QVERIFY2(r.infoText().contains("at the 1 MP cap"), qPrintable(r.infoText()));
        const nodes::CropSize size = nodes::regionSize({0, 0, 3000, 2000}, 1, 1, 16);
        QVERIFY2(r.infoText().contains(QString("The model gets %1 × %2").arg(size.width).arg(size.height)), qPrintable(r.infoText()));
    }

    // A big canvas draws from a smaller copy, the numbers stay the canvas's.
    void aLargeCanvasIsShownDownscaledAndTheRectangleStaysInItsPixels() {
        RegionApp r(6000, 3000, R"(, "x": 3000, "y": 1500, "width": 1200, "height": 600)");
        QVERIFY(r.app->open());
        r.openDialog();
        QTRY_VERIFY(r.frame().width() > 0);
        QObject *picker = r.dialog()->property("picker").value<QObject *>();
        QCOMPARE(picker->property("canvasSize").toSize(), QSize(6000, 3000));
        const QImage shown(picker->property("source").toUrl().toLocalFile());
        QVERIFY(!shown.isNull());
        QCOMPARE(shown.size(), QSize(2048, 1024));
        QCOMPARE(r.draft().value("x").toInt(), 3000);
        QCOMPARE(r.draft().value("width").toInt(), 1200);
        const QRectF frame = r.frame();
        QVERIFY(qAbs(frame.width() / frame.height() - 2.0) < 0.05);
        // A drag still moves it in canvas pixels, past the downscale.
        const qreal scale = frame.width() / 1200;
        r.drag(frame.center().toPoint(), frame.center().toPoint() + QPoint(20, 0));
        QTRY_VERIFY(r.draft().value("x").toInt() > 3000);
        // 20 window pixels are 20 / scale canvas pixels, to within one window pixel.
        QVERIFY2(qAbs(r.draft().value("x").toInt() - 3000 - 20 / scale) <= 1 / scale + 1, qPrintable(QString::number(r.draft().value("x").toInt())));
    }

    // The head is named when the picker opens, as a run names it when it compiles.
    void headIsResolvedAtOpenAndAnExplicitRevisionIsRead() {
        RegionApp r;
        QVERIFY(r.app->open());
        r.openDialog();
        QTRY_VERIFY(r.frame().width() > 0);
        QObject *picker = r.dialog()->property("picker").value<QObject *>();
        QCOMPARE(picker->property("resolved").toString(), r.head);
        r.click("regionCancel");
        QTRY_VERIFY(!r.dialogOpen());
        // A new head (a bigger revision), then the picker again: it sees it.
        canvas::CanvasStore s = r.writer();
        mask::PaintDocument second(400, 300);
        second.addLayer(mask::LayerKind::Paint, "L");
        second.brush.color = mask::Rgba{20, 200, 20, 255};
        second.fillRect(10, 10, 50, 50);
        const auto next = s.commit(r.canvasId, second, r.head, "test");
        QVERIFY(next.ok());
        r.openDialog();
        QTRY_COMPARE(r.dialog()->property("picker").value<QObject *>()->property("resolved").toString(), next->revision.id);
        r.click("regionCancel");
        QTRY_VERIFY(!r.dialogOpen());
        // The node names the first revision: that is what the picker reads.
        QVERIFY(r.app->model.widgets()->setText("region", "revision", r.head.mid(7, 12)).isEmpty());
        r.openDialog();
        QTRY_COMPARE(r.dialog()->property("picker").value<QObject *>()->property("resolved").toString(), r.head);
        QVERIFY2(r.infoText().contains("revision (" + r.head.mid(7, 8) + ")"), qPrintable(r.infoText()));
    }

    void theMenuEntryOpensItAndAMaskIsSaidToWinOverTheRectangle() {
        RegionApp r(400, 300, R"(, "x": 10, "y": 10, "width": 50, "height": 50, "mask": {"from": "src.mask"})");
        // The mask's source need not exist for the picker: it only sees the input is linked.
        QVERIFY(r.app->open());
        QVariantMap state = r.app->model.widgets()->regionState("region");
        QVERIFY(state.value("ok").toBool());
        QVERIFY(state.value("maskLinked").toBool());
        QVERIFY(state.value("editable").toBool());
        r.openDialog();
        QTRY_VERIFY(r.find("regionNote"));
        QVERIFY2(r.find("regionNote")->property("text").toString().contains("mask"), "the note says the mask wins");
    }

    void aLinkedRectangleCannotBeApplied() {
        RegionApp r(400, 300, R"(, "x": {"from": "n.value"}, "y": 10, "width": 50, "height": 50)");
        QVERIFY(r.app->open());
        QVERIFY(!r.app->model.widgets()->regionState("region").value("editable").toBool());
        QVERIFY(!r.app->model.widgets()->setRegion("region", 1, 2, 3, 4));
        r.openDialog();
        QTRY_VERIFY(r.find("regionNote"));
        QVERIFY(r.find("regionNote")->property("text").toString().contains("linked"));
        QVERIFY(!r.find("regionApply")->isEnabled());
        QCOMPARE(r.steps(), 0);
    }

    void aMissingCanvasOpensWithTheReasonAndNothingToApply() {
        RegionApp r(400, 300, "", false);
        r.app = std::make_unique<App>(load(fixture("canvas-0123456789ab", "")));
        r.app->model.widgets()->canvasChoices()->setStoreRoot(r.store.filePath("store"));
        QVERIFY(r.app->open());
        r.openDialog();
        QTRY_VERIFY(r.find("regionError"));
        QVERIFY(!r.find("regionApply")->isEnabled());
        QVERIFY(!r.find("regionError")->property("text").toString().isEmpty());
        QVERIFY(!r.find("cropFrame"));
    }

    void settingRegionOnAnotherNodeDoesNothing() {
        RegionApp r;
        QVERIFY(r.app->open());
        QVERIFY(r.app->model.widgets()->regionState("nothing").isEmpty());
        QVERIFY(!r.app->model.widgets()->setRegion("nothing", 1, 2, 3, 4));
        // The same numbers as stored: no step.
        QVERIFY(!r.app->model.widgets()->setRegion("region", 100, 60, 160, 96));
        QCOMPARE(r.steps(), 0);
    }

    // Pictures: FUGUE_SHOT_DIR=<dir> test_region_picker savesThePictures (nothing without it), dark and light.
    void savesThePictures_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("name");
        QTest::newRow("dark") << "tokyo-night" << "dark";
        QTest::newRow("light") << "catppuccin-latte" << "light";
    }
    void savesThePictures() {
        const QByteArray dir = qgetenv("FUGUE_SHOT_DIR");
        if (dir.isEmpty())
            QSKIP("FUGUE_SHOT_DIR is not set");
        QFETCH(QString, theme);
        QFETCH(QString, name);
        RegionApp r(400, 300, R"(, "x": 100, "y": 60, "width": 160, "height": 96, "padding": 20)", true, canvas_test::Look{theme, 8});
        QVERIFY(r.app->open());
        r.openDialog();
        QTRY_VERIFY(r.dialogOpen() && r.frame().width() > 0);
        QTest::qWait(400);
        QVERIFY(test_support::grab(r.app->window).save(QStringLiteral("%1/region-%2.png").arg(QString::fromLocal8Bit(dir), name)));
    }
};

QTEST_MAIN(TestRegionPicker)
#include "test_region_picker.moc"
