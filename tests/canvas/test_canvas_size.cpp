// A node's size (#49): auto-size from its content, a manual resize that pins it in the layout,
// the grip's hit test, undo, and what follows (wires, hit tests). docs/canvas-look.md 4.1.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUndoStack>
#include <QtTest>

#include "canvas/canvas_model.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "canvas/node_size.h"
#include "nodes/builtin.h"
#include "../cpu_time.h"

using namespace fugue;

namespace {

Document document(const QByteArray &json) {
    const Result<Document> loaded = Document::load(json);
    if (!loaded.ok())
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

QVariantMap node(CanvasModel &canvas, const QString &id) {
    const QHash<int, QByteArray> roles = canvas.nodes()->roleNames();
    for (int i = 0; i < canvas.nodes()->rowCount(); ++i) {
        const QModelIndex index = canvas.nodes()->index(i, 0);
        if (canvas.nodes()->data(index, roles.key("nodeId")).toString() != id)
            continue;
        QVariantMap values;
        for (auto it = roles.begin(); it != roles.end(); ++it)
            values.insert(QString::fromUtf8(it.value()), canvas.nodes()->data(index, it.key()));
        return values;
    }
    return {};
}

QVariantMap port(const QVariantMap &node, const QString &side, const QString &name) {
    for (const QVariant &p : node.value(side).toList())
        if (p.toMap().value("name") == name)
            return p.toMap();
    return {};
}

QVariantMap link(CanvasModel &canvas, const QString &to) {
    const QHash<int, QByteArray> roles = canvas.links()->roleNames();
    for (int i = 0; i < canvas.links()->rowCount(); ++i) {
        QVariantMap values;
        for (auto it = roles.begin(); it != roles.end(); ++it)
            values.insert(QString::fromUtf8(it.value()), canvas.links()->data(canvas.links()->index(i, 0), it.key()));
        if (values.value("toNode").toString() + "." + values.value("toPort").toString() == to)
            return values;
    }
    return {};
}

QByteArray workflow(const QString &layoutNodes, const QString &textValue = "a red fox", const QString &name = "fugue") {
    return QStringLiteral(R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": %1}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}, "name": %2}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": {"from": "prompt.text"}}},
    "ghost": {"type": "acme.tools/ghost", "version": "1.0.0", "inputs": {"in": {"from": "prompt.text"}}}
  }},
  "layout": {"nodes": %3}
})")
        .arg(QString::fromUtf8(QJsonDocument(QJsonArray{textValue}).toJson(QJsonDocument::Compact)).mid(1).chopped(1))
        .arg(QString::fromUtf8(QJsonDocument(QJsonArray{name}).toJson(QJsonDocument::Compact)).mid(1).chopped(1))
        .arg(layoutNodes)
        .toUtf8();
}

const QString placed = R"({"prompt": {"pos": [0, 100]}, "gen": {"pos": [300, 80]}, "save": {"pos": [640, 120]},
                           "ghost": {"pos": [0, 400]}})";

} // namespace

class TestCanvasSize : public QObject {
    Q_OBJECT

    Registry m_registry;

private slots:
    void initTestCase() { nodes::addBuiltinNodeTypes(m_registry); }

    void aNodeIsAutoSizedAtSixtyUnitsWide() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 240.0);
        QVERIFY(!canvas.document().layout.value("nodes").toObject().value("save").toObject().contains("size"));
    }

    void aLongTextValueWidensTheNodeUpToTheMaximum() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed, "a red fox", "a quite long file name for the picture")));
        const qreal wide = node(canvas, "save").value("nodeWidth").toDouble();
        QVERIFY2(wide > 240.0 && wide <= 480.0, qPrintable(QString::number(wide)));
        canvas.setDocument(document(workflow(placed, "a red fox", QString(200, 'x'))));
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 480.0);
    }

    void aMultilineValueGrowsItsRowWithItsLinesUpToTheMaximum() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        const qreal plain = node(canvas, "prompt").value("nodeHeight").toDouble();
        canvas.setDocument(document(workflow(placed, QString("line\n").repeated(8))));
        const qreal eight = node(canvas, "prompt").value("nodeHeight").toDouble();
        QVERIFY2(eight > plain, "eight lines are taller than the three-line row");
        canvas.setDocument(document(workflow(placed, QString("line\n").repeated(80))));
        QCOMPARE(node(canvas, "prompt").value("nodeHeight").toDouble(), plain - 64 + 160);
    }

    // #74: a multiline field keeps its label as a caption line above its text, so a filled one
    // still says what it is: its row is one line taller than its text.
    void aMultilineRowHasACaptionLineAboveItsText() {
        QCOMPARE(node_size::multilineHeight(3, 12), 4 * 16 + 2 * 6.0);
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed, "one\ntwo\nthree\nfour")));
        const QVariantMap text = port(node(canvas, "prompt"), "inputs", "text");
        QCOMPARE(text.value("rowHeight").toDouble(), node_size::multilineHeight(4, 12));
    }

    // A long prompt is measured only as far as the row's 40u cap, so typing into it does
    // not lay the whole text out on every key (CPU time, tests/cpu_time.h).
    void aLongPromptIsMeasuredOnlyToTheCap() {
        const QString text = QString("a red fox in the snow ").repeated(20000);
        QCOMPARE(node_size::wrappedLines(text, 200, 12, 10), 11); // one past the cap says "more"
        QCOMPARE(node_size::wrappedLines("one\ntwo", 200, 12, 10), 2);
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed, text)));
        const qint64 start = test_support::cpuMs();
        canvas.setTextSize(13);
        canvas.setTextSize(12);
        const qint64 spent = test_support::cpuMs() - start;
        qInfo("two layouts of a %lld-character prompt: %lld ms CPU", qint64(text.size()), spent);
        QVERIFY2(spent < 40, qPrintable(QString::number(spent)));
    }

    void editingTheContentResizesTheNodeAndItsWiresFollow() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        const qreal before = link(canvas, "gen.prompt").value("y1").toDouble();
        Document doc = canvas.document();
        doc.graph.nodes["save"].inputs.insert("name", InputValue::literal(QString(200, 'x')));
        doc.graph.nodes["gen"].inputs.insert("prompt", InputValue::literal(QString("a\n").repeated(50)));
        doc.graph.nodes["prompt"].inputs.insert("text", InputValue::literal(QString("b\n").repeated(50)));
        canvas.commit(doc, "edit");
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 480.0);
        QCOMPARE(link(canvas, "save.image").value("x2").toDouble(), 640.0);
        QCOMPARE(link(canvas, "save.image").value("y2").toDouble(),
                 640.0 > 0 ? 120.0 + port(node(canvas, "save"), "inputs", "image").value("y").toDouble() : 0.0);
        Q_UNUSED(before)
        canvas.undo();
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 240.0);
    }

    void aResizePinsTheSizeInTheLayoutAsOneUndoStep() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        const qreal height = node(canvas, "save").value("nodeHeight").toDouble();
        const int steps = canvas.undoStack()->count();
        canvas.beginResize("save");
        for (int i = 1; i <= 5; ++i)
            canvas.resizeBy(20.0 * i, 10.0 * i);
        canvas.endResize();
        QCOMPARE(canvas.undoStack()->count(), steps + 1);
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 340.0);
        QCOMPARE(node(canvas, "save").value("nodeHeight").toDouble(), height + 50);
        QCOMPARE(canvas.document().layout.value("nodes").toObject().value("save").toObject().value("size").toArray(),
                 (QJsonArray{340, height + 50}));
        canvas.undo();
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 240.0);
        QCOMPARE(node(canvas, "save").value("nodeHeight").toDouble(), height);
        QVERIFY(!canvas.document().layout.value("nodes").toObject().value("save").toObject().contains("size"));
        canvas.redo();
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 340.0);
    }

    void aResizeIsHeldBetweenTheMinimumAndMaximumWidthAndTheContentsHeight() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        const qreal height = node(canvas, "save").value("nodeHeight").toDouble();
        canvas.beginResize("save");
        canvas.resizeBy(-1000, -1000);
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 160.0);
        QCOMPARE(node(canvas, "save").value("nodeHeight").toDouble(), height);
        canvas.resizeBy(1000, 0);
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 480.0);
        canvas.endResize();
    }

    void aClickOnTheGripChangesNothing() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        const int steps = canvas.undoStack()->count();
        canvas.beginResize("save");
        canvas.resizeBy(0, 0);
        canvas.endResize();
        QCOMPARE(canvas.undoStack()->count(), steps);
        QVERIFY(!canvas.document().layout.value("nodes").toObject().value("save").toObject().contains("size"));
    }

    void aPinnedHeightGivesItsExtraToTheLastMultilineInput() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(R"({"prompt": {"pos": [0, 0], "size": [300, 300]}})")));
        QCOMPARE(node(canvas, "prompt").value("nodeWidth").toDouble(), 300.0);
        QCOMPARE(node(canvas, "prompt").value("nodeHeight").toDouble(), 300.0);
        // The output stays where it was; the node is just taller.
        QCOMPARE(port(node(canvas, "prompt"), "outputs", "text").value("y").toDouble(), 28.0 + 4 + 12);
    }

    // #72: the rows under a growing multiline input move down, and so do the section headers
    // among them: the Advanced header stays between Height and Seed and its click still opens it.
    void aPinnedHeightMovesTheSectionHeadersWithTheirRows() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(R"({"gen": {"pos": [300, 80], "size": [240, 700]}})")));
        const QVariantMap gen = node(canvas, "gen");
        const QVariantMap height = port(gen, "inputs", "height");
        const QVariantMap steps = port(gen, "inputs", "steps");
        const QVariantMap seed = port(gen, "inputs", "seed");
        QVERIFY(steps.value("advancedHeader").toBool());
        const qreal header = steps.value("headerTop").toDouble();
        QCOMPARE(header, height.value("rowTop").toDouble() + height.value("rowHeight").toDouble());
        QVERIFY(header < seed.value("rowTop").toDouble());
        QCOMPARE(canvas.advancedHeaderAt(310, 80 + header + 2), QStringLiteral("gen"));
        QCOMPARE(canvas.advancedHeaderAt(310, 80 + height.value("rowTop").toDouble() + 2), QString());
    }

    void aSavedSizeIsKeptOnReloadAndClampedWhenOutOfRange() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(R"({"save": {"pos": [0, 0], "size": [333, 0]},
                                                 "gen": {"pos": [0, 0], "size": [9000, 0]},
                                                 "prompt": {"pos": [0, 0], "size": [10, 0]}})")));
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 333.0);
        QCOMPARE(node(canvas, "gen").value("nodeWidth").toDouble(), 480.0);
        QCOMPARE(node(canvas, "prompt").value("nodeWidth").toDouble(), 160.0);
        // The file's size is written back as it was read.
        QCOMPARE(canvas.document().layout.value("nodes").toObject().value("save").toObject().value("size").toArray(),
                 (QJsonArray{333, 0}));
    }

    void aDoubleClickOnTheGripReturnsToAuto() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(R"({"save": {"pos": [640, 120], "size": [400, 300]}})")));
        QVERIFY(canvas.autoSize("save"));
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 240.0);
        QVERIFY(!canvas.document().layout.value("nodes").toObject().value("save").toObject().contains("size"));
        QVERIFY(!canvas.autoSize("save")); // already automatic
        canvas.undo();
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 400.0);
        QCOMPARE(node(canvas, "save").value("nodeHeight").toDouble(), 300.0);
    }

    void aResizeMovesTheNodesWiresAndHitTests() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        QCOMPARE(link(canvas, "save.image").value("x1").toDouble(), 540.0); // gen, 300 + 240
        canvas.beginResize("gen");
        canvas.resizeBy(100, 0);
        canvas.endResize();
        QCOMPARE(link(canvas, "save.image").value("x1").toDouble(), 640.0);
        QCOMPARE(canvas.nodeAt(635, 100), QString("gen"));
        canvas.undo();
        QCOMPARE(link(canvas, "save.image").value("x1").toDouble(), 540.0);
        QCOMPARE(canvas.nodeAt(635, 100), QString());
    }

    // Any corner of an open node resizes it (#63, as ComfyUI): a square of `size` inside each
    // corner. Edges do not, and neither does a collapsed node.
    void theResizeCornersAreTheFourCornersOfAnOpenNode() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        const QVariantMap save = node(canvas, "save");
        const qreal left = 640, top = 120;
        const qreal right = left + save.value("nodeWidth").toDouble(), bottom = top + save.value("nodeHeight").toDouble();
        const auto corner = [&](qreal x, qreal y) {
            const QVariantMap at = canvas.resizeCornerAt(x, y, 12);
            return at.isEmpty() ? QString() : at.value("node").toString() + " " + at.value("corner").toString();
        };
        QCOMPARE(corner(right - 3, bottom - 3), QString("save se"));
        QCOMPARE(corner(left + 3, bottom - 3), QString("save sw"));
        QCOMPARE(corner(right - 3, top + 3), QString("save ne"));
        QCOMPARE(corner(left + 3, top + 3), QString("save nw"));
        QCOMPARE(corner(right - 3, bottom - 30), QString()); // the right edge
        QCOMPARE(corner((left + right) / 2, bottom - 3), QString()); // the bottom edge
        QCOMPARE(corner(right + 3, bottom - 3), QString()); // outside
        canvas.select({"save"});
        canvas.toggleCollapsed();
        QCOMPARE(corner(right - 3, bottom - 3), QString());
        QCOMPARE(corner(left + 3, top + 3), QString());
    }

    // A top or left corner moves the node too, so the opposite corner stays where it was; the
    // limits are the bottom right corner's, and the whole drag is one undo step that takes the
    // position back as well.
    void aResizeFromAnyCornerKeepsTheOppositeCornerInPlace_data() {
        QTest::addColumn<QString>("corner");
        QTest::addColumn<QPointF>("drag");
        QTest::addColumn<QPointF>("pos");
        QTest::addColumn<QSizeF>("grown");
        QTest::newRow("nw") << "nw" << QPointF(-40, -30) << QPointF(600, 90) << QSizeF(40, 30);
        QTest::newRow("ne") << "ne" << QPointF(30, -20) << QPointF(640, 100) << QSizeF(30, 20);
        QTest::newRow("sw") << "sw" << QPointF(-30, 20) << QPointF(610, 120) << QSizeF(30, 20);
        QTest::newRow("se") << "se" << QPointF(30, 20) << QPointF(640, 120) << QSizeF(30, 20);
    }
    void aResizeFromAnyCornerKeepsTheOppositeCornerInPlace() {
        QFETCH(QString, corner);
        QFETCH(QPointF, drag);
        QFETCH(QPointF, pos);
        QFETCH(QSizeF, grown);
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        const qreal height = node(canvas, "save").value("nodeHeight").toDouble();
        const int steps = canvas.undoStack()->count();
        canvas.beginResize("save", corner);
        canvas.resizeBy(drag.x() / 2, drag.y() / 2);
        canvas.resizeBy(drag.x(), drag.y());
        canvas.endResize();
        QCOMPARE(canvas.undoStack()->count(), steps + 1);
        QCOMPARE(canvas.position("save"), pos);
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 240 + grown.width());
        QCOMPARE(node(canvas, "save").value("nodeHeight").toDouble(), height + grown.height());
        const QJsonObject saved = canvas.document().layout.value("nodes").toObject().value("save").toObject();
        QCOMPARE(saved.value("pos").toArray(), (QJsonArray{pos.x(), pos.y()}));
        QCOMPARE(saved.value("size").toArray(), (QJsonArray{240 + grown.width(), height + grown.height()}));
        // The wire into its input follows the left edge and the port.
        QCOMPARE(link(canvas, "save.image").value("x2").toDouble(), pos.x());
        canvas.undo();
        QCOMPARE(canvas.position("save"), QPointF(640, 120));
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 240.0);
        QCOMPARE(link(canvas, "save.image").value("x2").toDouble(), 640.0);
        canvas.redo();
        QCOMPARE(canvas.position("save"), pos);
    }

    void aTopLeftResizeHeldByItsLimitsStillKeepsTheBottomRightCorner() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        const qreal height = node(canvas, "save").value("nodeHeight").toDouble();
        canvas.beginResize("save", "nw");
        canvas.resizeBy(1000, 1000); // smaller than the minimum width and the content
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 160.0);
        QCOMPARE(node(canvas, "save").value("nodeHeight").toDouble(), height);
        QCOMPARE(canvas.position("save"), QPointF(640 + 240 - 160, 120));
        canvas.resizeBy(-1000, 0); // wider than the maximum
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 480.0);
        QCOMPARE(canvas.position("save"), QPointF(640 + 240 - 480, 120));
        canvas.resizeBy(0, 0); // back where it began: nothing changed
        canvas.endResize();
        QCOMPARE(canvas.position("save"), QPointF(640, 120));
        QVERIFY(!canvas.document().layout.value("nodes").toObject().value("save").toObject().contains("size"));
    }

    // #19 gives a node its thumbnail from a run; the size follows the image's aspect, and the
    // hook it calls for later changes of content tells the views.
    void aPreviewGrowsTheNodeByItsAspectAndAPinnedHeightGivesItTheRest() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        const qreal plain = node(canvas, "save").value("nodeHeight").toDouble();
        canvas.setRunState("save", {{"state", "cached"}, {"thumbnail", "file:///x.png"}, {"thumbnailAspect", 0.5}});
        QCOMPARE(node(canvas, "save").value("thumbnailRect").toRectF().height(), 114.0);
        QCOMPARE(node(canvas, "save").value("nodeHeight").toDouble(), plain + 4 + 114);
        canvas.beginResize("save");
        canvas.resizeBy(0, 100);
        canvas.endResize();
        QCOMPARE(node(canvas, "save").value("nodeHeight").toDouble(), plain + 4 + 114 + 100);
        QCOMPARE(node(canvas, "save").value("thumbnailRect").toRectF().height(), 214.0);
        // Narrower, the aspect makes it shorter; the pinned height keeps the node's.
        canvas.beginResize("save");
        canvas.resizeBy(-80, 0);
        canvas.endResize();
        QCOMPARE(node(canvas, "save").value("nodeWidth").toDouble(), 160.0);
        QVERIFY(!canvas.refreshSize("save")); // nothing changed since
    }

    // A pinned height that is only spare room above the content shrinks back to the content
    // when the content shrinks (a thumbnail clears), never below it; the pin is kept, so the
    // picture has its room again when it comes back. Room pinned over the content as it is
    // stays (Appendix A).
    void aPinnedHeightShrinksBackToTheContentWhenTheThumbnailClears() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        const qreal plain = node(canvas, "save").value("nodeHeight").toDouble();
        const QVariantMap picture{{"state", "cached"}, {"thumbnail", "file:///x.png"}, {"thumbnailAspect", 0.5}};
        canvas.setRunState("save", picture);
        canvas.beginResize("save");
        canvas.resizeBy(0, 100);
        canvas.endResize();
        const qreal pinned = plain + 4 + 114 + 100;
        QCOMPARE(node(canvas, "save").value("nodeHeight").toDouble(), pinned);

        canvas.setRunState("save", {{"state", "stale"}}); // an edit dropped the result
        QCOMPARE(node(canvas, "save").value("nodeHeight").toDouble(), plain);
        QCOMPARE(canvas.document().layout.value("nodes").toObject().value("save").toObject().value("size").toArray(),
                 (QJsonArray{240, pinned}));
        canvas.setRunState("save", picture);
        QCOMPARE(node(canvas, "save").value("nodeHeight").toDouble(), pinned);
        QCOMPARE(node(canvas, "save").value("thumbnailRect").toRectF().height(), 214.0);

        // Blank room pinned on purpose, over content that has not shrunk, stays.
        const qreal gen = node(canvas, "gen").value("nodeHeight").toDouble();
        canvas.beginResize("gen");
        canvas.resizeBy(0, 50);
        canvas.endResize();
        canvas.setRunState("gen", {{"state", "stale"}});
        QCOMPARE(node(canvas, "gen").value("nodeHeight").toDouble(), gen + 50);
    }

    void anOptionalNumberWithNoValueShowsAutoDimmed() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        const QVariantMap width = port(node(canvas, "gen"), "inputs", "width");
        QCOMPARE(width.value("value").toString(), QString("auto"));
        QVERIFY(width.value("isDefault").toBool());
        QVERIFY(width.value("placeholder").toBool());
        // One with a default shows it, and is no placeholder.
        const QVariantMap strength = port(node(canvas, "gen"), "inputs", "strength");
        QCOMPARE(strength.value("value").toString(), QString("0.7"));
        QVERIFY(!strength.value("placeholder").toBool());
    }

    void aMissingTypesLinkedInputTakesTheLinkedTypesShape() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(workflow(placed)));
        const QVariantMap ghost = port(node(canvas, "ghost"), "inputs", "in");
        const QVariantMap source = port(node(canvas, "prompt"), "outputs", "text");
        QVERIFY(ghost.value("wired").toBool());
        QCOMPARE(ghost.value("shape"), source.value("shape"));
        QCOMPARE(ghost.value("color"), source.value("color"));
    }
};

QTEST_MAIN(TestCanvasSize)
#include "test_canvas_size.moc"
