// The backend of the Canvas view (#221, epics #216/#217, spec 14.5): the strip of what to look at, the overlay of a
// candidate on the head, the Canvas Region rectangle read through the edit layer, 'Region reads from this revision' as
// one undo step, and the refresh when the store changes. No window: the model alone, a store of the test's own.

#include <QQuickStyle>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QtTest>

#include "canvas/canvas_view_model.h"
#include "canvas/widget_edit.h"
#include "canvas_app.h"
#include "engine/canvas_store.h"

using namespace fugue;
using namespace fugue::mask;
using canvas_test::App;
using canvas_test::load;

namespace {

PaintDocument withFill(PaintDocument doc, const QString &name, int x) {
    doc.addLayer(LayerKind::Paint, name);
    doc.brush.color = Rgba{200, 10, 10, 255};
    doc.fillRect(x, 100, 60, 60);
    return doc;
}

QByteArray fixture(const QString &canvas, const QString &inputs) {
    return QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {"region": {"type": "fugue/canvas-region", "version": "1.0.0", "inputs": {"canvas": "%1"%2}},
                      "other": {"type": "fugue/canvas-region", "version": "1.0.0", "inputs": {"canvas": "canvas-0123456789ab"}}}},
  "layout": {"nodes": {"region": {"pos": [20, 20]}, "other": {"pos": [20, 200]}}, "view": {"center": [200, 200], "zoom": 1}}
})").arg(canvas, inputs).toUtf8();
}

QStringList ids(const QVariantList &rows) {
    QStringList out;
    for (const QVariant &row : rows)
        out << row.toMap().value("id").toString();
    return out;
}

} // namespace

class TestCanvasViewModel : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_canvas, m_head, m_c1, m_c2;
    PaintDocument m_base{512, 512};

    canvas::CanvasStore store() const { return canvas::CanvasStore(m_dir.filePath("store")); }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void init() {
        QDir(m_dir.filePath("store")).removeRecursively();
        m_base = PaintDocument(512, 512);
        m_base.addLayer(LayerKind::Paint, "Base");
        m_base.brush.color = Rgba{30, 60, 200, 255};
        m_base.fillRect(0, 0, 512, 512);
        canvas::CanvasStore s = store();
        const auto made = s.create("Sketch", m_base, "new");
        QVERIFY(made);
        m_canvas = made->id;
        m_head = made->head;
        m_c1 = s.addRevision(m_canvas, withFill(m_base, "Fill A", 100), m_head, "canvas-write:a").value().id;
        m_c2 = s.addRevision(m_canvas, withFill(m_base, "Fill B", 300), m_head, "canvas-write:b").value().id;
    }

    void theStripListsTheHeadFirstThenCandidatesThenTheLatestRevisions() {
        canvas::CanvasStore s = store();
        // Seven more revisions on the head: five stay in the strip, the rest go to the menu.
        QString at = m_head;
        for (int i = 0; i < 7; ++i) {
            at = s.addRevision(m_canvas, withFill(m_base, QString("L%1").arg(i), 10 * i), at, "t").value().id;
            QVERIFY(s.accept(m_canvas, at));
        }
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        const QVariantList choices = view.choices();
        QCOMPARE(choices.size(), 1 + 2 + CanvasViewModel::recentCount);
        QCOMPARE(choices.first().toMap().value("kind").toString(), QString("head"));
        QCOMPARE(choices.first().toMap().value("id").toString(), at);
        QCOMPARE(choices.first().toMap().value("label").toString(), QString("Head"));
        QCOMPARE(choices.at(1).toMap().value("kind").toString(), QString("candidate"));
        QCOMPARE(choices.at(3).toMap().value("kind").toString(), QString("history"));
        QCOMPARE(view.earlier().size(), 7 - CanvasViewModel::recentCount); // 8 revisions less the head, five in the strip
        QCOMPARE(view.revisionB(), at); // opens on the head
        QVERIFY(!view.pictureSource().isEmpty());
    }

    void aCandidateIsComparedWithTheHeadAndTheOverlayTogglesItsPicture() {
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        QVERIFY(!view.showsCandidate());
        QCOMPARE(view.pictureSource(), view.sourceB());
        view.show(m_c1);
        QCOMPARE(view.revisionB(), m_c1);
        QCOMPARE(view.revisionA(), m_head); // against the head
        QVERIFY(view.showsCandidate());
        QVERIFY(view.overlay());
        QVERIFY(!view.headSource().isEmpty());
        QCOMPARE(view.pictureSource(), view.sourceB()); // the candidate over the head
        QVERIFY(view.pictureSource() != view.headSource());
        QSignalSpy changed(&view, &CanvasViewModel::pictureChanged);
        view.setOverlay(false);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(view.pictureSource(), view.headSource()); // the head alone
        QCOMPARE(view.pictureSize(), QSize(512, 512));
        view.setOverlay(true);
        QCOMPARE(view.pictureSource(), view.sourceB());
        QVERIFY(view.hasParent());
        QVERIFY(view.sourceDifference().isValid() && !view.sourceDifference().isEmpty());
        // Showing the head again drops the candidate's overlay: the picture is the head's.
        view.show(m_head);
        QVERIFY(!view.showsCandidate());
        view.setOverlay(false);
        QCOMPARE(view.pictureSource(), view.sourceB());
    }

    void acceptAndDropGoThroughTheSameStoreFunctionsAndTheStripFollows() {
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        view.show(m_c2);
        QVERIFY(view.drop(m_c2));
        QVERIFY(!ids(view.choices()).contains(m_c2)); // dropped ones leave the strip
        view.show(m_c1);
        QVERIFY(view.accept(m_c1));
        QCOMPARE(view.revisionB(), m_c1); // the accepted one is the head and shown
        QCOMPARE(view.choices().first().toMap().value("id").toString(), m_c1);
        QVERIFY(!view.showsCandidate());
    }

    void exportWritesTheShownPicture() {
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        view.show(m_c1);
        const QString target = m_dir.filePath("out/" + view.exportName());
        QDir().mkpath(m_dir.filePath("out"));
        QVERIFY(view.exportPicture(target));
        QVERIFY(QFileInfo(target).size() > 0);
        QVERIFY(!view.exportPicture(target)); // never replaced
        QVERIFY(!view.error().isEmpty());
    }

    void aStoreChangeShowsWithinAMoment() {
        CanvasViewModel view;
        view.setShowNewResults(false); // this one is about the strip and a moved head; the new result's view is tested below
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        QCOMPARE(view.choices().size(), 3);
        // A run elsewhere adds a candidate: the strip grows without a call.
        const QString c3 = store().addRevision(m_canvas, withFill(m_base, "Fill C", 200), m_head, "canvas-write:c").value().id;
        QTRY_COMPARE_WITH_TIMEOUT(view.choices().size(), 4, 3000);
        QVERIFY(ids(view.choices()).contains(c3));
        // The head moves: a view on the head goes along; one on a candidate stays.
        QVERIFY(store().accept(m_canvas, m_c1));
        QTRY_COMPARE_WITH_TIMEOUT(view.revisionB(), m_c1, 3000);
        view.show(m_c2);
        QString newHead = store().addRevision(m_canvas, withFill(m_base, "Next", 50), m_c1, "t").value().id;
        QVERIFY(store().accept(m_canvas, newHead));
        QTRY_VERIFY_WITH_TIMEOUT(view.head() == newHead, 3000);
        QCOMPARE(view.revisionB(), m_c2);
        // A canvas made elsewhere is in the list.
        const int before = view.canvases().size();
        QVERIFY(store().create("Second", m_base, "new"));
        QTRY_COMPARE_WITH_TIMEOUT(view.canvases().size(), before + 1, 3000);
    }

    // A candidate a run adds is added the way Canvas Write adds it (addRevision from the head, no head move).
    QString arrive(const QString &name, int x) {
        return store().addRevision(m_canvas, withFill(m_base, name, x), m_head, "canvas-write:" + name).value().id;
    }

    void aNewCandidateIsShownOverTheHeadAtOnceAndTheBannerNamesIt() {
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        QVERIFY(view.showNewResults());
        QCOMPARE(view.newResult(), QString()); // what was there before is not new
        QCOMPARE(view.revisionB(), m_head);
        const QString c3 = arrive("c3", 200);
        QTRY_COMPARE_WITH_TIMEOUT(view.revisionB(), c3, 3000);
        QCOMPARE(view.newResult(), c3);
        QCOMPARE(view.newResultOthers(), 0);
        QCOMPARE(view.newResultFrom(), QString()); // no document has the Canvas Write: no id is shown in its place (#228)
        QCOMPARE(view.revisionA(), m_head); // against the head
        QVERIFY(view.showsCandidate() && view.overlay());
        QCOMPARE(view.pictureSource(), view.sourceB()); // the combined picture
        // Back to Head: the banner ends, the candidate stays in the strip.
        view.show(m_head);
        QCOMPARE(view.newResult(), QString());
        QVERIFY(ids(view.choices()).contains(c3));
    }

    void severalArrivalsShowTheNewestAndCountTheOthers() {
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        const QString c3 = arrive("c3", 200);
        const QString c4 = arrive("c4", 220);
        // Both land in one refresh: the newest of the rows is shown, one other is counted.
        QTRY_VERIFY_WITH_TIMEOUT(!view.newResult().isEmpty(), 3000);
        QCOMPARE(view.revisionB(), view.newResult());
        QVERIFY(view.newResult() == c3 || view.newResult() == c4);
        const QStringList rows = ids(view.revisions());
        QCOMPARE(view.newResult(), rows.at(qMin(rows.indexOf(c3), rows.indexOf(c4)))); // the first of the rows is the newest
        QTRY_COMPARE_WITH_TIMEOUT(view.newResultOthers(), 1, 3000);
        // One more while still looking at the result it showed: it is shown, and the count grows.
        QTest::qWait(300);
        const QString c5 = arrive("c5", 240);
        QTRY_COMPARE_WITH_TIMEOUT(view.revisionB(), view.newResult(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(view.newResultOthers(), 2, 3000);
        QVERIFY(ids(view.choices()).contains(c5));
    }

    void aViewOnAnotherRevisionOrLockedOrSwitchedOffIsNeverMoved() {
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        // The person chose a candidate: a new one only joins the strip.
        view.show(m_c1);
        const QString c3 = arrive("c3", 200);
        QTRY_VERIFY_WITH_TIMEOUT(ids(view.choices()).contains(c3), 3000);
        QTest::qWait(300);
        QCOMPARE(view.revisionB(), m_c1);
        QCOMPARE(view.newResult(), QString());
        // On the head but locked (Compare on, a drag): nothing moves until the lock lifts, then the result shows.
        view.show(m_head);
        view.setViewLocked(true);
        const QString c4 = arrive("c4", 220);
        QTRY_VERIFY_WITH_TIMEOUT(ids(view.choices()).contains(c4), 3000);
        QTest::qWait(300);
        QCOMPARE(view.revisionB(), m_head);
        view.setViewLocked(false);
        QCOMPARE(view.revisionB(), c4);
        QCOMPARE(view.newResult(), c4);
        // Locked and then the person went elsewhere: the late result is not shown.
        view.show(m_head);
        view.setViewLocked(true);
        const QString c5 = arrive("c5", 240);
        QTRY_VERIFY_WITH_TIMEOUT(ids(view.choices()).contains(c5), 3000);
        view.show(m_c2);
        view.setViewLocked(false);
        QCOMPARE(view.revisionB(), m_c2);
        // Switched off: it joins the strip and the view stays on the head.
        view.show(m_head);
        view.setShowNewResults(false);
        const QString c6 = arrive("c6", 260);
        QTRY_VERIFY_WITH_TIMEOUT(ids(view.choices()).contains(c6), 3000);
        QTest::qWait(300);
        QCOMPARE(view.revisionB(), m_head);
        QCOMPARE(view.newResult(), QString());
    }

    void acceptAndDropOfTheNewResultEndItsBanner() {
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        const QString c3 = arrive("c3", 200);
        QTRY_COMPARE_WITH_TIMEOUT(view.newResult(), c3, 3000);
        QVERIFY(view.accept(c3));
        QCOMPARE(view.newResult(), QString());
        QTRY_COMPARE_WITH_TIMEOUT(view.head(), c3, 3000);
        // The next one arrives on the new head (a candidate of the old head waits, as it did).
        const QString c4 = store().addRevision(m_canvas, withFill(m_base, "c4", 220), c3, "canvas-write:c4").value().id;
        QTRY_COMPARE_WITH_TIMEOUT(view.newResult(), c4, 3000);
        QVERIFY(view.drop(c4));
        QCOMPARE(view.newResult(), QString());
    }

    void aFollowedRegionIsDrawnInCanvasPixelsAndMovesTheViewToItsCanvas() {
        App app(load(fixture(m_canvas, R"(, "x": 100, "y": 60, "width": 160, "height": 96, "padding": 20, "revision": "head")")));
        app.model.widgets()->canvasChoices()->setStoreRoot(m_dir.filePath("store"));
        QVERIFY(app.open());
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        QVERIFY(!view.region().value("valid").toBool()); // nothing followed
        view.setEdits(app.model.widgets());
        view.setFollowNode("region");
        QCOMPARE(view.canvasId(), m_canvas); // the node's canvas
        const QVariantMap region = view.region();
        QVERIFY2(region.value("valid").toBool(), qPrintable(region.value("reason").toString()));
        QCOMPARE(region.value("x").toInt(), 100);
        QCOMPARE(region.value("y").toInt(), 60);
        QCOMPARE(region.value("width").toInt(), 160);
        QCOMPARE(region.value("height").toInt(), 96);
        const QVariantMap context = region.value("context").toMap(); // 20 px of padding each side
        QCOMPARE(context.value("x").toInt(), 80);
        QCOMPARE(context.value("width").toInt(), 200);
        QCOMPARE(context.value("height").toInt(), 136);
        QCOMPARE(region.value("reads").toString(), QString("head"));
        QVERIFY(region.value("readsShown").toBool()); // head shown, head read
        // Looking is not an edit.
        QCOMPARE(app.model.undoStack()->count(), 0);
        // A node that is no region, and one on another canvas, say why and draw nothing.
        view.setFollowNode("nothing");
        QVERIFY(!view.region().value("valid").toBool());
        QVERIFY(!view.region().value("reason").toString().isEmpty());
        view.setFollowNode("region");
        QVERIFY(view.region().value("valid").toBool());
        view.setCanvasId("canvas-0123456789ab");
        QVERIFY(!view.region().value("valid").toBool());
    }

    void regionReadsFromThisRevisionIsOneUndoStepAndFollowsTheHeadOnHead() {
        App app(load(fixture(m_canvas, R"(, "x": 100, "y": 60, "width": 160, "height": 96, "revision": "head")")));
        app.model.widgets()->canvasChoices()->setStoreRoot(m_dir.filePath("store"));
        QVERIFY(app.open());
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setEdits(app.model.widgets());
        view.setFollowNode("region");
        QVERIFY(!view.useForRegion()); // already reads the head shown: nothing to write
        QCOMPARE(app.model.undoStack()->count(), 0);
        view.show(m_c1);
        QVERIFY(!view.region().value("readsShown").toBool());
        QVERIFY(view.useForRegion());
        QCOMPARE(app.model.undoStack()->count(), 1);
        QCOMPARE(app.model.document().graph.nodes.value("region").inputs.value("revision").toJson().toString(), m_c1);
        QVERIFY(view.region().value("readsShown").toBool());
        QCOMPARE(view.region().value("reads").toString(), m_c1);
        QVERIFY(!view.useForRegion()); // again: nothing
        QCOMPARE(app.model.undoStack()->count(), 1);
        // Back on the head writes "head", so the node goes on following it.
        view.show(m_head);
        QVERIFY(!view.region().value("readsShown").toBool());
        QVERIFY(view.useForRegion());
        QCOMPARE(app.model.undoStack()->count(), 2);
        QCOMPARE(app.model.widgets()->regionState("region").value("revision").toString(), QString("head"));
        // Undo puts the candidate's hash back, and the view sees it.
        app.model.undoStack()->undo();
        QTRY_COMPARE(view.region().value("reads").toString(), m_c1);
    }

    void theRegionIsEditedThroughTheModelInOneUndoStepWithTheNodesOwnNumbers() {
        App app(load(fixture(m_canvas, R"(, "x": 100, "y": 60, "width": 160, "height": 96, "padding": 20, "multiple": 16, "revision": "head")")));
        app.model.widgets()->canvasChoices()->setStoreRoot(m_dir.filePath("store"));
        QVERIFY(app.open());
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        QVERIFY(!view.setRegion({{"x", 1}, {"y", 1}, {"width", 8}, {"height", 8}})); // nothing followed
        view.setEdits(app.model.widgets());
        view.setFollowNode("region");
        QVERIFY(view.region().value("editable").toBool());
        QCOMPARE(view.region().value("multiple").toInt(), 16);
        // The preview is the node's: padded context kept inside the canvas, the model size on its multiple.
        const QVariantMap p = view.previewRegion({{"x", 10}, {"y", 10}, {"width", 100}, {"height", 40}});
        QCOMPARE(p.value("context").toMap().value("x").toInt(), 0); // 10 - 20, held at the edge
        QVERIFY(p.value("context").toMap().value("width").toInt() >= 130); // the node's own rule (may round up to its multiple)
        QCOMPARE(p.value("modelWidth").toInt() % 16, 0);
        QCOMPARE(p.value("modelHeight").toInt() % 16, 0);
        // Snap puts the edges on multiples; keepSize moves only the top left.
        const QVariantMap snapped = view.snapRegion({{"x", 21}, {"y", 37}, {"width", 100}, {"height", 45}});
        QCOMPARE(snapped.value("x").toInt() % 16, 0);
        QCOMPARE(snapped.value("y").toInt() % 16, 0);
        const QVariantMap kept = view.snapRegion({{"x", 21}, {"y", 37}, {"width", 100}, {"height", 45}}, true);
        QCOMPARE(kept.value("width").toInt(), 100);
        QCOMPARE(kept.value("height").toInt(), 45);
        // Previewing and snapping are not edits.
        QCOMPARE(app.model.undoStack()->count(), 0);
        // Writing: the four numbers, one undo step, a candidate shown or not.
        view.show(m_c1);
        QVERIFY(view.setRegion({{"x", 200}, {"y", 150}, {"width", 64}, {"height", 48}}));
        QCOMPARE(app.model.undoStack()->count(), 1);
        const QVariantMap now = app.model.widgets()->regionState("region");
        QCOMPARE(now.value("x").toInt(), 200);
        QCOMPARE(now.value("y").toInt(), 150);
        QCOMPARE(now.value("width").toInt(), 64);
        QCOMPARE(now.value("height").toInt(), 48);
        QCOMPARE(view.region().value("x").toInt(), 200); // the view follows at once
        QCOMPARE(now.value("revision").toString(), QString("head")); // drawing on a candidate does not repin the node
        QVERIFY(!view.setRegion({{"x", 200}, {"y", 150}, {"width", 64}, {"height", 48}})); // the same: nothing
        QCOMPARE(app.model.undoStack()->count(), 1);
        QVERIFY(view.setRegion({{"x", 201}, {"y", 150}, {"width", 64}, {"height", 48}}, true)); // snapped to 208
        QCOMPARE(app.model.widgets()->regionState("region").value("x").toInt() % 16, 0);
        app.model.undoStack()->undo();
        QTRY_COMPARE(view.region().value("x").toInt(), 200);
        app.model.undoStack()->undo();
        QTRY_COMPARE(view.region().value("x").toInt(), 100);
    }

    void aLinkedNumberLeavesTheRegionReadOnly() {
        const QString nodes = QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {"src": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 40}},
                      "region": {"type": "fugue/canvas-region", "version": "1.0.0",
                                 "inputs": {"canvas": "%1", "x": {"from": "src.value"}}}}},
  "layout": {"nodes": {"src": {"pos": [0, 0]}, "region": {"pos": [200, 0]}}, "view": {"center": [200, 200], "zoom": 1}}
})").arg(m_canvas);
        App app(load(nodes.toUtf8()));
        app.model.widgets()->canvasChoices()->setStoreRoot(m_dir.filePath("store"));
        QVERIFY(app.open());
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setEdits(app.model.widgets());
        view.setFollowNode("region");
        QVERIFY(view.region().value("valid").toBool());
        QVERIFY(!view.region().value("editable").toBool());
        QVERIFY(!view.setRegion({{"x", 10}, {"y", 10}, {"width", 64}, {"height", 48}}));
        QCOMPARE(app.model.undoStack()->count(), 0);
    }

    void aLinkedRevisionIsNotWritten() {
        const QString nodes = QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {"src": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "head"}},
                      "region": {"type": "fugue/canvas-region", "version": "1.0.0",
                                 "inputs": {"canvas": "%1", "revision": {"from": "src.text"}}}}},
  "layout": {"nodes": {"src": {"pos": [0, 0]}, "region": {"pos": [200, 0]}}, "view": {"center": [200, 200], "zoom": 1}}
})").arg(m_canvas);
        App app(load(nodes.toUtf8()));
        app.model.widgets()->canvasChoices()->setStoreRoot(m_dir.filePath("store"));
        QVERIFY(app.open());
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setEdits(app.model.widgets());
        view.setFollowNode("region");
        QVERIFY(view.region().value("valid").toBool());
        QVERIFY(!view.region().value("writable").toBool());
        view.show(m_c1);
        QVERIFY(!view.useForRegion());
        QCOMPARE(app.model.undoStack()->count(), 0);
    }
};

QTEST_MAIN(TestCanvasViewModel)
#include "test_canvas_view_model.moc"
