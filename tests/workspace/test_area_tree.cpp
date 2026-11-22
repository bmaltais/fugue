// The area tree (#217): split, join, resize by shares with minimum sizes, the layout in pixels, the saved
// form and what a damaged one gives; the view registry; the QML helper over both. No window: the rules are
// all here.
//
// Seams: workspace/area_tree.h, workspace/view_registry.h, workspace/area_layout_qml.h.

#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

#include <cmath>

#include "workspace/area_layout_qml.h"
#include "workspace/area_tree.h"
#include "workspace/view_registry.h"

using namespace fugue;

namespace {

// A Graph is at least 160 x 120; any other view in these tests 100 x 50.
QSizeF minimum(const QString &view) { return view == QLatin1String("graph") ? QSizeF(160, 120) : QSizeF(100, 50); }

QList<double> shares(const AreaTree::Node &branch) {
    QList<double> out;
    for (const auto &child : branch.children)
        out << child.size;
    return out;
}

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

QRectF rectOf(const QList<AreaTree::Placed> &placed, int area) {
    for (const auto &p : placed)
        if (p.area == area)
            return p.rect;
    return {};
}

QJsonObject leaf(int id, const QString &view = QStringLiteral("graph")) {
    return {{"type", "leaf"}, {"area", id}, {"view", view}, {"state", QJsonObject{}}};
}
QJsonObject branch(const QString &axis, const QList<QPair<double, QJsonObject>> &children) {
    QJsonArray list;
    for (const auto &child : children)
        list.append(QJsonObject{{"size", child.first}, {"node", child.second}});
    return {{"type", "branch"}, {"axis", axis}, {"children", list}};
}
QJsonObject tree(const QJsonObject &root) { return {{"format", 1}, {"root", root}}; }

} // namespace

class TestAreaTree : public QObject {
    Q_OBJECT

private slots:
    void theDefaultTreeIsOneGraphArea() {
        const AreaTree tree;
        QCOMPARE(tree.count(), 1);
        QCOMPARE(tree.areaIds(), QList<int>({1}));
        QVERIFY(tree.root().leaf);
        QCOMPARE(tree.area(1)->view, QString("graph"));
        QVERIFY(tree.area(1)->state.isEmpty());
        QVERIFY(!tree.area(2));
        QCOMPARE(tree.toJson(), QJsonObject({{"format", 1}, {"root", leaf(1)}}));
    }

    void aSplitGivesTheNewAreaItsShareAfterOrBefore() {
        AreaTree after;
        QCOMPARE(after.split(1, AreaAxis::Row, "queue", 0.25), 2);
        QVERIFY(!after.root().leaf);
        QCOMPARE(after.root().axis, AreaAxis::Row);
        QCOMPARE(after.areaIds(), QList<int>({1, 2}));
        QVERIFY(near(shares(after.root()).at(0), 0.75) && near(shares(after.root()).at(1), 0.25));
        QCOMPARE(after.area(2)->view, QString("queue"));

        AreaTree before;
        QCOMPARE(before.split(1, AreaAxis::Column, "queue", 0.25, false), 2);
        QCOMPARE(before.root().axis, AreaAxis::Column);
        QCOMPARE(before.areaIds(), QList<int>({2, 1}));
        QVERIFY(near(shares(before.root()).at(0), 0.25));
    }

    void aSplitAlongTheParentsAxisAddsASiblingAndAcrossItNests() {
        AreaTree tree;
        tree.split(1, AreaAxis::Row, "a");          // row [1, 2]
        QCOMPARE(tree.split(2, AreaAxis::Row, "b"), 3); // row [1, 2, 3], not row [1, row [2, 3]]
        QCOMPARE(int(tree.root().children.size()), 3);
        QVERIFY(near(shares(tree.root()).at(0), 0.5) && near(shares(tree.root()).at(1), 0.25)
                && near(shares(tree.root()).at(2), 0.25));
        QCOMPARE(tree.split(3, AreaAxis::Column, "c"), 4); // row [1, 2, column [3, 4]]
        const AreaTree::Node &column = tree.root().children[2];
        QVERIFY(!column.leaf);
        QCOMPARE(column.axis, AreaAxis::Column);
        QVERIFY(near(column.size, 0.25)); // the column took the place area 3 had
        QCOMPARE(tree.areaIds(), QList<int>({1, 2, 3, 4}));
        QCOMPARE(tree.pathOf(4), AreaTree::Path({2, 1}));
    }

    void aSplitIsRefusedWhenItCannotBe() {
        AreaTree tree;
        QCOMPARE(tree.split(9, AreaAxis::Row, "a"), 0);
        QCOMPARE(tree.split(1, AreaAxis::Row, ""), 0);
        QCOMPARE(tree.split(1, AreaAxis::Row, "a", 0), 0);
        QCOMPARE(tree.split(1, AreaAxis::Row, "a", 1), 0);
        QCOMPARE(tree.split(1, AreaAxis::Row, "a", std::nan("")), 0);
        QCOMPARE(tree.count(), 1);
        for (int i = 1; i < AreaTree::kMaxAreas; ++i)
            QVERIFY(tree.split(i, AreaAxis::Row, "a"));
        QCOMPARE(tree.count(), AreaTree::kMaxAreas);
        QCOMPARE(tree.split(1, AreaAxis::Row, "a"), 0);

        // Nested deeper than a saved tree may be: refused, so whatever is made can be read back.
        AreaTree deep;
        int last = 1;
        for (int depth = 1; depth <= AreaTree::kMaxDepth; ++depth) {
            last = deep.split(last, depth % 2 ? AreaAxis::Row : AreaAxis::Column, "a");
            QVERIFY(last > 0);
        }
        QCOMPARE(deep.split(last, AreaTree::kMaxDepth % 2 ? AreaAxis::Column : AreaAxis::Row, "a"), 0);
        QString problem;
        QCOMPARE(AreaTree::load(deep.save(), &problem).toJson(), deep.toJson());
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
    }

    void aJoinGivesTheGoneAreasPlaceToItsNeighbour() {
        AreaTree tree;
        tree.split(1, AreaAxis::Row, "a", 0.5);    // row [1, 2]
        tree.split(2, AreaAxis::Row, "b", 0.5);    // row [1 .5, 2 .25, 3 .25]
        QVERIFY(!tree.join(1, 3));                 // not side by side
        QVERIFY(!tree.join(1, 1));
        QVERIFY(!tree.join(1, 9));
        QVERIFY(tree.join(2, 3));
        QCOMPARE(tree.areaIds(), QList<int>({1, 2}));
        QVERIFY(near(shares(tree.root()).at(1), 0.5));

        // A branch left with one child is that child.
        tree.split(2, AreaAxis::Column, "c");      // row [1, column [2, 3]]: a gone area's id is free again
        QCOMPARE(tree.areaIds(), QList<int>({1, 2, 3}));
        QVERIFY(!tree.join(1, 3));                 // not in one branch
        QVERIFY(tree.join(3, 2));
        QCOMPARE(tree.areaIds(), QList<int>({1, 3}));
        QVERIFY(tree.root().children[1].leaf);
        QVERIFY(near(shares(tree.root()).at(1), 0.5));

        // The last area stays.
        QVERIFY(tree.join(1, 3));
        QVERIFY(tree.root().leaf);
        QCOMPARE(tree.areaIds(), QList<int>({1}));
        QVERIFY(near(tree.root().size, 1));
        QVERIFY(!tree.join(1, 1));
    }

    void aCloseGivesTheGoneAreasShareToTheChildBeforeIt() {
        AreaTree tree;
        tree.split(1, AreaAxis::Row, "a", 0.5);    // row [1 .5, 2 .5]
        tree.split(2, AreaAxis::Column, "b", 0.5); // row [1, column [2, 3]]
        QCOMPARE(tree.close(9), 0);
        // The first child of a branch gives its share to the one after it.
        QCOMPARE(tree.close(2), 3);
        QCOMPARE(tree.areaIds(), QList<int>({1, 3}));
        QVERIFY(tree.root().children[1].leaf); // the column of one is that one
        QVERIFY(near(shares(tree.root()).at(1), 0.5));
        // Any other to the one before it; the area that takes the place is the first area of that child.
        QCOMPARE(tree.split(1, AreaAxis::Column, "c", 0.5), 4); // row [column [1, 4], 3]
        QCOMPARE(tree.close(3), 1);
        QCOMPARE(tree.root().axis, AreaAxis::Column);
        QCOMPARE(tree.areaIds(), QList<int>({1, 4}));
        QVERIFY(near(tree.root().size, 1));
        QCOMPARE(tree.close(1), 4);
        QVERIFY(tree.root().leaf);
        QCOMPARE(tree.close(4), 0); // the last area stays
        QCOMPARE(tree.count(), 1);
    }

    void aSwapTradesViewsAndStatesButNotPlaces() {
        AreaTree tree;
        tree.split(1, AreaAxis::Row, "canvas", 0.25);
        tree.setState(1, {{"zoom", 2}});
        tree.setState(2, {{"canvasId", "c1"}});
        QVERIFY(!tree.swap(1, 1));
        QVERIFY(!tree.swap(1, 9));
        QVERIFY(tree.swap(1, 2));
        QCOMPARE(tree.area(1)->view, QString("canvas"));
        QCOMPARE(tree.area(2)->view, QString("graph"));
        QCOMPARE(tree.area(1)->state.value("canvas").toObject().value("canvasId").toString(), QString("c1"));
        QCOMPARE(tree.area(2)->state.value("graph").toObject().value("zoom").toInt(), 2);
        QCOMPARE(tree.areaIds(), QList<int>({1, 2}));
        QVERIFY(near(shares(tree.root()).at(0), 0.75)); // the places stay
    }

    void equalizeSharesABordersTwoChildrenEqually() {
        AreaTree tree;
        tree.split(1, AreaAxis::Row, "a", 0.5);
        tree.split(2, AreaAxis::Row, "b", 0.5); // row [1 .5, 2 .25, 3 .25]
        QVERIFY(tree.equalize({}, 0));
        QVERIFY(near(shares(tree.root()).at(0), 0.375) && near(shares(tree.root()).at(1), 0.375));
        QVERIFY(near(shares(tree.root()).at(2), 0.25));
        QVERIFY(!tree.equalize({}, 0)); // already
        QVERIFY(!tree.equalize({}, 2));
        QVERIFY(!tree.equalize({0}, 0));
    }

    void aResizeMovesABorderAndKeepsBothSidesAtTheirMinimum() {
        AreaTree tree;
        tree.split(1, AreaAxis::Row, "a", 0.5); // graph (min 160) | a (min 100), 1000 px
        QVERIFY(tree.resize({}, 0, 200, 1000, minimum));
        QVERIFY(near(shares(tree.root()).at(0), 0.7) && near(shares(tree.root()).at(1), 0.3));
        QVERIFY(tree.resize({}, 0, 5000, 1000, minimum));
        QVERIFY(near(shares(tree.root()).at(1), 0.1)); // a keeps its 100
        QVERIFY(!tree.resize({}, 0, 50, 1000, minimum)); // already at the end
        QVERIFY(tree.resize({}, 0, -5000, 1000, minimum));
        QVERIFY(near(shares(tree.root()).at(0), 0.16)); // the Graph keeps its 160
        QVERIFY(!tree.resize({}, 1, 10, 1000, minimum)); // no border after the last child
        QVERIFY(!tree.resize({0}, 0, 10, 1000, minimum)); // a leaf has no border
        QVERIFY(!tree.resize({}, 0, 10, 0, minimum));
        QVERIFY(!tree.resize({}, 0, 10, 200, minimum)); // 200 px cannot hold 160 + 100
    }

    void anAreaKeepsTheStateOfEveryViewItShowed() {
        AreaTree tree;
        QVERIFY(tree.setState(1, {{"zoom", 2}}));
        QVERIFY(tree.setView(1, "queue"));
        QVERIFY(!tree.setView(1, "queue"));
        QVERIFY(!tree.setView(1, ""));
        QVERIFY(!tree.setView(7, "queue"));
        QVERIFY(tree.setState(1, {{"scroll", 3}}));
        QVERIFY(tree.setView(1, "graph"));
        QCOMPARE(tree.area(1)->state.value("graph").toObject().value("zoom").toInt(), 2);
        QCOMPARE(tree.area(1)->state.value("queue").toObject().value("scroll").toInt(), 3);
        QVERIFY(!tree.setState(7, {}));
    }

    void theLayoutIsProportionalAndMirrorsARowOnly() {
        AreaTree tree;
        tree.split(1, AreaAxis::Row, "a", 0.25);
        QList<AreaTree::Placed> placed = tree.layout(QRectF(0, 0, 800, 600), minimum, false);
        QCOMPARE(rectOf(placed, 1), QRectF(0, 0, 600, 600));
        QCOMPARE(rectOf(placed, 2), QRectF(600, 0, 200, 600));
        // A wider window: every area grows in proportion.
        placed = tree.layout(QRectF(0, 0, 1600, 600), minimum, false);
        QCOMPARE(rectOf(placed, 1), QRectF(0, 0, 1200, 600));
        QCOMPARE(rectOf(placed, 2), QRectF(1200, 0, 400, 600));
        // Right to left, the start is the right edge.
        placed = tree.layout(QRectF(10, 20, 800, 600), minimum, true);
        QCOMPARE(rectOf(placed, 1), QRectF(210, 20, 600, 600));
        QCOMPARE(rectOf(placed, 2), QRectF(10, 20, 200, 600));
        // A column goes top to bottom in every language.
        tree.split(2, AreaAxis::Column, "b", 0.5);
        placed = tree.layout(QRectF(0, 0, 800, 600), minimum, true);
        QCOMPARE(rectOf(placed, 2), QRectF(0, 0, 200, 300));
        QCOMPARE(rectOf(placed, 3), QRectF(0, 300, 200, 300));
    }

    void theLayoutKeepsMinimumsAndWholePixels() {
        // 0.9 / 0.1 of 1000 would leave the second view 100 px: it keeps its minimum.
        QCOMPARE(AreaTree::distribute(1000, {0.9, 0.1}, {160, 150}), QList<double>({850, 150}));
        // Too small for both minimums: each in proportion to its minimum.
        QCOMPARE(AreaTree::distribute(200, {0.5, 0.5}, {150, 50}), QList<double>({150, 50}));
        QCOMPARE(AreaTree::distribute(100, {0.5, 0.5}, {150, 50}), QList<double>({75, 25}));
        // Thirds of 100: borders on whole pixels, the parts add up.
        const QList<double> thirds = AreaTree::distribute(100, {1 / 3.0, 1 / 3.0, 1 / 3.0}, {0, 0, 0});
        QCOMPARE(thirds, QList<double>({33, 34, 33}));
        // Holding one part at its minimum can push another under its own: that one is held too.
        QCOMPARE(AreaTree::distribute(500, {0.45, 0.45, 0.1}, {0, 210, 100}), QList<double>({190, 210, 100}));
        QCOMPARE(AreaTree::distribute(10, {}, {}), QList<double>());
    }

    void theMinimumSizeAddsAlongAnAxisAndTakesTheLargestAcrossIt() {
        AreaTree tree;
        tree.split(1, AreaAxis::Row, "a");
        tree.split(2, AreaAxis::Column, "b"); // row [graph, column [a, b]]
        QCOMPARE(tree.minimumSize(minimum), QSizeF(260, 120));
        QVERIFY(tree.fits(QSizeF(260, 120), minimum));
        QVERIFY(!tree.fits(QSizeF(259, 120), minimum));
        QVERIFY(!tree.fits(QSizeF(260, 119), minimum));
        // The way a split is checked against a size: on a copy.
        AreaTree next = tree;
        next.split(1, AreaAxis::Row, "c");
        QVERIFY(!next.fits(QSizeF(300, 200), minimum));
        QCOMPARE(tree.count(), 3);
    }

    void theSavedFormReadsBackTheSame() {
        AreaTree tree;
        tree.split(1, AreaAxis::Row, "queue", 0.375);
        tree.split(2, AreaAxis::Column, "results", 0.5, false);
        tree.setState(1, {{"zoom", 1.5}, {"center", QJsonArray{10, 20}}});
        QString problem = "unset";
        const AreaTree back = AreaTree::load(tree.save(), &problem);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(back.toJson(), tree.toJson());
        QCOMPARE(back.save(), tree.save());
        QCOMPARE(back.areaIds(), QList<int>({1, 3, 2}));
        // A split after loading takes an id no area has.
        AreaTree more = back;
        QCOMPARE(more.split(1, AreaAxis::Column, "x"), 4);
    }

    void unknownViewsAndKeysAreKept() {
        QJsonObject future = leaf(2, "storyboard");
        future.insert("maximized", true);
        QJsonObject root = branch("row", {{0.5, leaf(1)}, {0.5, future}});
        root.insert("locked", true);
        QJsonObject json = tree(root);
        json.insert("hint", "from a newer Fugue");
        QString problem;
        const AreaTree read = AreaTree::fromJson(json, &problem);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(read.area(2)->view, QString("storyboard"));
        QCOMPARE(read.toJson(), json);
    }

    void redundantShapesAreNormalized() {
        // A row inside a row is one row; shares are scaled to sum to 1.
        const AreaTree merged = AreaTree::fromJson(tree(branch("row", {{2, leaf(1)}, {6, branch("row", {{1, leaf(2)}, {1, leaf(3)}})}})));
        QCOMPARE(merged.areaIds(), QList<int>({1, 2, 3}));
        QCOMPARE(int(merged.root().children.size()), 3);
        QVERIFY(near(shares(merged.root()).at(0), 0.25) && near(shares(merged.root()).at(1), 0.375));
        // A branch of one child is that child.
        const AreaTree single = AreaTree::fromJson(tree(branch("column", {{1, leaf(5, "queue")}})));
        QVERIFY(single.root().leaf);
        QCOMPARE(single.areaIds(), QList<int>({5}));
    }

    void aDamagedTreeGivesTheDefault_data() {
        QTest::addColumn<QByteArray>("bytes");
        const auto bytes = [](const QJsonObject &json) { return QJsonDocument(json).toJson(); };
        QJsonObject noState = leaf(1);
        noState.insert("state", 3);
        QJsonObject deep = leaf(1);
        for (int i = 0; i < AreaTree::kMaxDepth + 1; ++i)
            deep = branch(i % 2 ? "row" : "column", {{1, deep}});
        QTest::newRow("not json") << QByteArray("{\"format\": 1, ");
        QTest::newRow("an array") << QByteArray("[1]");
        QTest::newRow("another format") << bytes({{"format", 2}, {"root", leaf(1)}});
        QTest::newRow("no format") << bytes({{"root", leaf(1)}});
        QTest::newRow("no root") << bytes({{"format", 1}});
        QTest::newRow("a node of no type") << bytes(tree({{"type", "blob"}}));
        QTest::newRow("area 0") << bytes(tree(leaf(0)));
        QTest::newRow("area 1.5") << bytes(tree({{"type", "leaf"}, {"area", 1.5}, {"view", "graph"}}));
        QTest::newRow("area as text") << bytes(tree({{"type", "leaf"}, {"area", "1"}, {"view", "graph"}}));
        QTest::newRow("no view") << bytes(tree({{"type", "leaf"}, {"area", 1}}));
        QTest::newRow("a state that is not an object") << bytes(tree(noState));
        QTest::newRow("an area twice") << bytes(tree(branch("row", {{1, leaf(1)}, {1, leaf(1)}})));
        QTest::newRow("size 0") << bytes(tree(branch("row", {{0, leaf(1)}, {1, leaf(2)}})));
        QTest::newRow("size below 0") << bytes(tree(branch("row", {{-1, leaf(1)}, {1, leaf(2)}})));
        QTest::newRow("an axis of left") << bytes(tree(branch("left", {{1, leaf(1)}, {1, leaf(2)}})));
        QTest::newRow("no children") << bytes(tree({{"type", "branch"}, {"axis", "row"}, {"children", QJsonArray{}}}));
        QTest::newRow("a child with no node") << bytes(tree({{"type", "branch"}, {"axis", "row"},
                                                            {"children", QJsonArray{QJsonObject{{"size", 1}}}}}));
        QTest::newRow("too deep") << bytes(tree(deep));
        QJsonArray many;
        for (int i = 1; i <= AreaTree::kMaxAreas + 1; ++i)
            many.append(QJsonObject{{"size", 1}, {"node", leaf(i)}});
        QTest::newRow("too many areas") << bytes(tree({{"type", "branch"}, {"axis", "row"}, {"children", many}}));
    }
    void aDamagedTreeGivesTheDefault() {
        QFETCH(QByteArray, bytes);
        QString problem;
        const AreaTree read = AreaTree::load(bytes, &problem);
        QVERIFY(!problem.isEmpty());
        QCOMPARE(read.toJson(), AreaTree().toJson());
    }

    void theRegistryKnowsTheGraphAndTheCanvas() {
        ViewRegistry views;
        QCOMPARE(views.ids(), QStringList({"graph", "canvas"}));
        QVERIFY(views.contains(ViewRegistry::kGraph));
        QCOMPARE(views.name("graph"), QString("Graph"));
        QCOMPARE(views.minimumSize("graph"), QSizeF(160, 120));
        QCOMPARE(views.multiplicity("graph"), QString("one-per-window"));
        QCOMPARE(views.component("graph"), QUrl("qrc:/canvas/FugueCanvas.qml"));
        QCOMPARE(views.glyph("graph"), QString("workflow"));
        QVERIFY(views.contains(ViewRegistry::kCanvas));
        QCOMPARE(views.name("canvas"), QString("Canvas"));
        QCOMPARE(views.minimumSize("canvas"), QSizeF(240, 180));
        QCOMPARE(views.multiplicity("canvas"), QString("many"));
        QCOMPARE(views.component("canvas"), QUrl("qrc:/workspace/CanvasArea.qml"));
        QCOMPARE(views.glyph("canvas"), QString("image"));
    }

    void anUnknownViewIsAPlaceholder() {
        ViewRegistry views;
        QVERIFY(!views.contains("storyboard"));
        QCOMPARE(views.name("storyboard"), QString("storyboard"));
        QCOMPARE(views.minimumSize("storyboard"), ViewRegistry::kPlaceholderMinimum);
        QVERIFY(views.component("storyboard").isEmpty());
        QCOMPARE(views.multiplicity("storyboard"), QString("many"));
    }

    void aRowAddsAViewOrReplacesTheOneOfItsId() {
        ViewRegistry views;
        views.add({"queue", "Queue", "layers", QUrl("qrc:/x.qml"), QSizeF(200, 100), ViewRegistry::Multiplicity::Many});
        views.add({"graph", "Graph", "workflow", QUrl("qrc:/y.qml"), QSizeF(300, 200), ViewRegistry::Multiplicity::OnePerTarget});
        QCOMPARE(views.ids(), QStringList({"graph", "canvas", "queue"}));
        QCOMPARE(views.minimumSize("graph"), QSizeF(300, 200));
        QCOMPARE(views.multiplicity("graph"), QString("one-per-target"));
    }

    void qmlGetsTheDefaultTreeAndABranchsRects() {
        AreaLayout layout;
        ViewRegistry views;
        const QVariantMap defaults = layout.defaultTree();
        QCOMPARE(QJsonObject::fromVariantMap(defaults), AreaTree().toJson());
        const QVariantMap row = branch("row", {{0.75, leaf(1)}, {0.25, leaf(2, "storyboard")}}).toVariantMap();
        const QVariantList rects = layout.childRects(row, QRectF(0, 0, 400, 300), nullptr, false);
        QCOMPARE(rects.size(), 2);
        QCOMPARE(rects.at(0).toRectF(), QRectF(0, 0, 300, 300));
        QCOMPARE(rects.at(1).toRectF(), QRectF(300, 0, 100, 300));
        // With the registry's minimums the placeholder keeps its 160 and the Graph gets the rest.
        const QVariantList held = layout.childRects(row, QRectF(0, 0, 400, 300), &views, true);
        QCOMPARE(held.at(0).toRectF(), QRectF(160, 0, 240, 300));
        QCOMPARE(held.at(1).toRectF(), QRectF(0, 0, 160, 300));
        // The children as given, in their order: a row in a row is not merged here.
        const QVariantMap nested = branch("row", {{0.5, leaf(1)}, {0.5, branch("row", {{1, leaf(2)}, {1, leaf(3)}})}}).toVariantMap();
        QCOMPARE(layout.childRects(nested, QRectF(0, 0, 400, 300), nullptr, false).size(), 2);
        QCOMPARE(layout.shapeOf(nested), QString("row(1:graph,row(2:graph,3:graph))"));
        QCOMPARE(layout.shapeOf(leaf(4, "queue").toVariantMap()), QString("4:queue"));
        QVERIFY(layout.shapeOf(QVariantMap{{"type", "blob"}}).isEmpty());
        QVERIFY(layout.childRects(QVariantMap{{"type", "blob"}}, QRectF(0, 0, 10, 10), nullptr, false).isEmpty());
        QVERIFY(layout.childRects(leaf(1).toVariantMap(), QRectF(0, 0, 10, 10), nullptr, false).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestAreaTree)
#include "test_area_tree.moc"
