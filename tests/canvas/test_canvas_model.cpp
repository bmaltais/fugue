// The canvas model (src/canvas/canvas_model.h) with the real built-in node types: what a
// workflow document looks like on the canvas, and the edits the canvas makes to it.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

#include "canvas/canvas_model.h"
#include "canvas/widget_edit.h"
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

    // The manifest's description reaches the port map, for the hover help on a node's row (#hover).
    void portsCarryTheirDescription() {
        Registry registry;
        NodeType type;
        type.id = "io.example.tools/soften";
        type.version = *Version::parse("1.0.0");
        type.name = "Soften";
        type.runtime = QJsonObject{{"kind", "builtin"}};
        InputSpec blur;
        blur.name = "blur";
        blur.type = {*PortType::parse("number"), false};
        blur.description = "How far the edge is blurred.";
        type.inputs.append(blur);
        InputSpec plain = blur;
        plain.name = "plain";
        plain.description.clear();
        type.inputs.append(plain);
        OutputSpec out;
        out.name = "image";
        out.type = {*PortType::parse("image"), false};
        out.description = "The softened picture.";
        type.outputs.append(out);
        QVERIFY(registry.add(type).isEmpty());
        CanvasModel canvas(&registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "s": {"type": "io.example.tools/soften", "version": "1.0.0"}}}})"));
        const QVariantMap s = node(canvas, "s");
        QCOMPARE(port(s, "inputs", "blur").value("description").toString(), QString("How far the edge is blurred."));
        QVERIFY(!port(s, "inputs", "plain").contains("description"));
        QCOMPARE(port(s, "outputs", "image").value("description").toString(), QString("The softened picture."));
    }

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
        // docs/canvas-look.md 4: a 28 px header (7u at text size 12), 4 px of padding, then
        // 24 px rows, outputs first. Text's one output is on its first row (28 + 4 + 12); Generate
        // Image's five outputs come before its inputs, so prompt is on row 6 (28 + 4 + 5 * 24 + 12).
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        QCOMPARE(canvas.links()->rowCount(), 2);
        const QVariantMap l = link(canvas, "gen.prompt");
        QCOMPARE(l.value("fromNode").toString(), QString("prompt"));
        QCOMPARE(l.value("fromPort").toString(), QString("text"));
        QCOMPARE(l.value("x1").toDouble(), 240.0);
        QCOMPARE(l.value("y1").toDouble(), 144.0);
        QCOMPARE(l.value("x2").toDouble(), 300.0);
        QCOMPARE(l.value("y2").toDouble(), 244.0);
        QCOMPARE(l.value("color").toString(), QString("text"));
        // Horizontal tangents half the run apart, at least 6u (8.1).
        QCOMPARE(l.value("path").toString(), QString("M 240.0 144.0 C 270.0 144.0 270.0 244.0 300.0 244.0"));
    }

    void outputsComeFirstThenInputsInTheirOrder() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        QCOMPARE(canvas.headerHeight(), 28.0);
        QCOMPARE(canvas.rowHeight(), 24.0);
        QCOMPARE(canvas.defaultWidth(), 240.0);
        const QVariantMap gen = node(canvas, "gen");
        QCOMPARE(gen.value("nodeWidth").toDouble(), 240.0);
        const QList<QVariant> outputs = gen.value("outputs").toList();
        for (int i = 0; i < outputs.size(); ++i) {
            QCOMPARE(outputs[i].toMap().value("y").toDouble(), 44.0 + 24 * i);
            QCOMPARE(outputs[i].toMap().value("rowTop").toDouble(), 32.0 + 24 * i);
        }
        // prompt is linked: a plain 24 px row. negative_prompt is an unlinked multiline: a 64 px
        // row (16u), its port level with the middle of a normal row.
        QCOMPARE(port(gen, "inputs", "prompt").value("y").toDouble(), 164.0);
        QCOMPARE(port(gen, "inputs", "prompt").value("rowHeight").toDouble(), 24.0);
        QCOMPARE(port(gen, "inputs", "negative_prompt").value("rowTop").toDouble(), 176.0);
        QCOMPARE(port(gen, "inputs", "negative_prompt").value("rowHeight").toDouble(), 64.0);
        QCOMPARE(port(gen, "inputs", "negative_prompt").value("y").toDouble(), 188.0);
        QCOMPARE(port(gen, "inputs", "model").value("rowTop").toDouble(), 240.0);
        // Text: header, padding, its output, its unlinked multiline input, 8 px under the last row.
        QCOMPARE(node(canvas, "prompt").value("nodeHeight").toDouble(), 28.0 + 4 + 24 + 64 + 8);
    }

    void widgetsSayWhatTheyAre() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        const QVariantMap gen = node(canvas, "gen");
        QCOMPARE(port(gen, "inputs", "prompt").value("widgetType").toString(), QString("multiline"));
        QCOMPARE(port(gen, "inputs", "width").value("widgetType").toString(), QString("spin"));
        QCOMPARE(port(gen, "inputs", "seed").value("widgetType").toString(), QString("seed"));
        QCOMPARE(port(gen, "inputs", "seed").value("value").toString(), QString("random"));
        QCOMPARE(port(gen, "inputs", "strength").value("widgetType").toString(), QString("slider"));
        // A slider's fill is its value's share of the range: 0.7 of 0 to 1.
        QCOMPARE(port(gen, "inputs", "strength").value("share").toDouble(), 0.7);
        // A resource input shows the model picker (spec 2.5, 6.2).
        QCOMPARE(port(gen, "inputs", "model").value("widgetType").toString(), QString("model"));
        QVERIFY(port(gen, "inputs", "model").value("widget").toBool());
        // Media inputs are wire-only.
        QVERIFY(!port(gen, "inputs", "image").value("widget").toBool());
        const QVariantMap save = node(canvas, "save");
        QCOMPARE(port(save, "inputs", "format").value("widgetType").toString(), QString("choice"));
        QCOMPARE(port(save, "inputs", "folder").value("widgetType").toString(), QString("folder"));
    }

    void aLinkIntoAMultilineInputShrinksItsRow() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        QCOMPARE(port(node(canvas, "gen"), "inputs", "negative_prompt").value("rowHeight").toDouble(), 64.0);
        const qreal before = node(canvas, "gen").value("nodeHeight").toDouble();
        QVERIFY(canvas.connectPorts("prompt", "text", "gen", "negative_prompt"));
        QCOMPARE(port(node(canvas, "gen"), "inputs", "negative_prompt").value("rowHeight").toDouble(), 24.0);
        QCOMPARE(node(canvas, "gen").value("nodeHeight").toDouble(), before - 40);
        // The wires into the rows below follow.
        QCOMPARE(link(canvas, "gen.negative_prompt").value("y2").toDouble(), 80.0 + 188);
    }

    void aBackwardWireLoopsOutAndBack() {
        // From prompt's output (240, 144) back to a node left of it: the tangents are
        // 12u + a quarter of the run (at most 100u), so the wire leaves and enters horizontally.
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "x"}},
            "back": {"type": "fugue/text-concat", "version": "1.0.0", "inputs": {"a": {"from": "prompt.text"}}}}},
            "layout": {"nodes": {"prompt": {"pos": [0, 100]}, "back": {"pos": [-200, 100]}}}})"));
        const QVariantMap l = link(canvas, "back.a");
        QCOMPARE(l.value("x1").toDouble(), 240.0);
        QCOMPARE(l.value("x2").toDouble(), -200.0);
        // |dx| is 440: 48 + min(440, 400) / 4 = 148.
        QVERIFY2(l.value("path").toString().startsWith("M 240.0 144.0 C 388.0 144.0 -348.0 "),
                 qPrintable(l.value("path").toString()));
    }

    void theWireUnderThePointerIsFound() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        // On the prompt -> gen wire, halfway: (270, 194) on its cubic.
        const QVariantMap hit = canvas.linkAt(270, 194, 6);
        QCOMPARE(hit.value("toNode").toString(), QString("gen"));
        QCOMPARE(hit.value("toPort").toString(), QString("prompt"));
        QCOMPARE(hit.value("midX").toDouble(), 270.0);
        QCOMPARE(hit.value("midY").toDouble(), 194.0);
        QVERIFY(hit.contains("row"));
        QVERIFY(canvas.linkAt(270, 230, 6).isEmpty());
        QVERIFY(canvas.linkAt(-500, -500, 6).isEmpty());
    }

    void portsShowTheirTypeByColorAndShape() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {}},
            "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {}},
            "edit": {"type": "fugue/edit-image", "version": "1.0.0", "inputs": {}}}}})"));
        const QVariantMap gen = node(canvas, "gen");
        QCOMPARE(port(gen, "inputs", "prompt").value("shape").toString(), QString("circle"));
        QCOMPARE(port(gen, "inputs", "prompt").value("color").toString(), QString("text"));
        QCOMPARE(port(gen, "inputs", "image").value("shape").toString(), QString("square"));
        QCOMPARE(port(gen, "inputs", "image").value("color").toString(), QString("image"));
        QCOMPARE(port(gen, "inputs", "model").value("shape").toString(), QString("diamond"));
        QCOMPARE(port(gen, "inputs", "model").value("color").toString(), QString("declared"));
        QCOMPARE(port(gen, "outputs", "seed").value("color").toString(), QString("number"));
        QCOMPARE(port(node(canvas, "show"), "inputs", "value").value("shape").toString(), QString("ring"));
        QCOMPARE(port(node(canvas, "show"), "inputs", "value").value("color").toString(), QString("any"));
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
        QCOMPARE(canvas.linkDrag().value("x").toDouble(), 540.0);
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
        canvas.beginMove("save"); // raises the node (its stack role): not a move's change
        nodeChanges.clear();
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

    // Overlapping nodes: the one selected or moved last comes to the front, also when it was already selected.
    void aSelectedOrMovedNodeComesToTheFront() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        const auto stack = [&](const QString &id) { return node(canvas, id).value("stack").toInt(); };
        QCOMPARE(stack("gen"), 0);
        canvas.select({"gen"});
        canvas.select({"save"});
        QVERIFY(stack("save") > stack("gen"));
        canvas.select({"gen"});
        QVERIFY(stack("gen") > stack("save"));
        // Pressing a node of a selection of two and dragging it raises that one.
        canvas.select({"gen", "save"});
        canvas.beginMove("save");
        canvas.endMove();
        QVERIFY(stack("save") > stack("gen"));
        // Taking a node out of the selection raises nothing.
        const int top = stack("save");
        canvas.select({"gen"}, "subtract");
        QCOMPARE(stack("save"), top);
        QCOMPARE(stack("prompt"), 0);
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

    // Spec 4.4: an exposed input narrows its target's constraints and may set a default; the
    // face and its widget follow the group, as the run's checks do (compiler.cpp).
    void aGroupInputTakesTheGroupsNarrowedConstraintsAndDefault() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "g": {"type": "local/two-pass", "version": "1.0.0", "inputs": {}}}},
          "groups": {"local/two-pass": {"version": "1.0.0", "name": "Two-pass",
            "inputs": [{"name": "steps", "targets": ["second.steps"], "min": 1, "max": 12, "default": 6},
                       {"name": "format", "targets": ["save.format"], "choices": ["png"]},
                       {"name": "fit", "targets": ["size.fit"]}],
            "outputs": [{"name": "image", "from": "second.image"}],
            "nodes": {"second": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {}},
                      "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {}},
                      "size": {"type": "fugue/resize", "version": "1.0.0", "inputs": {"fit": "cover"}}}}}})"));
        WidgetEdit *edits = canvas.widgets();
        const QVariantMap steps = edits->spec("g", "steps");
        QCOMPARE(steps.value("max").toDouble(), 12.0);
        QCOMPARE(steps.value("default").toDouble(), 6.0);
        QVERIFY(!edits->check("g", "steps", "50").isEmpty());
        QVERIFY(edits->check("g", "steps", "12").isEmpty());
        QCOMPARE(edits->stepped("g", "steps", 12, 1), 12.0);
        const QVariantMap g = node(canvas, "g");
        QCOMPARE(port(g, "inputs", "steps").value("value").toString(), QString("6"));
        QCOMPARE(edits->spec("g", "format").value("choices").toStringList(), QStringList{"png"});
        QVERIFY(!edits->check("g", "format", "webp").isEmpty());
        // Unset on the instance and without a group default: the inner node's own value.
        QCOMPARE(port(g, "inputs", "fit").value("value").toString(), QString("cover"));
    }

    // Inputs a type does not define (a placeholder's, or one the installed version lacks)
    // show their value but cannot be typed into: there is no spec to check it against.
    void inputsWithoutASpecAreReadOnly() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "odd": {"type": "io.example.tools/sharpen", "version": "2.0.0", "inputs": {"amount": 3}},
            "text": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "x", "extra": "kept"}}}}})"));
        const QVariantMap amount = port(node(canvas, "odd"), "inputs", "amount");
        QCOMPARE(amount.value("value").toString(), QString("3"));
        QCOMPARE(amount.value("widgetType").toString(), QString());
        QCOMPARE(port(node(canvas, "text"), "inputs", "extra").value("widgetType").toString(), QString());
        QCOMPARE(port(node(canvas, "text"), "inputs", "extra").value("value").toString(), QString("kept"));
    }

    // The engine passes an output the first input of its type and its list-ness (3.5).
    void aBypassPassesOnlyAnInputOfTheSameListness() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "edit": {"type": "fugue/edit-image", "version": "1.0.0", "inputs": {}, "mode": "bypass"}}}})"));
        const QVariantMap edit = node(canvas, "edit");
        QVERIFY(!port(edit, "outputs", "image").contains("passFrom")); // images is a list
        QCOMPARE(port(edit, "outputs", "seed").value("passFrom").toString(), QString("steps"));
    }

    void nodesTheLayoutDoesNotPlaceGoInColumns() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "a": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "x"}},
            "b": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "y"}},
            "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": {"from": "a.text"}}},
            "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}}}}}})"));
        QCOMPARE(canvas.position("a"), QPointF(0, 0));
        // Text is 28 + 4 + 24 + 64 (its multiline input) + 8 = 128 high, a stack leaves 40
        // between nodes, and columns are a node's 240 and 80 apart.
        QCOMPARE(canvas.position("b"), QPointF(0, 168));
        QCOMPARE(canvas.position("gen"), QPointF(320, 0));
        QCOMPARE(canvas.position("save"), QPointF(640, 0));
    }

    void hitTestsFindPortsAndNodes() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        const QVariantMap hit = canvas.portAt(242, 146, 8);
        QCOMPARE(hit.value("node").toString(), QString("prompt"));
        QCOMPARE(hit.value("port").toString(), QString("text"));
        QVERIFY(hit.value("output").toBool());
        QVERIFY(canvas.portAt(110, 146, 8).isEmpty());
        QCOMPARE(canvas.nodeAt(110, 146), QString("prompt"));
        QCOMPARE(canvas.nodeAt(-10, 146), QString());
        QCOMPARE(canvas.nodesIn(QRectF(-10, 50, 600, 400)), (QStringList{"gen", "prompt"}));
        // As in ComfyUI, a node the rectangle touches is in it: gen spans x 300 to 540.
        QCOMPARE(canvas.nodesIn(QRectF(500, 300, 100, 50)), QStringList{"gen"});
    }

    void anotherTextSizeLaysEveryNodeAndWireOutAgain() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        QSignalSpy nodesReset(canvas.nodes(), &QAbstractItemModel::modelReset);
        QSignalSpy linksReset(canvas.links(), &QAbstractItemModel::modelReset);
        // At 15 px the unit is 5: a row is 30 high (6u), the title 35 (7u), a node 300 wide (60u).
        canvas.setTextSize(15);
        QCOMPARE(canvas.rowHeight(), 30.0);
        QCOMPARE(canvas.headerHeight(), 35.0);
        QCOMPARE(node(canvas, "gen").value("nodeWidth").toDouble(), 300.0);
        QCOMPARE(nodesReset.count(), 1);
        QCOMPARE(linksReset.count(), 1);
        QCOMPARE(link(canvas, "gen.prompt").value("y1").toDouble(), 100.0 + 35 + 5 + 15);
        canvas.setTextSize(15);
        QCOMPARE(linksReset.count(), 1);
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
        QCOMPARE(node(canvas, "gen").value("nodeHeight").toDouble(), 28.0);
        QCOMPARE(link(canvas, "gen.prompt").value("y2").toDouble(), 94.0);
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

    void reroutePointsAndCommentsAreDrawnFromTheLayout() {
        CanvasModel canvas(&m_registry);
        Document doc = document(threeNodes);
        QJsonObject layout = doc.layout;
        layout.insert("links", QJsonObject{{"save.image", QJsonObject{{"points", QJsonArray{QJsonArray{580, 400}}}}}});
        doc.layout = layout;
        canvas.setDocument(doc);
        QCOMPARE(canvas.reroutes().size(), 1);
        const QVariantMap point = canvas.reroutes().first().toMap();
        QCOMPARE(point.value("x").toDouble(), 580.0);
        QCOMPARE(point.value("color").toString(), QString("image"));
        QCOMPARE(canvas.comments().size(), 1);
        QCOMPARE(canvas.comments().first().toMap().value("text").toString(), QString("keep me"));
    }

    // #19 fills these from a run; with no run they are empty, so a node never run shows no state.
    void runStateIsEmptyUntilARunSetsIt() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(threeNodes));
        QVariantMap gen = node(canvas, "gen");
        QCOMPARE(gen.value("runState").toString(), QString());
        QCOMPARE(gen.value("progress").toDouble(), -1.0);
        QCOMPARE(gen.value("thumbnail").toString(), QString());
        const qreal plain = gen.value("nodeHeight").toDouble();

        QSignalSpy changed(canvas.nodes(), &QAbstractItemModel::dataChanged);
        canvas.setRunState("gen", {{"state", "cached"}, {"seconds", 6.2}, {"thumbnail", "file:///tmp/x.png"},
                                   {"thumbnailAspect", 0.5}, {"thumbnailInfo", "1024 × 512"},
                                   {"values", QVariantMap{{"seed", "4818231"}}}, {"warnings", QStringList{"slow"}}});
        QCOMPARE(changed.count(), 1);
        gen = node(canvas, "gen");
        QCOMPARE(gen.value("runState").toString(), QString("cached"));
        QCOMPARE(gen.value("seconds").toDouble(), 6.2);
        QCOMPARE(gen.value("warnings").toStringList(), QStringList{"slow"});
        QCOMPARE(gen.value("thumbnailInfo").toString(), QString("1024 × 512"));
        QCOMPARE(port(gen, "outputs", "seed").value("value").toString(), QString("4818231"));
        // The thumbnail sits under the rows, 6 px in from each side, its height from its aspect.
        QCOMPARE(gen.value("thumbnailRect").toRectF(), QRectF(6, plain - 8 + 4, 228, 114));
        QCOMPARE(gen.value("nodeHeight").toDouble(), plain + 4 + 114);

        // An edit of the node keeps it until the next run says otherwise.
        canvas.select({"gen"});
        canvas.toggleCollapsed();
        canvas.toggleCollapsed();
        QCOMPARE(node(canvas, "gen").value("runState").toString(), QString("cached"));
        canvas.clearRunState();
        QCOMPARE(node(canvas, "gen").value("runState").toString(), QString());
        QCOMPARE(node(canvas, "gen").value("nodeHeight").toDouble(), plain);
    }

    void layoutColorAndPinShowOnTheNode() {
        CanvasModel canvas(&m_registry);
        Document doc = document(threeNodes);
        QJsonObject layout = doc.layout;
        QJsonObject nodes = layout.value("nodes").toObject();
        nodes.insert("gen", QJsonObject{{"pos", QJsonArray{300, 80}}, {"color", "blue"}, {"pinned", true}});
        layout.insert("nodes", nodes);
        doc.layout = layout;
        canvas.setDocument(doc);
        QCOMPARE(node(canvas, "gen").value("color").toString(), QString("blue"));
        QVERIFY(node(canvas, "gen").value("pinned").toBool());
        QCOMPARE(node(canvas, "save").value("color").toString(), QString());
    }

    // Spec 3.5: a bypassed node passes each output the first input of the same type.
    void aBypassedNodeShowsWhatPassesThrough() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "crop": {"type": "fugue/crop", "version": "1.0.0", "inputs": {}, "mode": "bypass"}}}})"));
        const QVariantMap crop = node(canvas, "crop");
        QCOMPARE(crop.value("mode").toString(), QString("bypass"));
        QCOMPARE(port(crop, "outputs", "image").value("passFrom").toString(), QString("image"));
    }

    // Rows that do not apply are hidden (#429), on the real Pad and Sampler: heights, values kept, a link shows the row.
    void padHidesBlurAndFeatheringThatDoNotApply() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "pad": {"type": "fugue/pad", "version": "1.2.0", "inputs": {"fill": "gray", "blur": 7}},
            "keep": {"type": "fugue/solid-mask", "version": "1.0.0", "inputs": {}}}}})"));
        QVariantMap pad = node(canvas, "pad");
        QVERIFY(port(pad, "inputs", "blur").value("notUsed").toBool());
        QVERIFY(port(pad, "inputs", "blur").value("hidden").toBool());
        QCOMPARE(port(pad, "inputs", "blur").value("rowHeight").toDouble(), 0.0);
        QVERIFY(!port(pad, "inputs", "feathering").value("notUsed").toBool()); // no Keep mask: it applies
        const qreal height = pad.value("nodeHeight").toDouble();
        WidgetEdit *edits = canvas.widgets();
        // Fill edge-blur: Blur applies and, folded as an Advanced row, still takes no room until opened.
        edits->setValue("pad", "fill", "edge-blur");
        pad = node(canvas, "pad");
        QVERIFY(!port(pad, "inputs", "blur").value("notUsed").toBool());
        QCOMPARE(pad.value("nodeHeight").toDouble(), height); // Advanced is folded either way
        // The hidden value was kept and goes to the engine as it is.
        QCOMPARE(canvas.document().graph.nodes.value("pad").inputs.value("blur").literal().toInt(), 7);
        // A Keep mask hides Feathering; a link to a hidden row shows it.
        QVERIFY(canvas.connectPorts("keep", "mask", "pad", "mask"));
        QVERIFY(port(node(canvas, "pad"), "inputs", "feathering").value("notUsed").toBool());
        edits->setValue("pad", "fill", "gray");
        QVERIFY(port(node(canvas, "pad"), "inputs", "blur").value("notUsed").toBool());
    }

    void aSectionHeaderGoesWithTheRowsThatApply() {
        // Advanced open: the two hidden rows leave no gap and the header stays for the ones left.
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "pad": {"type": "fugue/pad", "version": "1.2.0", "inputs": {"fill": "gray"}}}},
            "layout": {"nodes": {"pad": {"advanced": true}}}})"));
        const QVariantMap open = node(canvas, "pad");
        QVERIFY(port(open, "inputs", "blur").value("notUsed").toBool());
        QCOMPARE(port(open, "inputs", "blur").value("rowHeight").toDouble(), 0.0);
        QVERIFY(port(open, "inputs", "target_width").value("rowHeight").toDouble() > 0);
        QVERIFY(port(open, "inputs", "feathering").value("rowHeight").toDouble() > 0);
        CanvasModel shown(&m_registry);
        shown.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "pad": {"type": "fugue/pad", "version": "1.2.0", "inputs": {"fill": "edge-blur"}}}},
            "layout": {"nodes": {"pad": {"advanced": true}}}})"));
        QCOMPARE(node(shown, "pad").value("nodeHeight").toDouble(),
                 open.value("nodeHeight").toDouble() + canvas.rowHeight());
    }

    void samplerHidesSoftMaskWithoutAMaskedLatentAndDenoiseModeAtFullDenoise() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "keep": {"type": "fugue/solid-mask", "version": "1.0.0", "inputs": {}},
            "enc": {"type": "fugue/vae-encode", "version": "1.1.0", "inputs": {}},
            "empty": {"type": "fugue/empty-latent", "version": "1.0.0", "inputs": {}},
            "s": {"type": "fugue/sampler", "version": "1.4.0", "inputs": {"latent": {"from": "empty.latent"}}}}},
            "layout": {"nodes": {"s": {"advanced": true}}}})"));
        const auto notUsed = [&](const QString &input) { return port(node(canvas, "s"), "inputs", input).value("notUsed").toBool(); };
        const auto height = [&] { return node(canvas, "s").value("nodeHeight").toDouble(); };
        QVERIFY(notUsed("soft_mask")); // an Empty Latent has no mask
        QVERIFY(notUsed("denoise_mode")); // Denoise is 1
        const qreal without = height();
        QVERIFY(canvas.connectPorts("enc", "latent", "s", "latent"));
        QVERIFY(notUsed("soft_mask")); // VAE Encode without a mask
        // Only the encoder is edited; the Sampler follows.
        QVERIFY(canvas.connectPorts("keep", "mask", "enc", "mask"));
        QVERIFY(!notUsed("soft_mask"));
        QCOMPARE(height(), without + canvas.rowHeight());
        QVERIFY(canvas.disconnectInput("enc", "mask"));
        QVERIFY(notUsed("soft_mask"));
        QCOMPARE(height(), without);
        canvas.widgets()->setValue("s", "denoise", 0.5);
        QVERIFY(!notUsed("denoise_mode"));
    }

    // A group's face reads the inner conditions under the exposed names; one it does not expose always shows.
    void aGroupFaceHidesWhatItsInnerConditionHides() {
        const auto face = [&](const char *inputs) {
            CanvasModel canvas(&m_registry);
            canvas.setDocument(document(QByteArray(R"({"fugue": 1, "graph": {"nodes": {
                "g": {"type": "local/p", "version": "1.0.0", "inputs": {"how": "gray"}}}},
                "groups": {"local/p": {"version": "1.0.0", "name": "P", "inputs": [)") + inputs + R"(],
                  "outputs": [], "nodes": {"pad": {"type": "fugue/pad", "version": "1.2.0", "inputs": {}}}}}})"));
            return node(canvas, "g");
        };
        const QVariantMap both = face(R"({"name": "how", "targets": ["pad.fill"]}, {"name": "soft", "targets": ["pad.blur"]})");
        QVERIFY(port(both, "inputs", "soft").value("notUsed").toBool());
        const QVariantMap alone = face(R"({"name": "soft", "targets": ["pad.blur"]})");
        QVERIFY(!port(alone, "inputs", "soft").value("notUsed").toBool());
    }
};

QTEST_MAIN(TestCanvasModel)
#include "test_canvas_model.moc"
