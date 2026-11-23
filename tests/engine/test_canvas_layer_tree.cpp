// The layers of a canvas as an outline (#278), no window and no store: the rows a list shows, the ids a layer inside a group takes,
// the open state kept by name, what a hidden or locked group means for the layers in it, the selection a closing group takes
// over, the drop line resolved within a group, and an edit made inside a group (LayerEdit::parent).

#include <QtTest>

#include "engine/canvas_layer_ops.h"
#include "engine/canvas_layer_tree.h"

using namespace fugue;
using namespace fugue::canvas;

namespace {

LayerRecord layer(const QString &name) {
    LayerRecord l;
    l.name = name;
    return l;
}

LayerRecord group(const QString &name, std::vector<LayerRecord> children) {
    LayerRecord g = layer(name);
    g.group = true;
    g.children = std::move(children);
    return g;
}

// Bottom first: Base, Props (group: Lamp, Table, Inner (group: Cup, Plate)), Sky.
Tree sample() {
    Tree t;
    t.layers = {layer("Base"), group("Props", {layer("Lamp"), layer("Table"), group("Inner", {layer("Cup"), layer("Plate")})}), layer("Sky")};
    return t;
}

QStringList names(const std::vector<OutlineRow> &rows) {
    QStringList out;
    for (const OutlineRow &r : rows)
        out << r.name;
    return out;
}

QStringList names(const std::vector<LayerRecord> &layers) {
    QStringList out;
    for (const LayerRecord &l : layers)
        out << l.name;
    return out;
}

} // namespace

class TestCanvasLayerTree : public QObject {
    Q_OBJECT

private slots:
    void groupsStartClosedAndOpenOneLevelAtATime() {
        LayerOutline o;
        o.setTree(sample());
        QCOMPARE(names(o.rows()), (QStringList{"Sky", "Props", "Base"}));
        QCOMPARE(o.rows()[1].children, 3);
        QVERIFY(o.setOpen({1}, true));
        QCOMPARE(names(o.rows()), (QStringList{"Sky", "Props", "Inner", "Table", "Lamp", "Base"}));
        QCOMPARE(o.rows()[2].depth, 2 - 1);
        QVERIFY(o.toggle({1, 2}));
        QCOMPARE(names(o.rows()), (QStringList{"Sky", "Props", "Inner", "Plate", "Cup", "Table", "Lamp", "Base"}));
        QVERIFY(!o.setOpen({0}, true)); // not a group
    }

    void anIdIsTheSameWhetherTheGroupIsOpenOrNot() {
        LayerOutline o;
        o.setTree(sample());
        QCOMPARE(o.topCount(), 3);
        QCOMPARE(o.idOf({2}), 2); // a top-level layer: its position
        const int cup = o.idOf({1, 2, 0});
        QVERIFY(cup >= 3);
        o.setAllOpen(true);
        QCOMPARE(o.idOf({1, 2, 0}), cup);
        QCOMPARE(o.pathOf(cup), (LayerPath{1, 2, 0}));
        QCOMPARE(o.row(cup)->name, QStringLiteral("Cup"));
        QCOMPARE(o.levelSize({1}), 3);
        QCOMPARE(o.levelSize({}), 3);
    }

    void theOpenStateFollowsTheNamesNotThePositions() {
        LayerOutline o;
        o.setTree(sample());
        o.setOpen({1}, true);
        Tree t = sample();
        t.layers.erase(t.layers.begin()); // Base deleted: Props moved down to position 0
        o.setTree(t);
        QVERIFY(o.isOpen({0}));
        QCOMPARE(names(o.rows()), (QStringList{"Sky", "Props", "Inner", "Table", "Lamp"}));
        // A rename carries it, and the groups inside.
        o.setOpen({0, 2}, true);
        t.layers[0].name = "Stuff";
        o.setTree(t);
        QVERIFY(!o.isOpen({0}));
        o.renameOpen({0}, "Props");
        QVERIFY(o.isOpen({0}));
        QVERIFY(o.isOpen({0, 2}));
    }

    void aHiddenGroupHidesAndALockedOneProtectsWhatIsInsideIt() {
        Tree t = sample();
        t.layers[1].visible = false;
        t.layers[1].children[2].locked = true;
        LayerOutline o;
        o.setTree(t);
        o.setAllOpen(true);
        QVERIFY(!o.row(o.idOf({1}))->shown);
        QVERIFY(!o.row(o.idOf({1, 0}))->shown);
        QVERIFY(!o.row(o.idOf({1, 2, 1}))->shown); // two levels down
        QVERIFY(o.row(o.idOf({0}))->shown);
        QVERIFY(o.row(o.idOf({1, 2, 1}))->lockedAbove);
        QVERIFY(!o.row(o.idOf({1, 2}))->lockedAbove); // the group itself is locked, not above itself
        QVERIFY(!o.row(o.idOf({1, 0}))->lockedAbove);
    }

    void aClosingGroupTakesTheSelectionOfWhatItHides() {
        LayerOutline o;
        o.setTree(sample());
        o.setAllOpen(true);
        const int cup = o.idOf({1, 2, 0}), table = o.idOf({1, 1});
        o.toggle({1, 2});
        QCOMPARE(o.fitSelection({cup, table, o.idOf({2})}), (std::vector<int>{o.idOf({1, 2}), table, 2}));
        o.toggle({1});
        QCOMPARE(o.fitSelection({cup, table}), (std::vector<int>{o.idOf({1})}));
    }

    void editsNeedLayersOfOneGroup() {
        LayerOutline o;
        o.setTree(sample());
        LayerPath parent;
        std::vector<int> at;
        QVERIFY(o.levelOf({o.idOf({1, 0}), o.idOf({1, 2})}, &parent, &at));
        QCOMPARE(parent, (LayerPath{1}));
        QCOMPARE(at, (std::vector<int>{0, 2}));
        QVERIFY(o.levelOf({0, 2}, &parent, &at));
        QVERIFY(parent.empty());
        QVERIFY(!o.levelOf({0, o.idOf({1, 0})}, &parent, &at));
        QVERIFY(!o.levelOf({}, &parent, &at));
    }

    void aDropLineResolvesWithinTheGroupOfTheDraggedLayers() {
        LayerOutline o;
        o.setTree(sample());
        o.setOpen({1}, true); // rows: Sky 0, Props 1, Inner 2, Table 3, Lamp 4, Base 5
        const int lamp = o.idOf({1, 0});
        // Lamp (bottom of Props) dropped above Table: it ends one up among the others, above Table only.
        DropPlace p = o.dropAt({lamp}, 3);
        QVERIFY(p.ok);
        QCOMPARE(p.parent, (LayerPath{1}));
        QCOMPARE(p.to, 1);
        QCOMPARE(o.dropAt({lamp}, 5).to, 0); // under the group's last child
        QCOMPARE(o.dropAt({lamp}, 2).to, 2); // above Inner: on top of the group
        // A line outside the group, or the group's own row, is not a place for its children.
        QVERIFY(!o.dropAt({lamp}, 1).ok);
        QVERIFY(!o.dropAt({lamp}, 0).ok);
        QVERIFY(!o.dropAt({lamp}, 6).ok);
        QVERIFY(!o.dropAt({lamp}, 1).why.isEmpty());
        // Top level, with the open group between: the line under Props' last child is below Props too.
        QCOMPARE(o.dropAt({2}, 5).to, 1); // Sky under Props' children: above Base only
        QCOMPARE(o.dropAt({2}, 6).to, 0);
        QVERIFY(!o.dropAt({2}, 3).ok); // inside Props
        QVERIFY(!o.dropAt({2, o.idOf({1, 0})}, 0).ok); // mixed levels
    }

    void aFlatListDropsAsItAlwaysDid() {
        Tree t;
        t.layers = {layer("A"), layer("B"), layer("C")};
        LayerOutline o;
        o.setTree(t);
        QCOMPARE(o.dropAt({0}, 0).to, 2);  // A to the top
        QCOMPARE(o.dropAt({2}, 3).to, 0);  // C to the bottom
        QCOMPARE(o.dropAt({0, 1}, 3).to, 0);
    }

    void anEditInsideAGroupChangesOnlyThatGroup() {
        const Tree t = sample();
        LayerEdit e;
        e.op = LayerOp::Rename;
        e.parent = {1, 2};
        e.layers = {0};
        e.text = "Mug";
        const Tree renamed = applyLayerEdit(t, e).value();
        QCOMPARE(names(renamed.layers[1].children[2].children), (QStringList{"Mug", "Plate"}));
        QCOMPARE(names(t.layers[1].children[2].children), (QStringList{"Cup", "Plate"}));

        e = LayerEdit();
        e.op = LayerOp::Move;
        e.parent = {1};
        e.layers = {0};
        e.to = 1;
        QCOMPARE(names(applyLayerEdit(t, e).value().layers[1].children), (QStringList{"Table", "Lamp", "Inner"}));

        e = LayerEdit();
        e.op = LayerOp::Remove;
        e.parent = {1};
        e.layers = {0, 1};
        QCOMPARE(names(applyLayerEdit(t, e).value().layers[1].children), (QStringList{"Inner"}));

        e = LayerEdit();
        e.op = LayerOp::Group;
        e.parent = {1};
        e.layers = {0, 1};
        e.text = "Pair";
        const Tree grouped = applyLayerEdit(t, e).value();
        QCOMPARE(names(grouped.layers[1].children), (QStringList{"Pair", "Inner"}));
        QCOMPARE(names(grouped.layers[1].children[0].children), (QStringList{"Lamp", "Table"}));
        e.op = LayerOp::Ungroup;
        e.layers = {2};
        e.parent = {1};
        QCOMPARE(names(applyLayerEdit(t, e).value().layers[1].children), (QStringList{"Lamp", "Table", "Cup", "Plate"}));
    }

    void aBadPlaceInsideAGroupIsRefusedInWords() {
        const Tree t = sample();
        LayerEdit e;
        e.op = LayerOp::Visible;
        e.parent = {0}; // Base is no group
        e.layers = {0};
        QVERIFY(!applyLayerEdit(t, e).ok());
        e.parent = {1};
        e.layers = {3}; // the group holds 3 layers
        const auto bad = applyLayerEdit(t, e);
        QVERIFY(!bad.ok());
        QVERIFY(bad.errors().first().message.contains("3 layers"));
        e.parent = {1, 9};
        e.layers = {0};
        QVERIFY(!applyLayerEdit(t, e).ok());
    }

    void aLockedGroupKeepsItsStackButNotItsLook() {
        Tree t = sample();
        t.layers[1].locked = true;
        LayerEdit e;
        e.parent = {1};
        e.layers = {0};
        e.op = LayerOp::Remove;
        const auto refused = applyLayerEdit(t, e);
        QVERIFY(!refused.ok());
        QVERIFY(refused.errors().first().message.contains("Props is locked"));
        e.op = LayerOp::New;
        QVERIFY(!applyLayerEdit(t, e).ok());
        e.op = LayerOp::Visible;
        e.flag = false;
        QVERIFY(!applyLayerEdit(t, e).value().layers[1].children[0].visible);
        e.op = LayerOp::Opacity;
        e.number = 0.5;
        QCOMPARE(applyLayerEdit(t, e).value().layers[1].children[0].opacity, 0.5);
    }

    void groupsStillNestAtMostEightDeepInsideAGroup() {
        LayerRecord inner = layer("leaf");
        Tree t;
        for (int d = 0; d < maxGroupDepth; ++d)
            inner = group(QString("g%1").arg(d), {inner});
        // The leaf sits in the deepest of maxGroupDepth groups: grouping it would make one more.
        t.layers = {inner};
        LayerPath chain;
        const LayerRecord *at = &t.layers[0];
        chain.push_back(0);
        while (!at->children.empty() && at->children[0].group) {
            at = &at->children[0];
            chain.push_back(0);
        }
        LayerEdit e;
        e.op = LayerOp::Group;
        e.parent = chain; // the deepest group: its leaf would sit one group deeper than the limit
        e.layers = {0};
        QVERIFY(!applyLayerEdit(t, e).ok());
    }
};

QTEST_GUILESS_MAIN(TestCanvasLayerTree)
#include "test_canvas_layer_tree.moc"
