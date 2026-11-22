// The Canvas view's mask brush (#230), the model alone: Add mask is one revision with a fully revealing mask and the
// brush then targets it; strokes painted into the view's MaskLayer are batched into ONE revision once the brush rests
// (producer canvas-view:mask-paint), their unchanged tiles shared; undo goes back one revision and the mask is read again;
// invert, fill and clear are one revision each; another layer ends the target; a painted selection (Quick Mask) is
// stored by content and written to the followed Canvas Region's `selection` input as one undo step.
// No window: CanvasViewModel on a store of the test's own, and a MaskLayer item outside any scene.

#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/canvas_view_model.h"
#include "canvas/mask_layer.h"
#include "canvas_app.h"
#include "engine/canvas_store.h"

using namespace fugue;
using namespace fugue::mask;

namespace {

MaskPaint solid(int w, int h, Rgba c) {
    MaskPaint m(w, h, 4);
    m.fill(c);
    return m;
}

// Base (0), Top (1), on a 512 px canvas: four tiles a side of 256.
PaintDocument sample() {
    PaintDocument d(512, 512);
    d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(512, 512, {30, 60, 200, 255}));
    d.loadLayer(LayerKind::Paint, "Top", true, 1, solid(512, 512, {200, 10, 10, 255}));
    return d;
}

void stroke(MaskLayer &layer, double x0, double y0, double x1, double y1, bool erase) {
    layer.beginStroke(erase, 30, 1, x0, y0);
    layer.strokeTo((x0 + x1) / 2, (y0 + y1) / 2);
    layer.strokeTo(x1, y1);
    layer.endStroke();
}

} // namespace

class TestCanvasMaskBrush : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_canvas, m_head;

    canvas::CanvasStore store() const { return canvas::CanvasStore(m_dir.filePath("store")); }
    void open(CanvasViewModel &model, MaskLayer &layer) {
        model.setStoreRoot(m_dir.filePath("store"));
        model.setCanvasId(m_canvas);
        model.setMaskLayer(&layer);
        model.setMaskIdle(60);
        QTRY_COMPARE(model.revisionB(), m_head);
    }
    int revisionCount() const { return int(store().log(m_canvas).value().size()); }
    canvas::LayerRecord top() const {
        const auto head = store().info(m_canvas)->head;
        return store().tree(store().revision(m_canvas, head)->tree)->layers[1];
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void init() {
        QDir(m_dir.filePath("store")).removeRecursively();
        const auto made = store().create("Masks", sample(), "new");
        QVERIFY(made);
        m_canvas = made->id;
        m_head = made->head;
    }

    void addMaskIsOneRevisionAndTargetsTheNewMask() {
        CanvasViewModel model;
        MaskLayer layer;
        open(model, layer);
        QCOMPARE(model.maskTarget(), -1);
        QVERIFY(model.addMask(1));
        QCOMPARE(revisionCount(), 2);
        QCOMPARE(store().revision(m_canvas, model.head())->producer, QString("canvas-view:mask-add"));
        QVERIFY(top().hasMask);
        QCOMPARE(model.maskTarget(), 1);
        QCOMPARE(model.selection(), (QVariantList{1}));
        QCOMPARE(layer.valueAt(10, 10), 255); // fully revealing
        QCOMPARE(layer.imageSize(), QSize(512, 512));
        QVERIFY(model.addMask(1)); // a layer with a mask is only targeted again
        QCOMPARE(revisionCount(), 2);
        QCOMPARE(model.stepName(model.head()), QString("Mask added: Top"));
    }

    void strokesAreBatchedIntoOneRevisionThatSharesItsTiles() {
        CanvasViewModel model;
        MaskLayer layer;
        open(model, layer);
        QVERIFY(model.addMask(1));
        const canvas::TileMap before = top().maskTiles;
        // Two strokes in a row in one corner (a layer mask's brush hides: it erases toward black), the idle between them
        // shorter than the rest: one revision.
        model.setMaskIdle(400);
        stroke(layer, 20, 20, 80, 30, true);
        model.maskPainted();
        stroke(layer, 20, 60, 80, 70, true);
        model.maskPainted();
        QVERIFY(model.maskPending());
        QCOMPARE(revisionCount(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(revisionCount(), 3, 3000);
        QVERIFY(!model.maskPending());
        QCOMPARE(store().revision(m_canvas, model.head())->producer, QString("canvas-view:mask-paint"));
        QCOMPARE(model.revisionB(), model.head());
        QCOMPARE(model.stepName(model.head()), QString("Painted: Top"));
        const canvas::TileMap after = top().maskTiles;
        int shared = 0;
        for (const auto &[key, hash] : after)
            shared += before.count(key) && before.at(key) == hash ? 1 : 0;
        QCOMPARE(int(after.size()), 4);
        QCOMPARE(shared, 3); // 512 px is four tiles of 256; the strokes touched one
    }

    void undoGoesBackOneRevisionAndReadsTheMaskAgain() {
        CanvasViewModel model;
        MaskLayer layer;
        open(model, layer);
        QVERIFY(model.addMask(1));
        stroke(layer, 100, 100, 200, 100, true);
        model.maskPainted();
        QCOMPARE(layer.valueAt(150, 100), 0);
        // Undo while the strokes rest: they are stored first, then undone: the head is the Add mask again.
        const QString added = model.head();
        QVERIFY(model.undo());
        QVERIFY(!store().redoTarget(m_canvas)->isEmpty()); // the strokes' revision, undone
        QCOMPARE(model.head(), added);
        QCOMPARE(model.maskTarget(), 1);
        QCOMPARE(layer.valueAt(150, 100), 255); // read again from the revision shown
        QVERIFY(model.redo());
        QCOMPARE(layer.valueAt(150, 100), 0);
        // Undo the mask itself: the target goes.
        QVERIFY(model.undo());
        QVERIFY(model.undo());
        QCOMPARE(model.head(), m_head);
        QCOMPARE(model.maskTarget(), -1);
    }

    void invertFillAndClearAreOneRevisionEach() {
        CanvasViewModel model;
        MaskLayer layer;
        open(model, layer);
        QVERIFY(model.addMask(1));
        QVERIFY(model.invertMask());
        QCOMPARE(revisionCount(), 3);
        QCOMPARE(layer.valueAt(5, 5), 0);
        QVERIFY(top().maskTiles.empty()); // all black: no tiles
        QCOMPARE(store().revision(m_canvas, model.head())->producer, QString("canvas-view:mask-invert"));
        QVERIFY(model.fillMask());
        QCOMPARE(revisionCount(), 4);
        QVERIFY(model.clearMask());
        QCOMPARE(revisionCount(), 5);
        QVERIFY(!model.clearMask()); // nothing changes: nothing written
        QCOMPARE(revisionCount(), 5);
    }

    void anotherLayerOrAnEarlierStepEndsTheTarget() {
        CanvasViewModel model;
        MaskLayer layer;
        open(model, layer);
        QVERIFY(model.addMask(1));
        stroke(layer, 10, 10, 50, 10, true);
        model.maskPainted();
        model.selectLayer(0); // what was painted is stored first, as its own step
        QCOMPARE(revisionCount(), 3);
        QCOMPARE(model.maskTarget(), -1);
        QVERIFY(!model.targetMask(0)); // Base has no mask
        QVERIFY(model.targetMask(1));
        model.show(m_head); // an earlier step: nothing is painted there
        QTRY_COMPARE(model.revisionB(), m_head);
        QCOMPARE(model.maskTarget(), -1);
    }

    void aPaintedSelectionGoesIntoTheFollowedRegionAsOneUndoStep() {
        canvas_test::App app(canvas_test::load(QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {"region": {"type": "fugue/canvas-region", "version": "1.2.0", "inputs": {"canvas": "%1"}}}}, "layout": {"nodes": {"region": {"pos": [20, 20]}}, "view": {"center": [200, 200], "zoom": 1}}})")
                                                   .arg(m_canvas).toUtf8()));
        app.model.widgets()->canvasChoices()->setStoreRoot(m_dir.filePath("store"));
        QVERIFY(app.open());
        CanvasViewModel model;
        MaskLayer layer, selection;
        model.setEdits(app.model.widgets());
        open(model, layer);
        model.setFollowNode("region");
        QTRY_VERIFY(model.region().value("valid").toBool());
        selection.setImageSize({512, 512});
        QCOMPARE(model.useSelection(&selection).value("ok").toBool(), false); // nothing painted
        selection.fillRect(100, 120, 40, 30, false);
        const QVariantMap used = model.useSelection(&selection);
        QVERIFY2(used.value("ok").toBool(), qPrintable(used.value("reason").toString()));
        const QString hash = used.value("hash").toString();
        QCOMPARE(model.region().value("selection").toString(), hash);
        QCOMPARE(model.region().value("x").toInt(), 100);
        QCOMPARE(model.region().value("width").toInt(), 40);
        QCOMPARE(store().selection(hash)->pixels(), selection.paint().pixels());
        QCOMPARE(app.model.undoStack()->count(), 1);
        app.model.undoStack()->undo();
        QTRY_COMPARE(model.region().value("selection").toString(), QString());
        app.model.undoStack()->redo();
        QTRY_COMPARE(model.region().value("selection").toString(), hash);
        // A rectangle drawn replaces the selection, in the same step.
        QVERIFY(model.setRegion(QVariantMap{{"x", 10}, {"y", 10}, {"width", 64}, {"height", 64}}));
        QCOMPARE(model.region().value("selection").toString(), QString());
        QCOMPARE(app.model.undoStack()->count(), 2);
    }
};

QTEST_MAIN(TestCanvasMaskBrush)
#include "test_canvas_mask_brush.moc"
