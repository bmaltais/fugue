// The backend of the Canvas view (#221, epics #216/#217, spec 14.5): the strip of what to look at, the overlay of a
// candidate on the head, the Canvas Region rectangle read through the edit layer, 'Region reads from this revision' as
// one undo step, and the refresh when the store changes. No window: the model alone, a store of the test's own.

#include <QQuickStyle>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QRegularExpression>
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

    // #239: a candidate (made apart, by the command line) is only listed: shown alone, read only, no overlay.
    void aCandidateIsShownAloneAgainstTheHead() {
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        QCOMPARE(view.pictureSource(), view.sourceB());
        view.show(m_c1);
        QCOMPARE(view.revisionB(), m_c1);
        QCOMPARE(view.revisionA(), m_head); // against the head
        QCOMPARE(view.pictureSource(), view.sourceB());
        QVERIFY(view.hasParent());
        QVERIFY(view.sourceDifference().isValid() && !view.sourceDifference().isEmpty());
        view.show(m_head);
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

    // The canvas as a whole (#232, File menu): export by extension, rename, duplicate, delete, clear.
    void exportWritesLayersOrAPictureByTheExtension() {
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        QDir().mkpath(m_dir.filePath("out"));
        for (const char *name : {"a.ora", "b.jpg", "c.webp", "d.JPEG"}) {
            QVERIFY2(view.exportPicture(m_dir.filePath(QString("out/") + name)), qPrintable(view.error()));
            QVERIFY(QFileInfo(m_dir.filePath(QString("out/") + name)).size() > 0);
        }
        QVERIFY(view.exportPicture(m_dir.filePath("out/e.txt"))); // another extension is a PNG
        QVERIFY(QFileInfo(m_dir.filePath("out/e.txt.png")).exists());
        QFile ora(m_dir.filePath("out/a.ora"));
        QVERIFY(ora.open(QIODevice::ReadOnly));
        QCOMPARE(ora.read(2), QByteArray("PK")); // a zip: the layers file
    }

    void renamingDuplicatingAndDeletingACanvas() {
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        QVERIFY(view.renameCanvas("Poster"));
        QCOMPARE(store().info(m_canvas)->name, QString("Poster"));
        QVERIFY(!view.renameCanvas("  "));
        QVERIFY(!view.error().isEmpty());
        QVERIFY(view.duplicateCanvas());
        const QString copy = view.canvasId();
        QVERIFY(copy != m_canvas);
        QCOMPARE(store().info(copy)->name, QString("Poster copy"));
        QCOMPARE(view.canvases().size(), 2);
        QVERIFY(view.deleteCanvas()); // the copy goes; the original is shown
        QCOMPARE(view.canvasId(), m_canvas);
        QCOMPARE(view.canvases().size(), 1);
        QVERIFY(view.deleteCanvas()); // the last one: the empty state
        QCOMPARE(view.canvasId(), QString());
        QCOMPARE(view.canvases().size(), 0);
    }

    void clearingTheCanvasIsOneUndoableStepOfTheHead() {
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        const QString before = view.revisionB();
        QVERIFY(view.clearCanvas());
        QVERIFY(view.revisionB() != before);
        QCOMPARE(view.layers().size(), 1);
        QCOMPARE(store().log(m_canvas)->size(), size_t(2));
        QVERIFY(view.undo());
        QCOMPARE(view.revisionB(), before);
        view.show(m_c1); // a candidate is shown: the head is not editable
        QVERIFY(!view.clearCanvas());
    }

    // The History panel's steps (#228, the owner's decision): what changed, in words, newest first, no hash; the
    // candidates listed only with all revisions.
    void theHistoryNamesEachStepInWords() {
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        const QString added = store().commit(m_canvas, withFill(m_base, "A wooden table", 200), m_head, "canvas-write:w").value().revision.id;
        view.refresh();
        QCOMPARE(view.head(), added);
        const int top = 0; // newest first: the table's layer sits on top
        QVERIFY(view.setOpacity({top + 1}, 0.5));
        QVERIFY(view.setVisible({0}, false));
        QVERIFY(view.removeLayers({0}));
        QStringList names;
        for (const QVariant &step : view.steps())
            names << step.toMap().value("name").toString();
        QCOMPARE(names, (QStringList{"Layer deleted: Base", "Hidden: Base", "Opacity 50%: A wooden table", "Layer added: A wooden table", "Canvas made"}));
        QVERIFY(view.steps().first().toMap().value("latest").toBool());
        for (const QString &name : names)
            QVERIFY2(!name.contains(QRegularExpression("[0-9a-f]{8}")), qPrintable(name));
        QCOMPARE(view.allSteps().size(), view.steps().size() + 2); // the two candidates, first
        QVERIFY(view.allSteps().first().toMap().value("name").toString().startsWith("Candidate: Layer added: Fill"));
    }

    void aStoreChangeShowsWithinAMoment() {
        CanvasViewModel view;
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

    // A candidate a run adds is added the way the command line adds one (addRevision from the head, no head move).
    QString arrive(const QString &name, int x) {
        return store().addRevision(m_canvas, withFill(m_base, name, x), m_head, "canvas-write:" + name).value().id;
    }

    // #239: nothing moves the view on its own for a candidate any more: it only joins the strip.
    void aNewCandidateJoinsTheListAndTheViewStaysOnTheHead() {
        CanvasViewModel view;
        view.setStoreRoot(m_dir.filePath("store"));
        view.setCanvasId(m_canvas);
        QCOMPARE(view.revisionB(), m_head);
        const QString c3 = arrive("c3", 200);
        QTRY_VERIFY_WITH_TIMEOUT(ids(view.choices()).contains(c3), 3000);
        QTest::qWait(300);
        QCOMPARE(view.revisionB(), m_head);
        QVERIFY(view.headNotice().isEmpty());
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
