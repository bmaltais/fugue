// The layers of the Canvas view (#226, spec 14.5), the model alone: the shown revision's layers top first with their
// thumbnails, a selection an outliner keeps (ctrl and shift), each edit ONE revision of the head that the view moves on
// to, undo and redo along the head's parents, solo as a view-only state, and nothing editable off the head.
// No window: CanvasViewModel on a store of the test's own.

#include <QFile>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/canvas_view_model.h"
#include "engine/canvas_store.h"

using namespace fugue;
using namespace fugue::mask;

namespace {

MaskPaint solid(int w, int h, Rgba c) {
    MaskPaint m(w, h, 4);
    m.fill(c);
    return m;
}

// Base (0), Fill 1 with a layer mask (1), Fill 2 (2).
PaintDocument sample() {
    PaintDocument d(256, 256);
    d.loadLayer(LayerKind::Paint, "Base", true, 1, solid(256, 256, {30, 60, 200, 255}));
    LayerInfo fill;
    fill.name = "Fill 1";
    fill.producer = "canvas-write:write";
    fill.created = "2026-11-22T10:00:00.000Z";
    const MaskPaint mask(256, 256, std::vector<uint8_t>(256 * 256, 255), 1);
    d.loadLayer(fill, solid(256, 256, {200, 10, 10, 255}), &mask);
    d.loadLayer(LayerKind::Paint, "Fill 2", true, 1, solid(256, 256, {10, 200, 10, 128}));
    return d;
}

QStringList names(const QVariantList &rows) {
    QStringList out;
    for (const QVariant &row : rows)
        out << row.toMap().value("name").toString();
    return out;
}

QVariantList at(std::initializer_list<int> positions) {
    QVariantList out;
    for (int p : positions)
        out << p;
    return out;
}

} // namespace

class TestCanvasLayersModel : public QObject {
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

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void init() {
        QDir(m_dir.filePath("store")).removeRecursively();
        const auto made = store().create("Layers", sample(), "new");
        QVERIFY(made);
        m_canvas = made->id;
        m_head = made->head;
    }

    void theLayersAreListedTopFirstWithThumbnailsAndTheTopIsCurrent() {
        CanvasViewModel model;
        open(model);
        QCOMPARE(names(model.layers()), (QStringList{"Fill 2", "Fill 1", "Base"}));
        const QVariantMap fill = model.layers().at(1).toMap();
        QCOMPARE(fill.value("position").toInt(), 1);
        QVERIFY(fill.value("hasMask").toBool());
        QVERIFY(fill.value("maskEnabled").toBool());
        QCOMPARE(fill.value("blend").toString(), QString("normal"));
        QCOMPARE(fill.value("from").toString(), QString("Canvas Write write"));
        QCOMPARE(fill.value("depth").toInt(), 0);
        QVERIFY(QFile::exists(fill.value("thumbnail").toUrl().toLocalFile()));
        QVERIFY(QFile::exists(fill.value("maskThumbnail").toUrl().toLocalFile()));
        QVERIFY(model.layers().at(0).toMap().value("maskThumbnail").toUrl().isEmpty());
        QCOMPARE(model.selection(), at({2}));
        QVERIFY(model.layersEditable());
        QVERIFY(!model.canUndo());
        QCOMPARE(model.blendModes().size(), 8);
    }

    void theSelectionIsAnOutliners() {
        CanvasViewModel model;
        open(model);
        model.selectLayer(0);
        QCOMPARE(model.selection(), at({0}));
        model.selectLayer(2, "toggle");
        QCOMPARE(model.selection(), at({0, 2}));
        model.selectLayer(0, "toggle");
        QCOMPARE(model.selection(), at({2}));
        model.selectLayer(2);
        model.selectLayer(0, "range"); // from the last one picked
        QCOMPARE(model.selection(), at({0, 1, 2}));
        model.selectNone();
        QVERIFY(model.selection().isEmpty());
    }

    void eachEditIsOneRevisionAndTheViewMovesOnToIt() {
        CanvasViewModel model;
        open(model);
        QVERIFY(model.setVisible(at({0, 2}), false));
        QCOMPARE(revisionCount(), 2);
        const QString first = model.revisionB();
        QVERIFY(first != m_head);
        QCOMPARE(model.head(), first);
        QVERIFY(!model.layers().at(0).toMap().value("visible").toBool());
        QVERIFY(!model.layers().at(2).toMap().value("visible").toBool());
        QCOMPARE(store().revision(m_canvas, first)->producer, QString("canvas-view:visible"));
        QCOMPARE(store().revision(m_canvas, first)->parent, m_head);
        QVERIFY(model.canUndo());

        QVERIFY(model.setOpacity(at({1}), 0.4));
        QCOMPARE(model.layers().at(1).toMap().value("opacity").toDouble(), 0.4);
        QVERIFY(model.setBlend(at({1}), "multiply"));
        QCOMPARE(model.layers().at(1).toMap().value("blend").toString(), QString("multiply"));
        QVERIFY(model.rename(1, "Red"));
        QCOMPARE(model.layers().at(1).toMap().value("name").toString(), QString("Red"));
        QVERIFY(model.setMaskEnabled(at({1}), false));
        QVERIFY(!model.layers().at(1).toMap().value("maskEnabled").toBool());
        QVERIFY(model.removeMask(at({1})));
        QVERIFY(!model.layers().at(1).toMap().value("hasMask").toBool());
        QCOMPARE(revisionCount(), 7);
        // The same value again is no revision.
        QVERIFY(!model.setOpacity(at({1}), 0.4));
        QCOMPARE(revisionCount(), 7);
        QVERIFY(model.error().isEmpty());
    }

    void duplicateMoveAndDeleteKeepAUsefulSelection() {
        CanvasViewModel model;
        open(model);
        QVERIFY(model.duplicate(at({1})));
        QCOMPARE(names(model.layers()), (QStringList{"Fill 2", "Fill 1 copy", "Fill 1", "Base"}));
        QCOMPARE(model.selection(), at({2})); // the copy
        QVERIFY(model.raise(model.selection()));
        QCOMPARE(names(model.layers()), (QStringList{"Fill 1 copy", "Fill 2", "Fill 1", "Base"}));
        QCOMPARE(model.selection(), at({3}));
        QVERIFY(!model.raise(model.selection())); // at the top already: the same tree, no revision
        QVERIFY(model.lower(at({3})));
        QVERIFY(model.lower(at({2})));
        QCOMPARE(names(model.layers()), (QStringList{"Fill 2", "Fill 1", "Fill 1 copy", "Base"}));
        // A drag: the line above the top row takes Base to the top.
        QVERIFY(model.dropAt(at({0}), 0));
        QCOMPARE(names(model.layers()), (QStringList{"Base", "Fill 2", "Fill 1", "Fill 1 copy"}));
        QCOMPARE(model.selection(), at({3}));
        QVERIFY(model.removeLayers(at({3, 2})));
        QCOMPARE(names(model.layers()), (QStringList{"Fill 1", "Fill 1 copy"}));
        QCOMPARE(model.selection(), at({1}));
    }

    void undoAndRedoWalkTheHead() {
        CanvasViewModel model;
        open(model);
        QVERIFY(model.setVisible(at({0}), false));
        QVERIFY(model.rename(2, "Top"));
        const QString last = model.revisionB();
        QVERIFY(model.undo());
        QCOMPARE(model.layers().at(0).toMap().value("name").toString(), QString("Fill 2"));
        QVERIFY(model.canRedo());
        QVERIFY(model.undo());
        QCOMPARE(model.revisionB(), m_head);
        QVERIFY(!model.canUndo());
        QVERIFY(!model.undo());
        QVERIFY(model.redo());
        QVERIFY(model.redo());
        QCOMPARE(model.revisionB(), last);
        QVERIFY(!model.canRedo());
        // An undone edit is not a candidate: nothing waits for review.
        QVERIFY(model.undo());
        QVERIFY(store().candidates(m_canvas)->empty());
    }

    void soloIsAViewOnlyStateAndNeverARevision() {
        CanvasViewModel model;
        open(model);
        const QUrl whole = model.pictureSource();
        model.setSolo(0);
        QCOMPARE(model.solo(), 0);
        QVERIFY(model.pictureSource() != whole);
        QVERIFY(QFile::exists(model.pictureSource().toLocalFile()));
        QCOMPARE(revisionCount(), 1);
        model.setSolo(0); // the same layer again ends it
        QCOMPARE(model.solo(), -1);
        QCOMPARE(model.pictureSource(), whole);
        model.setSolo(1);
        QVERIFY(model.setOpacity(at({1}), 0.5)); // another revision: solo ends
        QCOMPARE(model.solo(), -1);
    }

    void onlyTheHeadIsEdited() {
        CanvasViewModel model;
        open(model);
        QVERIFY(model.setVisible(at({0}), false));
        model.show(m_head); // an older revision
        QTRY_COMPARE(model.revisionB(), m_head);
        QVERIFY(!model.layersEditable());
        QVERIFY(!model.canUndo());
        QVERIFY(!model.setVisible(at({1}), false));
        QCOMPARE(revisionCount(), 2);
        QCOMPARE(names(model.layers()), (QStringList{"Fill 2", "Fill 1", "Base"})); // its layers are shown, read only
    }

    // ---- #252: the Layer menu's operations, each one revision and one undo ----------------------------------
    void newLayerGoesAboveTheCurrentOneAndIsSelected() {
        CanvasViewModel model;
        open(model);
        model.selectLayer(1); // Fill 1
        QVERIFY(model.newLayer());
        QCOMPARE(names(model.layers()), (QStringList{"Fill 2", "Layer 4", "Fill 1", "Base"}));
        QCOMPARE(model.selection(), at({2}));
        QCOMPARE(revisionCount(), 2);
        QVERIFY(model.undo());
        QCOMPARE(names(model.layers()), (QStringList{"Fill 2", "Fill 1", "Base"}));
    }

    void lockingIsOneRevisionAndAProtectedLayerRefusesTheEditsThatWouldChangeIt() {
        CanvasViewModel model;
        open(model);
        QVERIFY(model.setLocked(at({1}), true));
        QCOMPARE(revisionCount(), 2);
        QVERIFY(model.layers().at(1).toMap().value("locked").toBool()); // top first: Fill 2, Fill 1, Base
        QVERIFY(!model.layers().at(0).toMap().value("locked").toBool());
        QVERIFY(!model.removeLayers(at({1})));
        QVERIFY(!model.mergeDown(2)); // into the locked one
        QVERIFY(!model.targetMask(1)); // the brush does not take a locked layer's mask
        QVERIFY(!model.error().isEmpty());
        QCOMPARE(revisionCount(), 2);
        QVERIFY(model.setVisible(at({1}), false)); // showing is not a pixel edit
        QVERIFY(model.toggleLocked(at({1})));      // all locked: unlocks
        QVERIFY(!model.layers().at(1).toMap().value("locked").toBool());
        QVERIFY(model.removeLayers(at({1})));
    }

    void aGenerateReadsOneNamedPaintLayerAsAViewStateNotARevision() {
        CanvasViewModel model;
        open(model);
        QVERIFY(model.regionLayer().isEmpty());
        QVERIFY(model.setRegionLayer(1));
        QCOMPARE(model.regionLayer(), QStringLiteral("Fill 1"));
        QCOMPARE(revisionCount(), 1);
        QVERIFY(model.setRegionLayer(1)); // again: the whole picture
        QVERIFY(model.regionLayer().isEmpty());
        QVERIFY(model.setRegionLayer(0));
        QVERIFY(model.rename(0, QStringLiteral("Ground"))); // the name is gone from the new revision: back to the picture
        QVERIFY(model.regionLayer().isEmpty());
        QVERIFY(model.setRegionLayer(2));
        QVERIFY(model.groupLayers(at({0, 1}))); // the layer is still there, in the group's neighbour
        QCOMPARE(model.regionLayer(), QStringLiteral("Fill 2"));
        QVERIFY(model.setRegionLayer(-1));
        QVERIFY(model.regionLayer().isEmpty());
        QVERIFY(!model.setRegionLayer(0)); // a group
        QVERIFY(!model.error().isEmpty());
    }

    void mergeDownFlattenGroupAndUngroupAreOneRevisionEach() {
        CanvasViewModel model;
        open(model);
        QVERIFY(model.mergeDown(2));
        QCOMPARE(names(model.layers()), (QStringList{"Fill 1", "Base"}));
        QCOMPARE(model.selection(), at({1}));
        QCOMPARE(revisionCount(), 2);
        QVERIFY(model.undo());
        QCOMPARE(names(model.layers()), (QStringList{"Fill 2", "Fill 1", "Base"}));
        QVERIFY(!model.mergeDown(0)); // nothing under the bottom layer

        QVERIFY(model.groupLayers(at({1, 2})));
        QCOMPARE(names(model.layers()), (QStringList{"Group 4", "Base"}));
        QCOMPARE(model.layers().first().toMap().value("kind").toString(), QStringLiteral("group"));
        QCOMPARE(model.layers().first().toMap().value("children").toInt(), 2);
        QCOMPARE(model.selection(), at({1}));
        QVERIFY(model.ungroup(at({1})));
        QCOMPARE(names(model.layers()), (QStringList{"Fill 2", "Fill 1", "Base"}));
        QCOMPARE(model.selection(), at({1, 2}));
        QVERIFY(!model.ungroup(at({0}))); // Base is no group

        QVERIFY(model.flatten());
        QCOMPARE(names(model.layers()), (QStringList{"Background"}));
        QVERIFY(!model.flatten()); // flat already
        QVERIFY(model.error().isEmpty());
    }

    // #278: a group is a row with an arrow; open, its layers follow it indented, and they are edited in place.
    void aGroupOpensIntoItsLayersWhichAreEditedInPlace() {
        CanvasViewModel model;
        open(model);
        QVERIFY(model.groupLayers(at({1, 2}))); // rows: Group 4 (id 1), Base (0); inside: Fill 1, Fill 2
        QVERIFY(!model.toggleOpen(0)); // Base is no group
        QVERIFY(model.toggleOpen(1));
        QCOMPARE(names(model.layers()), (QStringList{"Group 4", "Fill 2", "Fill 1", "Base"}));
        const QVariantMap fill2 = model.layers().at(1).toMap(), fill1 = model.layers().at(2).toMap();
        QCOMPARE(fill2.value("depth").toInt(), 1);
        QCOMPARE(fill2.value("position").toInt(), 2); // ids after the top level's, the same open or closed
        QVERIFY(QFile::exists(fill1.value("thumbnail").toUrl().toLocalFile()));
        QVERIFY(QFile::exists(fill1.value("maskThumbnail").toUrl().toLocalFile())); // Fill 1 has a layer mask

        // A layer inside the group is hidden, renamed and duplicated in place; each is one revision; the group stays open.
        const int revisions = revisionCount();
        model.selectLayer(2);
        QVERIFY(model.setVisible(at({2}), false));
        QVERIFY(!model.layers().at(1).toMap().value("visible").toBool());
        QCOMPARE(model.selection(), at({2}));
        QVERIFY(model.rename(2, "Sky"));
        QCOMPARE(names(model.layers()), (QStringList{"Group 4", "Sky", "Fill 1", "Base"}));
        QVERIFY(model.duplicate(at({2})));
        QCOMPARE(names(model.layers()), (QStringList{"Group 4", "Sky copy", "Sky", "Fill 1", "Base"}));
        QCOMPARE(model.selection(), at({2})); // the copy is the one selected
        QCOMPARE(revisionCount(), revisions + 3);

        // Move it down within the group, then delete it: the layer under it is selected.
        QVERIFY(model.lower(at({2})));
        QCOMPARE(names(model.layers()), (QStringList{"Group 4", "Sky", "Sky copy", "Fill 1", "Base"}));
        QVERIFY(model.removeLayers(model.selection()));
        QCOMPARE(names(model.layers()), (QStringList{"Group 4", "Sky", "Fill 1", "Base"}));

        // A pick across groups is changed one group at a time, with a word about it.
        QVERIFY(!model.setOpacity(at({2, 0}), 0.5));
        QVERIFY(!model.error().isEmpty());

        // A closing group takes the selection of what it hides.
        model.selectLayer(2);
        QVERIFY(model.setOpen(1, false));
        QCOMPARE(model.selection(), at({1}));
        QCOMPARE(names(model.layers()), (QStringList{"Group 4", "Base"}));
        QCOMPARE(model.layers().first().toMap().value("children").toInt(), 2);
    }

    void aDropStaysInTheGroupOfTheLayersDragged() {
        CanvasViewModel model;
        open(model);
        QVERIFY(model.groupLayers(at({1, 2})));
        QVERIFY(model.toggleOpen(1)); // rows: Group 4, Fill 2 (id 2), Fill 1 (id 3), Base
        QVERIFY(model.canDropAt(at({3}), 1)); // Fill 1 above Fill 2: in the group
        QVERIFY(!model.canDropAt(at({3}), 0)); // above the group's own row
        QVERIFY(!model.canDropAt(at({3}), 4)); // under Base: the top level
        QVERIFY(!model.dropAt(at({3}), 4));
        QVERIFY(!model.error().isEmpty());
        QVERIFY(model.dropAt(at({3}), 1));
        QCOMPARE(names(model.layers()), (QStringList{"Group 4", "Fill 1", "Fill 2", "Base"}));
    }

    void thePropertiesDialogIsOneRevision() {
        CanvasViewModel model;
        open(model);
        QVERIFY(model.setProperties(1, QVariantMap{{"name", "Red"}, {"opacity", 0.4}, {"blend", "multiply"}, {"maskEnabled", false}}));
        QCOMPARE(revisionCount(), 2);
        const QVariantMap row = model.layers().at(1).toMap();
        QCOMPARE(row.value("name").toString(), QStringLiteral("Red"));
        QCOMPARE(row.value("opacity").toDouble(), 0.4);
        QCOMPARE(row.value("blend").toString(), QStringLiteral("multiply"));
        QCOMPARE(row.value("maskEnabled").toBool(), false);
        QVERIFY(model.setProperties(1, QVariantMap{{"removeMask", true}}));
        QCOMPARE(model.layers().at(1).toMap().value("hasMask").toBool(), false);
        QVERIFY(model.undo());
        QCOMPARE(model.layers().at(1).toMap().value("hasMask").toBool(), true);
        QVERIFY(model.undo());
        QCOMPARE(names(model.layers()), (QStringList{"Fill 2", "Fill 1", "Base"})); // one undo took the whole dialog back
        QVERIFY(!model.setProperties(1, QVariantMap{}));
        QVERIFY(model.stepName(model.revisionB()).size() > 0);
    }
};

QTEST_MAIN(TestCanvasLayersModel)
#include "test_canvas_layers_model.moc"
