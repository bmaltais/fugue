// Layer operations on a canvas (#226, epic #206, spec 14.5): each is a pure edit of a tree (no pixel
// read or stored, the tiles shared), stored as ONE new revision whose parent is the one edited, with
// the head moved by compare-and-swap; undo moves the head back to the parent and redo forward again;
// the tree hash covers every layer property and nothing else.
//
// Seam: engine/canvas_layer_ops.h (pure) and CanvasStore::editLayers, undo, redo, with a folder and a
// clock of the test's own.

#include <QDirIterator>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/canonical.h"
#include "engine/canvas_diff.h"
#include "engine/canvas_layer_ops.h"
#include "engine/canvas_store.h"

using namespace fugue;
using namespace fugue::canvas;
using namespace fugue::mask;

namespace {

MaskPaint solid(int w, int h, Rgba c) {
    MaskPaint m(w, h, 4);
    m.fill(c);
    return m;
}

// Three layers (bottom first): Base, Fill 1 (with a layer mask), Fill 2.
PaintDocument sample() {
    PaintDocument d(300, 300);
    d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(300, 300, {40, 80, 120, 255}));
    LayerInfo fill;
    fill.name = "Fill 1";
    fill.producer = "canvas-write:write";
    fill.created = "2026-11-22T10:00:00.000Z";
    const MaskPaint mask(300, 300, std::vector<uint8_t>(300 * 300, 128), 1);
    d.loadLayer(fill, solid(300, 300, {200, 30, 30, 255}), &mask);
    d.loadLayer(LayerKind::Paint, "Fill 2", true, 1, solid(300, 300, {10, 200, 10, 90}));
    return d;
}

LayerEdit edit(LayerOp op, std::vector<int> layers) {
    LayerEdit e;
    e.op = op;
    e.layers = std::move(layers);
    return e;
}

QStringList names(const Tree &t) {
    QStringList out;
    for (const LayerRecord &l : t.layers)
        out << l.name;
    return out;
}

int fileCount(const QString &folder) {
    int n = 0;
    QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++n;
    }
    return n;
}

} // namespace

class TestCanvasLayerOps : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    int m_tick = 0;
    Clock clock() { return [this] { return QDateTime(QDate(2026, 11, 22), QTime(12, 0, 0), QTimeZone::utc()).addSecs(m_tick++); }; }
    CanvasStore store() { return CanvasStore(m_dir.path(), clock()); }
    Tree sampleTree() { return treeOf(sample()).value(); }

private slots:
    // ---- the pure edits ---------------------------------------------------------------------------

    void eachOpChangesOnlyWhatItSays() {
        const Tree t = sampleTree();
        LayerEdit hide = edit(LayerOp::Visible, {0, 2});
        hide.flag = false;
        const Tree hidden = applyLayerEdit(t, hide).value();
        QVERIFY(!hidden.layers[0].visible && hidden.layers[1].visible && !hidden.layers[2].visible);
        QCOMPARE(hidden.layers[0].tiles, t.layers[0].tiles);

        LayerEdit opacity = edit(LayerOp::Opacity, {1});
        opacity.number = 0.25;
        QCOMPARE(applyLayerEdit(t, opacity)->layers[1].opacity, 0.25);
        opacity.number = 1.5;
        QCOMPARE(applyLayerEdit(t, opacity).error().code, QStringLiteral("input_invalid"));

        LayerEdit blend = edit(LayerOp::Blend, {1});
        blend.text = "multiply";
        QCOMPARE(applyLayerEdit(t, blend)->layers[1].compositeOp, QStringLiteral("svg:multiply"));
        blend.text = "normal";
        QCOMPARE(applyLayerEdit(t, blend)->layers[1].compositeOp, QString());
        blend.text = "dodge";
        QVERIFY(!applyLayerEdit(t, blend).ok());

        LayerEdit rename = edit(LayerOp::Rename, {2});
        rename.text = "  Sky  ";
        QCOMPARE(names(applyLayerEdit(t, rename).value()), (QStringList{"Base", "Fill 1", "Sky"}));
        rename.text = " ";
        QVERIFY(!applyLayerEdit(t, rename).ok());
        rename.layers = {1, 2};
        rename.text = "Two";
        QVERIFY(!applyLayerEdit(t, rename).ok());

        QCOMPARE(names(applyLayerEdit(t, edit(LayerOp::Remove, {0, 2})).value()), (QStringList{"Fill 1"}));
        QVERIFY(!applyLayerEdit(t, edit(LayerOp::Remove, {3})).ok());
        QVERIFY(!applyLayerEdit(t, edit(LayerOp::Remove, {1, 1})).ok());
        QVERIFY(!applyLayerEdit(t, edit(LayerOp::Remove, {})).ok());
    }

    void moveTakesTheChosenLayersAsABlock() {
        const Tree t = sampleTree();
        LayerEdit move = edit(LayerOp::Move, {0});
        move.to = 2;
        QCOMPARE(names(applyLayerEdit(t, move).value()), (QStringList{"Fill 1", "Fill 2", "Base"}));
        move.to = 99; // clamped to the top
        QCOMPARE(names(applyLayerEdit(t, move).value()), (QStringList{"Fill 1", "Fill 2", "Base"}));
        move = edit(LayerOp::Move, {1, 2});
        move.to = 0;
        QCOMPARE(names(applyLayerEdit(t, move).value()), (QStringList{"Fill 1", "Fill 2", "Base"}));
        move.to = 1; // already there: the same tree
        QCOMPARE(treeToJson(applyLayerEdit(t, move).value()), treeToJson(t));
    }

    void aDuplicateSharesItsTilesAndMaskAndGoesAboveIt() {
        const Tree t = sampleTree();
        const Tree two = applyLayerEdit(t, edit(LayerOp::Duplicate, {1, 2})).value();
        QCOMPARE(names(two), (QStringList{"Base", "Fill 1", "Fill 1 copy", "Fill 2", "Fill 2 copy"}));
        QCOMPARE(two.layers[2].tiles, t.layers[1].tiles);
        QCOMPARE(two.layers[2].maskTiles, t.layers[1].maskTiles);
        QVERIFY(two.layers[2].hasMask);
        QCOMPARE(two.layers[2].producer, QStringLiteral("canvas-write:write"));
        const Tree three = applyLayerEdit(two, edit(LayerOp::Duplicate, {1})).value();
        QCOMPARE(three.layers[2].name, QStringLiteral("Fill 1 copy 2"));
    }

    void aLayerMaskIsTurnedOffOrDeleted() {
        const Tree t = sampleTree();
        LayerEdit off = edit(LayerOp::MaskEnabled, {1});
        off.flag = false;
        const Tree disabled = applyLayerEdit(t, off).value();
        QVERIFY(disabled.layers[1].hasMask);
        QVERIFY(!disabled.layers[1].maskEnabled);
        const Tree gone = applyLayerEdit(t, edit(LayerOp::RemoveMask, {1})).value();
        QVERIFY(!gone.layers[1].hasMask);
        QVERIFY(gone.layers[1].maskTiles.empty());
        QCOMPARE(applyLayerEdit(t, edit(LayerOp::RemoveMask, {0})).error().code, QStringLiteral("input_invalid"));
    }

    void theOpNamesRoundTrip() {
        for (LayerOp op : {LayerOp::Visible, LayerOp::Opacity, LayerOp::Blend, LayerOp::Rename, LayerOp::Remove, LayerOp::Move, LayerOp::Duplicate,
                           LayerOp::MaskEnabled, LayerOp::RemoveMask}) {
            LayerOp back = LayerOp::Visible;
            QVERIFY(layerOpFromName(layerOpName(op), &back));
            QCOMPARE(int(back), int(op));
        }
        QCOMPARE(layerOpName(LayerOp::Remove), QStringLiteral("delete"));
    }

    // ---- the tree hash ----------------------------------------------------------------------------

    // A property at its default is not written, so a tree from before blend, masks and origins has the
    // hash it always had; a non-default one is part of the hash.
    void theTreeHashCoversEveryPropertyAndKeepsOldHashes() {
        PaintDocument plain(10, 10);
        plain.loadLayer(LayerKind::Paint, "Base", true, 1, solid(10, 10, {1, 2, 3, 255}));
        const QJsonObject json = treeToJson(treeOf(plain).value());
        const QJsonObject layer = json.value("layers").toArray().at(0).toObject();
        const QStringList keys = layer.keys();
        QCOMPARE(QSet<QString>(keys.begin(), keys.end()), (QSet<QString>{"name", "kind", "visible", "opacity", "tiles"}));

        const Tree t = sampleTree();
        const QString base = canonicalHash(treeToJson(t));
        QStringList seen{base};
        for (const LayerEdit &e : [] {
                 LayerEdit hide = edit(LayerOp::Visible, {0});
                 LayerEdit blend = edit(LayerOp::Blend, {0});
                 blend.text = "screen";
                 LayerEdit opacity = edit(LayerOp::Opacity, {0});
                 opacity.number = 0.5;
                 LayerEdit off = edit(LayerOp::MaskEnabled, {1});
                 LayerEdit rename = edit(LayerOp::Rename, {0});
                 rename.text = "Ground";
                 return std::vector<LayerEdit>{hide, blend, opacity, off, rename, edit(LayerOp::RemoveMask, {1})};
             }()) {
            const QString hash = canonicalHash(treeToJson(applyLayerEdit(t, e).value()));
            QVERIFY2(!seen.contains(hash), qPrintable(layerOpName(e.op)));
            seen << hash;
        }
        // The tree read back from its JSON is the same tree.
        QCOMPARE(treeToJson(treeFromJson(treeToJson(t)).value()), treeToJson(t));
        QVERIFY(treeFromJson(treeToJson(t))->layers[1].hasMask);
    }


    // The diff names what an edit did: a rename is the same layer, a blend or mask edit a property.
    void theDiffNamesARenameABlendAndAMaskEdit() {
        const Tree t = sampleTree();
        LayerEdit rename = edit(LayerOp::Rename, {1});
        rename.text = "Red";
        const TreeDiff renamed = diffTrees(t, applyLayerEdit(t, rename).value());
        QCOMPARE(renamed.layers.size(), size_t(1));
        QCOMPARE(int(renamed.layers[0].change), int(LayerChange::Changed));
        QCOMPARE(renamed.layers[0].properties, QStringList{"name"});
        QCOMPARE(renamed.layers[0].tilesChanged, 0);
        LayerEdit blend = edit(LayerOp::Blend, {2});
        blend.text = "overlay";
        QCOMPARE(diffTrees(t, applyLayerEdit(t, blend).value()).layers[0].properties, QStringList{"blend"});
        QCOMPARE(diffTrees(t, applyLayerEdit(t, edit(LayerOp::RemoveMask, {1})).value()).layers[0].properties, QStringList{"mask"});
        QCOMPARE(int(diffTrees(t, applyLayerEdit(t, edit(LayerOp::Remove, {2})).value()).layers[0].change), int(LayerChange::Removed));
    }

    // ---- stored as revisions ----------------------------------------------------------------------

    void eachEditIsOneRevisionOnTheHeadAndAddsNoTile() {
        CanvasStore s = store();
        const CanvasInfo made = s.create("layers", sample(), "test").value();
        const int blobs = fileCount(m_dir.path() + "/blobs");
        LayerEdit hide = edit(LayerOp::Visible, {2});
        const Commit first = s.editLayers(made.id, made.head, hide, "canvas-view:visible").value();
        QVERIFY(first.move.moved);
        QCOMPARE(first.revision.parent, made.head);
        QCOMPARE(first.revision.producer, QStringLiteral("canvas-view:visible"));
        QCOMPARE(s.info(made.id)->head, first.revision.id);
        QCOMPARE(fileCount(m_dir.path() + "/blobs"), blobs);
        QVERIFY(!s.load(made.id, first.revision.id)->layer(2).visible);
        QVERIFY(s.load(made.id, first.revision.id)->layer(1).hasMask);

        // Showing it again is a new revision with the old tree: same pixels and properties, same tree hash.
        hide.flag = true;
        const Commit again = s.editLayers(made.id, first.revision.id, hide, "canvas-view:visible").value();
        QVERIFY(again.move.moved);
        QCOMPARE(again.revision.tree, s.revision(made.id, made.head)->tree);
        QVERIFY(again.revision.id != made.head);
        QCOMPARE(s.log(made.id)->size(), size_t(3));
    }

    void anEditThatChangesNothingOrIsNotOnTheHeadWritesNothing() {
        CanvasStore s = store();
        const CanvasInfo made = s.create("layers", sample(), "test").value();
        const int commits = fileCount(m_dir.path() + "/canvases/" + made.id + "/commits");
        LayerEdit show = edit(LayerOp::Visible, {0});
        show.flag = true; // visible already
        const Commit same = s.editLayers(made.id, made.head, show, "canvas-view:visible").value();
        QVERIFY(!same.move.moved);
        QCOMPARE(same.revision.id, made.head);
        QCOMPARE(same.move.head, made.head);

        LayerEdit hide = edit(LayerOp::Visible, {0});
        const Commit moved = s.editLayers(made.id, made.head, hide, "canvas-view:visible").value();
        // The old revision is not the head now: an edit of it is refused, with what the head is.
        const Commit stale = s.editLayers(made.id, made.head, edit(LayerOp::Remove, {0}), "canvas-view:delete").value();
        QVERIFY(!stale.move.moved);
        QCOMPARE(stale.move.head, moved.revision.id);
        QCOMPARE(fileCount(m_dir.path() + "/canvases/" + made.id + "/commits"), commits + 1);
        QVERIFY(s.candidates(made.id)->empty());
        QVERIFY(!s.editLayers(made.id, moved.revision.id, edit(LayerOp::Remove, {7}), "canvas-view:delete").ok());
    }

    void undoGoesBackAlongTheChainAndRedoForward() {
        CanvasStore s = store();
        const CanvasInfo made = s.create("layers", sample(), "test").value();
        const Commit a = s.editLayers(made.id, made.head, edit(LayerOp::Visible, {0}), "canvas-view:visible").value();
        LayerEdit rename = edit(LayerOp::Rename, {1});
        rename.text = "Red";
        const Commit b = s.editLayers(made.id, a.revision.id, rename, "canvas-view:rename").value();
        QCOMPARE(s.redoTarget(made.id).value(), QString());

        QVERIFY(s.undo(made.id, b.revision.id)->moved);
        QCOMPARE(s.info(made.id)->head, a.revision.id);
        QVERIFY(s.undo(made.id, a.revision.id)->moved);
        QCOMPARE(s.info(made.id)->head, made.head);
        // An undone revision is not a candidate for review.
        QVERIFY(s.candidates(made.id)->empty());
        QCOMPARE(s.undo(made.id, made.head).error().code, QStringLiteral("canvas_no_step"));
        // Undo from a head that moved meanwhile does nothing.
        QVERIFY(!s.undo(made.id, b.revision.id)->moved);

        QCOMPARE(s.redoTarget(made.id).value(), a.revision.id);
        QVERIFY(s.redo(made.id, made.head)->moved);
        QVERIFY(s.redo(made.id, a.revision.id)->moved);
        QCOMPARE(s.info(made.id)->head, b.revision.id);
        QCOMPARE(s.redo(made.id, b.revision.id).error().code, QStringLiteral("canvas_no_step"));

        // A new edit after an undo: redo then goes to the newest one.
        QVERIFY(s.undo(made.id, b.revision.id)->moved);
        LayerEdit other = edit(LayerOp::Duplicate, {2});
        const Commit c = s.editLayers(made.id, a.revision.id, other, "canvas-view:duplicate").value();
        QVERIFY(s.undo(made.id, c.revision.id)->moved);
        QCOMPARE(s.redoTarget(made.id).value(), c.revision.id);
    }

    // A candidate made from an older revision is rebased with its added layer whole: blend, mask, origin.
    void aRebasedAcceptKeepsTheAddedLayersMaskAndBlend() {
        CanvasStore s = store();
        const CanvasInfo made = s.create("layers", sample(), "test").value();
        PaintDocument fill = s.load(made.id, made.head).value();
        LayerInfo info;
        info.name = "Fill 3";
        info.compositeOp = "svg:screen";
        info.producer = "canvas-write:write";
        const MaskPaint mask(300, 300, std::vector<uint8_t>(300 * 300, 77), 1);
        fill.loadLayer(info, solid(300, 300, {1, 2, 3, 255}), &mask);
        const Revision candidate = s.addRevision(made.id, fill, made.head, "canvas-write:write").value();
        QVERIFY(s.editLayers(made.id, made.head, edit(LayerOp::Visible, {0}), "canvas-view:visible")->move.moved);
        const Accepted done = s.accept(made.id, candidate.id).value();
        QVERIFY(done.rebased && done.moved);
        const PaintDocument head = s.load(made.id, done.head.id).value();
        const LayerInfo top = head.layer(head.layerCount() - 1);
        QCOMPARE(top.compositeOp, QStringLiteral("svg:screen"));
        QVERIFY(top.hasMask);
        QCOMPARE(head.maskPixels(top.id)->at(5, 5), uint8_t(77));
        QCOMPARE(top.producer, QStringLiteral("canvas-write:write"));
        QVERIFY(!head.layer(0).visible); // the head's own edit stays
    }
};

QTEST_GUILESS_MAIN(TestCanvasLayerOps)
#include "test_canvas_layer_ops.moc"
