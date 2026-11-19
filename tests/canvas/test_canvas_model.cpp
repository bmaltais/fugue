// The canvas model (src/canvas/canvas_model.h) with the real built-in node types: what a
// workflow document looks like on the canvas, and the edits the canvas makes to it.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

#include "canvas/canvas_model.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;

namespace {

Document document(const char *json) {
    const Result<Document> loaded = Document::load(QByteArray(json));
    if (!loaded.ok())
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

// prompt (Text) -> gen (Generate Image) -> save (Save Image), placed by the layout.
const char *threeNodes = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0",
            "inputs": {"prompt": {"from": "prompt.text"}, "seed": {"auto": "random"}}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [0, 100]}, "gen": {"pos": [300, 80], "title": "Fox"},
                       "save": {"pos": [640, 120]}},
             "comments": [{"id": "c1", "text": "keep me", "pos": [0, 0], "size": [10, 10]}]}
})";

QVariantMap row(QAbstractItemModel *model, int index) {
    QVariantMap values;
    const QHash<int, QByteArray> roles = model->roleNames();
    for (auto it = roles.begin(); it != roles.end(); ++it)
        values.insert(QString::fromUtf8(it.value()), model->data(model->index(index, 0), it.key()));
    return values;
}

// The row of the node with this id.
QVariantMap node(CanvasModel &canvas, const QString &id) {
    for (int i = 0; i < canvas.nodes()->rowCount(); ++i)
        if (row(canvas.nodes(), i).value("nodeId") == id)
            return row(canvas.nodes(), i);
    return {};
}

QVariantMap port(const QVariantMap &node, const QString &side, const QString &name) {
    for (const QVariant &p : node.value(side).toList())
        if (p.toMap().value("name") == name)
            return p.toMap();
    return {};
}

// The link into `to` ("node.input").
QVariantMap link(CanvasModel &canvas, const QString &to) {
    for (int i = 0; i < canvas.links()->rowCount(); ++i) {
        const QVariantMap l = row(canvas.links(), i);
        if (l.value("toNode").toString() + "." + l.value("toPort").toString() == to)
            return l;
    }
    return {};
}

} // namespace

class TestCanvasModel : public QObject {
    Q_OBJECT

    Registry m_registry;

private slots:
    void initTestCase() { nodes::addBuiltinNodeTypes(m_registry); }

    void showsEveryNodeWhereTheLayoutPutsIt() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        QCOMPARE(canvas.nodes()->rowCount(), 3);
        const QVariantMap gen = node(canvas, "gen");
        QCOMPARE(gen.value("posX").toDouble(), 300.0);
        QCOMPARE(gen.value("posY").toDouble(), 80.0);
        // The layout's display title wins over the type's name.
        QCOMPARE(gen.value("title").toString(), QString("Fox"));
        QCOMPARE(node(canvas, "save").value("title").toString(), QString("Save Image"));
    }

    void wiresRunBetweenTheirPorts() {
        // Text's one output sits on the first row (26 + 11). Generate Image has five outputs
        // beside its wire-only inputs, so its first value input, prompt, is on row 6 (26 + 5 * 22 + 11).
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        QCOMPARE(canvas.links()->rowCount(), 2);
        const QVariantMap l = link(canvas, "gen.prompt");
        QCOMPARE(l.value("fromNode").toString(), QString("prompt"));
        QCOMPARE(l.value("fromPort").toString(), QString("text"));
        QCOMPARE(l.value("x1").toDouble(), 220.0);
        QCOMPARE(l.value("y1").toDouble(), 137.0);
        QCOMPARE(l.value("x2").toDouble(), 300.0);
        QCOMPARE(l.value("y2").toDouble(), 227.0);
        QCOMPARE(l.value("color").toString(), QString("green"));
        QVERIFY(l.value("path").toString().startsWith("M 220.0 137.0 C"));
    }

    void portsShowTheirTypeByColorAndShape() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {}},
            "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {}},
            "edit": {"type": "fugue/edit-image", "version": "1.0.0", "inputs": {}}}}})"));
        const QVariantMap gen = node(canvas, "gen");
        QCOMPARE(port(gen, "inputs", "prompt").value("shape").toString(), QString("circle"));
        QCOMPARE(port(gen, "inputs", "prompt").value("color").toString(), QString("green"));
        QCOMPARE(port(gen, "inputs", "image").value("shape").toString(), QString("square"));
        QCOMPARE(port(gen, "inputs", "image").value("color").toString(), QString("blue"));
        QCOMPARE(port(gen, "inputs", "model").value("shape").toString(), QString("diamond"));
        QCOMPARE(port(gen, "outputs", "seed").value("color").toString(), QString("yellow"));
        QCOMPARE(port(node(canvas, "show"), "inputs", "value").value("shape").toString(), QString("ring"));
        QVERIFY(port(node(canvas, "edit"), "inputs", "images").value("list").toBool());
    }

    void valueInputsShowTheirValues() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        QCOMPARE(port(node(canvas, "prompt"), "inputs", "text").value("value").toString(), QString("a red fox"));
        const QVariantMap gen = node(canvas, "gen");
        QCOMPARE(port(gen, "inputs", "seed").value("value").toString(), QString("random"));
        QCOMPARE(port(gen, "inputs", "strength").value("value").toString(), QString("0.7"));
        QVERIFY(port(gen, "inputs", "strength").value("isDefault").toBool());
        QVERIFY(port(gen, "inputs", "prompt").value("wired").toBool());
        QVERIFY(!port(gen, "inputs", "image").value("wired").toBool());
        QVERIFY(port(gen, "outputs", "image").value("wired").toBool());
        QCOMPARE(port(node(canvas, "save"), "inputs", "name").value("value").toString(), QString("fugue"));
    }

    void linkProblemsSayWhy() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        QCOMPARE(canvas.linkProblem("gen", "image", "save", "image"), QString());
        QCOMPARE(canvas.linkProblem("prompt", "text", "save", "name"), QString());
        const QString typeProblem = canvas.linkProblem("prompt", "text", "save", "image");
        QVERIFY2(typeProblem.contains("text") && typeProblem.contains("image"), qPrintable(typeProblem));
        QVERIFY(canvas.linkProblem("gen", "image", "gen", "image").contains("itself"));
        // save reads from gen, so gen cannot read from save.
        QVERIFY(canvas.linkProblem("save", "file", "gen", "image").contains("cycle"));
        QVERIFY(!canvas.linkProblem("gen", "nope", "save", "image").isEmpty());
    }

    void connectReplacesASingleInputsLink() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        const QString load = canvas.addNode("fugue/load-image", 0, 400);
        QVERIFY(!load.isEmpty());
        QSignalSpy edited(&canvas, &CanvasModel::edited);
        QVERIFY(canvas.connectPorts(load, "image", "save", "image"));
        QCOMPARE(edited.count(), 1);
        QCOMPARE(canvas.links()->rowCount(), 2);
        QCOMPARE(link(canvas, "save.image").value("fromNode").toString(), load);
        QCOMPARE(canvas.document().graph.nodes.value("save").inputs.value("image").from().node, load);
        QVERIFY(!port(node(canvas, "gen"), "outputs", "image").value("wired").toBool());
    }

    void connectAddsAnItemToAListInput() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        const QString edit = canvas.addNode("fugue/edit-image", 600, 400);
        const QString load = canvas.addNode("fugue/load-image", 0, 400);
        QVERIFY(canvas.connectPorts("gen", "image", edit, "images"));
        QVERIFY(canvas.connectPorts(load, "image", edit, "images"));
        const InputValue images = canvas.document().graph.nodes.value(edit).inputs.value("images");
        QCOMPARE(images.kind(), InputValue::Kind::List);
        QCOMPARE(images.items().size(), 2);
        QCOMPARE(images.items()[1].from().node, load);
        QCOMPARE(canvas.links()->rowCount(), 4);
        QVERIFY(canvas.disconnectInput(edit, "images", 0));
        QCOMPARE(canvas.document().graph.nodes.value(edit).inputs.value("images").items().size(), 1);
        QCOMPARE(canvas.links()->rowCount(), 3);
    }

    void aRefusedLinkChangesNothing() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        const QByteArray before = canvas.document().save();
        QSignalSpy edited(&canvas, &CanvasModel::edited);
        QVERIFY(!canvas.connectPorts("prompt", "text", "save", "image"));
        QVERIFY(!canvas.connectPorts("save", "file", "gen", "image"));
        QCOMPARE(edited.count(), 0);
        QCOMPARE(canvas.document().save(), before);
    }

    void disconnectRemovesTheWire() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        QVERIFY(canvas.disconnectInput("save", "image"));
        QCOMPARE(canvas.links()->rowCount(), 1);
        QVERIFY(!canvas.document().graph.nodes.value("save").inputs.contains("image"));
        QVERIFY(!canvas.disconnectInput("save", "image"));
    }

    void draggingALinkMarksWhatItCannotReach() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        QSignalSpy dragChanged(&canvas, &CanvasModel::linkDragChanged);
        canvas.beginLinkDrag("gen", "image", true);
        QCOMPARE(dragChanged.count(), 1);
        QCOMPARE(canvas.linkDrag().value("node").toString(), QString("gen"));
        QCOMPARE(canvas.linkDrag().value("x").toDouble(), 520.0);
        QCOMPARE(canvas.dragProblem("save", "image", false), QString());
        QVERIFY(!canvas.dragProblem("save", "name", false).isEmpty());
        QVERIFY(!canvas.dragProblem("prompt", "text", true).isEmpty()); // output to output
        QVERIFY(canvas.dragProblem("gen", "image", false).contains("itself"));
        QVERIFY(!canvas.endLinkDrag()); // let go on nothing: no link, and the drag is over
        QVERIFY(canvas.linkDrag().isEmpty());
    }

    void draggingFromAnInputReachesBackToAnOutput() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        const QString load = canvas.addNode("fugue/load-image", 0, 400);
        canvas.beginLinkDrag("gen", "image", false); // gen's start image, not linked yet
        QCOMPARE(canvas.dragProblem(load, "image", true), QString());
        QVERIFY(canvas.dragProblem("save", "file", true).contains("cycle"));
        QVERIFY(canvas.endLinkDrag(load, "image", true));
        QCOMPARE(link(canvas, "gen.image").value("fromNode").toString(), load);
    }

    void draggingALinkedInputPicksTheLinkUp() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        const QString load = canvas.addNode("fugue/load-image", 0, 400);
        canvas.beginLinkDrag("save", "image", false);
        // The link is off save and the drag goes on from its source, as in ComfyUI.
        QCOMPARE(canvas.links()->rowCount(), 1);
        QCOMPARE(canvas.linkDrag().value("node").toString(), QString("gen"));
        QVERIFY(canvas.linkDrag().value("output").toBool());
        QVERIFY(!canvas.endLinkDrag(load, "image", true)); // an output again: nothing
        QCOMPARE(canvas.links()->rowCount(), 1);
    }

    void endLinkDragReturnsWhetherItConnected() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        canvas.beginLinkDrag("prompt", "text", true);
        QVERIFY(!canvas.endLinkDrag("save", "image", false));
        canvas.beginLinkDrag("prompt", "text", true);
        QVERIFY(canvas.endLinkDrag("save", "name", false));
        QCOMPARE(link(canvas, "save.name").value("fromNode").toString(), QString("prompt"));
    }

    void movingANodeTouchesOnlyItsOwnLinks() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        QSignalSpy linkChanges(canvas.links(), &QAbstractItemModel::dataChanged);
        QSignalSpy nodeChanges(canvas.nodes(), &QAbstractItemModel::dataChanged);
        canvas.beginMove("save");
        canvas.moveBy(10, -20);
        QCOMPARE(nodeChanges.count(), 1);
        QCOMPARE(linkChanges.count(), 1);
        const QVariantMap l = link(canvas, "save.image");
        QCOMPARE(linkChanges.first().at(0).value<QModelIndex>().row(),
                 linkChanges.first().at(1).value<QModelIndex>().row());
        QCOMPARE(l.value("x2").toDouble(), 650.0);
        QSignalSpy edited(&canvas, &CanvasModel::edited);
        canvas.endMove();
        QCOMPARE(edited.count(), 1);
        QCOMPARE(canvas.position("save"), QPointF(650, 100));
    }

    void aMoveCarriesTheSelection() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        canvas.select({"prompt", "gen"});
        QCOMPARE(canvas.selectedIds(), (QStringList{"gen", "prompt"}));
        canvas.beginMove("gen");
        canvas.moveBy(5, 5);
        canvas.endMove();
        QCOMPARE(canvas.position("prompt"), QPointF(5, 105));
        QCOMPARE(canvas.position("gen"), QPointF(305, 85));
        QCOMPARE(canvas.position("save"), QPointF(640, 120));
        // A node outside the selection moves alone.
        canvas.beginMove("save");
        canvas.moveBy(1, 1);
        canvas.endMove();
        QCOMPARE(canvas.position("gen"), QPointF(305, 85));
    }

    void selectAddsSubtractsAndToggles() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        canvas.select({"prompt"});
        canvas.select({"gen"}, "add");
        QCOMPARE(canvas.selectedIds().size(), 2);
        canvas.select({"prompt"}, "subtract");
        QCOMPARE(canvas.selectedIds(), QStringList{"gen"});
        canvas.select({"gen", "save"}, "toggle");
        QCOMPARE(canvas.selectedIds(), QStringList{"save"});
        QVERIFY(node(canvas, "save").value("selected").toBool());
    }

    void theDocumentKeepsItsLayoutAndGainsThePositions() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        canvas.beginMove("gen");
        canvas.moveBy(0.4, 10.6);
        canvas.endMove();
        canvas.setView(100.2, 50, 1.23456);
        const QJsonObject layout = canvas.document().layout;
        const QJsonObject gen = layout.value("nodes").toObject().value("gen").toObject();
        QCOMPARE(gen.value("pos").toArray(), (QJsonArray{300, 91}));
        QCOMPARE(gen.value("title").toString(), QString("Fox"));
        QCOMPARE(layout.value("comments").toArray().size(), 1);
        QCOMPARE(layout.value("view").toObject().value("zoom").toDouble(), 1.235);
        QCOMPARE(layout.value("view").toObject().value("center").toArray(), (QJsonArray{100, 50}));
        // The graph is untouched by a move.
        QCOMPARE(canvas.document().sourceHash(), document(threeNodes).sourceHash());
        // And it reads back the same.
        CanvasModel again(&m_registry);
        again.setDocument(canvas.document());
        QCOMPARE(again.document().save(), canvas.document().save());
        QCOMPARE(again.view().value("zoom").toDouble(), 1.235);
    }

    void aMissingTypeIsAPlaceholderThatKeepsItsLinks() {
        CanvasModel canvas(&m_registry);
        const Document doc = document(R"({"fugue": 1, "graph": {"nodes": {
            "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "x"}},
            "odd": {"type": "io.example.tools/sharpen", "version": "2.0.0",
                    "inputs": {"image": {"from": "prompt.text"}, "amount": 3}},
            "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "odd.image"}}}}}})");
        canvas.setDocument(doc);
        const QVariantMap odd = node(canvas, "odd");
        QVERIFY(odd.value("missing").toBool());
        QCOMPARE(odd.value("title").toString(), QString("io.example.tools/sharpen"));
        QVERIFY(port(odd, "inputs", "image").value("wired").toBool());
        QCOMPARE(port(odd, "inputs", "amount").value("value").toString(), QString("3"));
        QVERIFY(port(odd, "outputs", "image").value("wired").toBool());
        QCOMPARE(canvas.links()->rowCount(), 2);
        QVERIFY(canvas.linkProblem("odd", "image", "save", "image").contains("not installed"));
        QCOMPARE(canvas.document().graph.nodes.value("odd").inputs.size(), 2);
    }

    void aGroupInstanceShowsItsExposedPorts() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "g": {"type": "local/two-pass", "version": "1.0.0", "inputs": {}}}},
          "groups": {"local/two-pass": {"version": "1.0.0", "name": "Two-pass generate",
            "inputs": [{"name": "prompt", "targets": ["first.prompt"]}],
            "outputs": [{"name": "image", "from": "first.image"}],
            "nodes": {"first": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {}}}}}})"));
        const QVariantMap g = node(canvas, "g");
        QCOMPARE(g.value("title").toString(), QString("Two-pass generate"));
        QVERIFY(!g.value("missing").toBool());
        QCOMPARE(port(g, "inputs", "prompt").value("type").toString(), QString("text"));
        QCOMPARE(port(g, "outputs", "image").value("shape").toString(), QString("square"));
    }

    void nodesTheLayoutDoesNotPlaceGoInColumns() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "a": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "x"}},
            "b": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "y"}},
            "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": {"from": "a.text"}}},
            "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}}}}}})"));
        QCOMPARE(canvas.position("a"), QPointF(0, 0));
        // Text is 26 + 2 rows * 22 + 6 = 76 high, and a stack leaves 40 between nodes.
        QCOMPARE(canvas.position("b"), QPointF(0, 116));
        QCOMPARE(canvas.position("gen"), QPointF(300, 0));
        QCOMPARE(canvas.position("save"), QPointF(600, 0));
    }

    void hitTestsFindPortsAndNodes() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        const QVariantMap hit = canvas.portAt(222, 139, 8);
        QCOMPARE(hit.value("node").toString(), QString("prompt"));
        QCOMPARE(hit.value("port").toString(), QString("text"));
        QVERIFY(hit.value("output").toBool());
        QVERIFY(canvas.portAt(110, 139, 8).isEmpty());
        QCOMPARE(canvas.nodeAt(110, 139), QString("prompt"));
        QCOMPARE(canvas.nodeAt(-10, 139), QString());
        QCOMPARE(canvas.nodesIn(QRectF(-10, 50, 600, 400)), (QStringList{"gen", "prompt"}));
    }

    void anotherRowHeightLaysEveryNodeOutAgain() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        canvas.setRowHeight(30);
        QCOMPARE(link(canvas, "gen.prompt").value("y1").toDouble(), 100.0 + 26 + 15);
    }

    void framesComeFromTheLayout() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {}},
            "layout": {"frames": [{"id": "f1", "title": "Inputs", "rect": [0, 10, 380, 300], "color": "muted", "nodes": []}]}})"));
        QCOMPARE(canvas.frames().size(), 1);
        const QVariantMap frame = canvas.frames().first().toMap();
        QCOMPARE(frame.value("title").toString(), QString("Inputs"));
        QCOMPARE(frame.value("height").toDouble(), 300.0);
    }

    void aCollapsedNodeKeepsItsWiresOnItsTitle() {
        CanvasModel canvas(&m_registry);
        Document doc = document(threeNodes);
        QJsonObject layout = doc.layout;
        QJsonObject nodes = layout.value("nodes").toObject();
        nodes.insert("gen", QJsonObject{{"pos", QJsonArray{300, 80}}, {"collapsed", true}});
        layout.insert("nodes", nodes);
        doc.layout = layout;
        canvas.setDocument(doc);
        QCOMPARE(node(canvas, "gen").value("nodeHeight").toDouble(), 26.0);
        QCOMPARE(link(canvas, "gen.prompt").value("y2").toDouble(), 93.0);
    }

    void reroutePointsBendTheWire() {
        CanvasModel canvas(&m_registry);
        Document doc = document(threeNodes);
        QJsonObject layout = doc.layout;
        layout.insert("links", QJsonObject{{"save.image", QJsonObject{{"points", QJsonArray{QJsonArray{580, 400}}}}}});
        doc.layout = layout;
        canvas.setDocument(doc);
        QCOMPARE(link(canvas, "save.image").value("path").toString().count(" C "), 2);
        QCOMPARE(link(canvas, "gen.prompt").value("path").toString().count(" C "), 1);
    }
};

QTEST_MAIN(TestCanvasModel)
#include "test_canvas_model.moc"
