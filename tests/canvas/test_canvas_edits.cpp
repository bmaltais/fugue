// The canvas model's edits and their one undo stack (spec 12.4, 12.11): selection, delete,
// copy and paste, clone, bypass, mute, collapse, and undo and redo of each, checked against
// the document the model gives back (canvas_model.h).

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <QUndoStack>

#include "canvas/align.h"
#include "canvas/canvas_model.h"
#include "canvas/widget_edit.h"
#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/group_library.h"
#include "engine/pack_loader.h"
#include "engine/registry.h"
#include "workflow/content_hash.h"
#include "nodes/builtin.h"
#include "../cpu_time.h"

using namespace fugue;

namespace {

Document document(const char *json) {
    const Result<Document> loaded = Document::load(QByteArray(json));
    if (!loaded.ok())
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

// prompt (Text) -> gen (Generate Image) -> save (Save Image), prompt -> join (Text Concat),
// and a lone Text, all placed; save's link bends through a reroute point, and a Frame holds
// prompt and other.
const char *chain = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0",
            "inputs": {"prompt": {"from": "prompt.text"}, "steps": 8}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}, "name": "fox"}},
    "other": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "snow"}},
    "join": {"type": "fugue/text-concat", "version": "1.0.0", "inputs": {"a": {"from": "prompt.text"}}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [0, 100], "title": "Prompt"}, "gen": {"pos": [300, 80]},
                       "save": {"pos": [640, 120]}, "other": {"pos": [0, 400]}, "join": {"pos": [300, 500]}},
             "links": {"save.image": {"points": [[600, 200]]}},
             "frames": [{"id": "f1", "title": "Inputs", "rect": [0, 0, 300, 600], "nodes": ["prompt", "other"]}]}
})";

QByteArray bytes(const CanvasModel &canvas) { return canvas.document().save(); }

// The link into `to` ("node.input"), as the link list shows it.
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

// The names of a node's input rows on its face, in order.
QStringList inputNames(const CanvasModel &canvas, const QString &node) {
    QStringList names;
    for (int i = 0; i < canvas.nodes()->rowCount(); ++i) {
        const QModelIndex index = canvas.nodes()->index(i, 0);
        if (canvas.nodes()->data(index, NodeList::NodeId).toString() == node)
            for (const QVariant &port : canvas.nodes()->data(index, NodeList::Inputs).toList())
                names << port.toMap().value("name").toString();
    }
    return names;
}

QString from(const CanvasModel &canvas, const QString &node, const QString &input) {
    const InputValue value = canvas.document().graph.nodes.value(node).inputs.value(input);
    return value.kind() == InputValue::Kind::Link ? value.from().toString() : QString();
}

} // namespace

class TestCanvasEdits : public QObject {
    Q_OBJECT

    Registry m_registry;

private slots:
    void initTestCase() {
        nodes::addBuiltinNodeTypes(m_registry);
        // A LoRA to link from: no built-in node has a LoRA output.
        const Result<NodeType> pick = NodeType::fromJson(QJsonDocument::fromJson(R"({"manifest": 1, "id": "io.example.lora/pick",
            "version": "1.0.0", "name": "Pick LoRA", "runtime": {"kind": "builtin"}, "inputs": [],
            "outputs": [{"name": "lora", "type": "fugue/lora"}]})").object());
        QVERIFY2(pick.ok(), qPrintable(pick.ok() ? QString() : pick.error().message));
        QVERIFY(m_registry.add(pick.value()).isEmpty());
    }

    // #55: P pins the selection (layout.nodes.<id>.pinned), one undo step; a pinned node stays
    // where it is when a drag or its Frame moves the others.
    void aPinnedNodeStaysPut() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"gen", "prompt"});
        canvas.togglePinned();
        QVERIFY(canvas.document().layout.value("nodes").toObject().value("gen").toObject().value("pinned").toBool());
        QVERIFY(canvas.selectionAll("pinned"));
        canvas.select({"gen", "save"});
        canvas.beginMove("gen");
        canvas.moveBy(50, 0);
        canvas.endMove();
        QCOMPARE(canvas.position("gen"), QPointF(300, 80));
        QCOMPARE(canvas.position("save"), QPointF(690, 120));
        canvas.beginFrameMove("f1");
        canvas.moveBy(0, 40);
        canvas.endMove();
        QCOMPARE(canvas.position("prompt"), QPointF(0, 100));
        QCOMPARE(canvas.position("other"), QPointF(0, 440));
        // Still inside the moved Frame, the pinned node is still one of its nodes.
        const QJsonArray held = canvas.document().layout.value("frames").toArray().first().toObject().value("nodes").toArray();
        QVERIFY(held.contains(QJsonValue("prompt")));
        QVERIFY(held.contains(QJsonValue("other")));
        // Pinned again with all pinned: unpinned, and the pin is one step back.
        canvas.select({"gen", "prompt"});
        canvas.togglePinned();
        QVERIFY(!canvas.selectionAll("pinned"));
        canvas.undo();
        QVERIFY(canvas.selectionAll("pinned"));
    }

    // #58: align puts the selection on its outermost edge, distribute evens the gaps; each is one
    // undo step, a pinned node stays, and what would not move makes no step.
    void alignAndDistributeAreOneUndoStepEach() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"prompt", "gen", "save"});
        const int steps = canvas.undoStack()->count();
        const QRectF gen = canvas.nodeRect("gen");
        const QRectF save = canvas.nodeRect("save");
        QVERIFY(canvas.alignSelection("top"));
        QCOMPARE(canvas.position("prompt").y(), 80.0);
        QCOMPARE(canvas.position("gen").y(), 80.0);
        QCOMPARE(canvas.position("save").y(), 80.0);
        QCOMPARE(canvas.position("save").x(), 640.0); // the other axis is left alone
        QCOMPARE(canvas.undoStack()->count(), steps + 1);
        canvas.undo();
        QCOMPARE(canvas.position("save"), QPointF(640, 120));
        QCOMPARE(canvas.position("prompt"), QPointF(0, 100));

        QVERIFY(canvas.alignSelection("right"));
        QCOMPARE(canvas.nodeRect("prompt").right(), save.right());
        QCOMPARE(canvas.nodeRect("gen").right(), save.right());
        canvas.undo();
        QVERIFY(canvas.alignSelection("bottom"));
        QCOMPARE(canvas.nodeRect("prompt").bottom(), std::max(gen.bottom(), save.bottom()));
        canvas.undo();
        QVERIFY(canvas.alignSelection("left"));
        QCOMPARE(canvas.position("gen").x(), 0.0);
        QVERIFY(!canvas.alignSelection("left")); // already there: no step
        QCOMPARE(canvas.undoStack()->count(), steps + 1);
        canvas.undo();

        // Distribute: equal gaps along x between the first node's left and the last one's right.
        canvas.select({"prompt", "gen", "save"});
        QVERIFY(canvas.alignSelection("horizontal"));
        const QRectF a = canvas.nodeRect("prompt"), b = canvas.nodeRect("gen"), c = canvas.nodeRect("save");
        QCOMPARE(a.left(), 0.0);
        QCOMPARE(c.right(), save.right());
        QVERIFY(std::abs((b.left() - a.right()) - (c.left() - b.right())) <= 1);
        canvas.undo();
        QCOMPARE(canvas.position("gen"), QPointF(300, 80));

        // Two nodes have no gap to even out; a pinned node stays.
        canvas.select({"prompt", "gen"});
        QVERIFY(!canvas.alignSelection("vertical"));
        canvas.select({"prompt", "gen", "save"});
        canvas.select({"gen"});
        canvas.togglePinned();
        canvas.select({"prompt", "gen", "save"});
        QVERIFY(canvas.alignSelection("bottom"));
        QCOMPARE(canvas.position("gen"), QPointF(300, 80));
        QVERIFY(!canvas.alignSelection("sideways"));
    }

    // Distribute never leaves nodes overlapping: tall nodes in a short span get a minimum gap and the run
    // grows past the span (found in the real window, #58).
    void distributeKeepsAMinimumGapWhenTheNodesDoNotFitTheSpan() {
        const QList<fugue::align::Box> boxes{{"a", QRectF(0, 0, 100, 200), false},
                                             {"b", QRectF(0, 50, 100, 200), false},
                                             {"c", QRectF(0, 100, 100, 200), false}};
        const QHash<QString, QPointF> moves = fugue::align::distribute(boxes, fugue::align::Axis::Vertical);
        QCOMPARE(moves.value("b").y(), 224.0); // a ends at 200, plus the gap
        QCOMPARE(moves.value("c").y(), 448.0);
        QVERIFY(!moves.contains("a"));
    }

    // #55: a rename sets the title the canvas shows (layout.nodes.<id>.title), one undo step;
    // an empty one goes back to the type's name.
    void aRenameSetsTheTitle() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.renameNode("gen", "  Hero shot ");
        QCOMPARE(canvas.nodeTitle("gen"), QStringLiteral("Hero shot"));
        QCOMPARE(canvas.document().layout.value("nodes").toObject().value("gen").toObject().value("title").toString(),
                 QStringLiteral("Hero shot"));
        canvas.undo();
        QCOMPARE(canvas.nodeTitle("gen"), QStringLiteral("Generate Image"));
        canvas.renameNode("prompt", "");
        QVERIFY(!canvas.document().layout.value("nodes").toObject().value("prompt").toObject().contains("title"));
        QCOMPARE(canvas.nodeTitle("prompt"), QStringLiteral("Text"));
    }

    // #74: a wire dropped on a node's body: the ports it could end on there, free ones first, so
    // a drop connects to the first and asks only when several free ones fit.
    void aWireDroppedOnANodeFindsItsPorts() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.beginLinkDrag("other", "text", true);
        QStringList names;
        for (const QVariant &target : canvas.dropTargets("gen"))
            names << target.toMap().value("port").toString() + (target.toMap().value("free").toBool() ? "" : "*");
        // prompt is linked already (prompt.text); negative_prompt is free.
        QCOMPARE(names, (QStringList{"negative_prompt", "prompt*"}));
        for (const QVariant &target : canvas.dropTargets("save")) // its text inputs, never its image
            QVERIFY(target.toMap().value("port").toString() != QLatin1String("image"));
        QVERIFY(canvas.dropTargets("other").isEmpty()); // not the node the wire comes from
        canvas.endLinkDrag();
        // From an input: the outputs that can feed it.
        canvas.beginLinkDrag("save", "image", false); // linked: the link is picked up, from gen.image
        canvas.endLinkDrag();
        canvas.beginLinkDrag("join", "b", false);
        names.clear();
        for (const QVariant &target : canvas.dropTargets("other"))
            names << target.toMap().value("port").toString();
        QCOMPARE(names, QStringList{"text"});
        canvas.endLinkDrag();
    }

    void aConnectIsUndoneAndRedone() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        const QByteArray before = bytes(canvas);
        QVERIFY(!canvas.canUndo());
        QVERIFY(canvas.connectPorts("other", "text", "gen", "prompt"));
        const QByteArray after = bytes(canvas);
        QVERIFY(canvas.canUndo());
        canvas.undo();
        QCOMPARE(bytes(canvas), before);
        QCOMPARE(from(canvas, "gen", "prompt"), QString("prompt.text"));
        QCOMPARE(canvas.links()->rowCount(), 3);
        QVERIFY(canvas.canRedo());
        canvas.redo();
        QCOMPARE(bytes(canvas), after);
        QCOMPARE(from(canvas, "gen", "prompt"), QString("other.text"));
    }

    void aLinkOverALiteralComesBackAsTheLiteral() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        const QByteArray before = bytes(canvas);
        // name holds the literal "fox"; a link replaces it.
        QVERIFY(canvas.connectPorts("prompt", "text", "save", "name"));
        canvas.undo();
        QCOMPARE(bytes(canvas), before);
    }

    void aDragIsOneUndoStep() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        const QByteArray before = bytes(canvas);
        canvas.beginMove("gen");
        for (int i = 1; i <= 5; ++i)
            canvas.moveBy(10 * i, 4 * i);
        canvas.endMove();
        QCOMPARE(canvas.position("gen"), QPointF(350, 100));
        QCOMPARE(canvas.undoStack()->count(), 1);
        canvas.undo();
        QCOMPARE(bytes(canvas), before);
        QCOMPARE(canvas.position("gen"), QPointF(300, 80));
        // The wires follow the node back.
        QCOMPARE(link(canvas, "save.image").value("x1").toDouble(), 300 + canvas.defaultWidth());
        canvas.redo();
        QCOMPARE(canvas.position("gen"), QPointF(350, 100));
        // A drag that ends where it began is no step at all.
        canvas.beginMove("gen");
        canvas.moveBy(30, 0);
        canvas.moveBy(0, 0);
        canvas.endMove();
        QCOMPARE(canvas.undoStack()->count(), 1);
    }

    void aPickedUpLinkPutDownElsewhereIsOneStep() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        const QByteArray before = bytes(canvas);
        canvas.beginLinkDrag("join", "a", false); // picks prompt.text up
        QVERIFY(canvas.endLinkDrag("join", "b", false));
        QCOMPARE(from(canvas, "join", "b"), QString("prompt.text"));
        QCOMPARE(from(canvas, "join", "a"), QString());
        QCOMPARE(canvas.undoStack()->count(), 1);
        canvas.undo();
        QCOMPARE(bytes(canvas), before);
        // Put back where it was: nothing to undo.
        canvas.beginLinkDrag("join", "a", false);
        QVERIFY(canvas.endLinkDrag("join", "a", false));
        QCOMPARE(bytes(canvas), before);
        QVERIFY(!canvas.canUndo());
        QVERIFY(canvas.canRedo());
    }

    void deleteTakesNodesLinksAndLayoutAndUndoBringsThemBack() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        const QByteArray before = bytes(canvas);
        QStringList order;
        for (int i = 0; i < canvas.nodes()->rowCount(); ++i)
            order << canvas.nodes()->data(canvas.nodes()->index(i, 0), NodeList::NodeId).toString();
        canvas.select({"prompt", "gen"});
        canvas.deleteSelection();
        const Document after = canvas.document();
        QCOMPARE(after.graph.nodes.keys(), (QStringList{"join", "other", "save"}));
        // No link is left pointing at a node that is gone.
        QVERIFY(!after.graph.nodes.value("save").inputs.contains("image"));
        QVERIFY(!after.graph.nodes.value("join").inputs.contains("a"));
        QCOMPARE(canvas.links()->rowCount(), 0);
        QVERIFY(!after.layout.value("nodes").toObject().contains("prompt"));
        QCOMPARE(after.layout.value("frames").toArray()[0].toObject().value("nodes").toArray(), QJsonArray{"other"});
        QVERIFY(canvas.selectedIds().isEmpty());

        canvas.undo();
        QCOMPARE(bytes(canvas), before);
        QCOMPARE(canvas.links()->rowCount(), 3);
        QStringList restored;
        for (int i = 0; i < canvas.nodes()->rowCount(); ++i)
            restored << canvas.nodes()->data(canvas.nodes()->index(i, 0), NodeList::NodeId).toString();
        QCOMPARE(restored, order); // in their places in the drawing order
        QCOMPARE(canvas.selectedIds(), (QStringList{"gen", "prompt"}));
        // The title from the layout came back with the node.
        QCOMPARE(canvas.nodes()->data(canvas.nodes()->index(order.indexOf("prompt"), 0), NodeList::Title).toString(),
                 QString("Prompt"));
    }

    void copyAndPasteKeepLinksBetweenTheCopiedNodes() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"prompt", "gen"});
        const QString copied = canvas.copySelection();
        QVERIFY(Document::load(copied.toUtf8()).ok()); // a workflow of its own
        canvas.setPointer(1000, 1000);
        const QStringList pasted = canvas.paste(copied);
        QCOMPARE(pasted.size(), 2);
        QCOMPARE(canvas.selectedIds().size(), 2);
        const Document after = canvas.document();
        QCOMPARE(after.graph.nodes.size(), 7);
        // The copy's top left is at the pointer, the nodes as far apart as before.
        const QString text = after.graph.nodes.value(pasted[0]).type == "fugue/text" ? pasted[0] : pasted[1];
        const QString gen = text == pasted[0] ? pasted[1] : pasted[0];
        QCOMPARE(canvas.position(text), QPointF(1000, 1020));
        QCOMPARE(canvas.position(gen), QPointF(1300, 1000));
        QCOMPARE(from(canvas, gen, "prompt"), text + ".text");
        QCOMPARE(after.graph.nodes.value(gen).inputs.value("steps").literal().toInt(), 8);
        QCOMPARE(canvas.undoStack()->count(), 1);
        canvas.undo();
        QCOMPARE(canvas.document().graph.nodes.size(), 5);
    }

    void aPasteWithConnectionsKeepsLinksFromOutside() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"gen", "join"});
        const QString copied = canvas.copySelection();
        const QStringList plain = canvas.paste(copied);
        for (const QString &id : plain)
            QVERIFY(!canvas.document().graph.nodes.value(id).inputs.contains(
                canvas.document().graph.nodes.value(id).type == "fugue/text-concat" ? "a" : "prompt"));
        const QStringList connected = canvas.paste(copied, true);
        for (const QString &id : connected) {
            const bool join = canvas.document().graph.nodes.value(id).type == "fugue/text-concat";
            QCOMPARE(from(canvas, id, join ? "a" : "prompt"), QString("prompt.text"));
        }
        QCOMPARE(canvas.links()->rowCount(), 3 + 2);
    }

    void aPastedLinkThatCannotFeedItsInputIsDropped() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        // A hand-written copy whose outside link comes from an image output into a text input.
        const QStringList ids = canvas.paste(R"({"fugue": 1, "graph": {"nodes": {
            "j": {"type": "fugue/text-concat", "version": "1.0.0",
                  "inputs": {"a": {"from": "gen.image"}, "b": {"from": "prompt.text"}}}}}})",
                                             true);
        QCOMPARE(ids.size(), 1);
        QCOMPARE(from(canvas, ids[0], "a"), QString());
        QCOMPARE(from(canvas, ids[0], "b"), QString("prompt.text"));
        QCOMPARE(canvas.undoStack()->count(), 1);
    }

    // A pasted group instance keeps the definition it was copied with: when this workflow
    // holds another definition under the same id, the pasted one comes in under a new id.
    void aPasteKeepsItsOwnGroupDefinition() {
        const char *mine = R"({"fugue": 1, "graph": {"nodes": {
            "g": {"type": "local/pass", "version": "1.0.0", "inputs": {}}}},
          "groups": {"local/pass": {"version": "1.0.0", "name": "Pass",
            "inputs": [{"name": "x", "targets": ["first.prompt"]}],
            "outputs": [{"name": "image", "from": "first.image"}],
            "nodes": {"first": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {}}}}}})";
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(mine));
        // The same definition: the instance shares it.
        canvas.select({"g"});
        const QStringList same = canvas.paste(canvas.copySelection());
        QCOMPARE(canvas.document().graph.nodes.value(same.value(0)).type, QString("local/pass"));
        QCOMPARE(canvas.document().groups.size(), 1);

        const QStringList other = canvas.paste(R"({"fugue": 1, "graph": {"nodes": {
            "p": {"type": "local/pass", "version": "1.0.0", "inputs": {}}}},
          "groups": {"local/pass": {"version": "1.0.0", "name": "Pass",
            "inputs": [{"name": "y", "targets": ["first.steps"]}],
            "outputs": [{"name": "image", "from": "first.image"}],
            "nodes": {"first": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {}}}}}})");
        QCOMPARE(other.size(), 1);
        const Document after = canvas.document();
        const QString type = after.graph.nodes.value(other[0]).type;
        QVERIFY(type != "local/pass");
        QCOMPARE(after.groups.value(type).inputs.value(0).name, QString("y"));
        QCOMPARE(after.groups.value("local/pass").inputs.value(0).name, QString("x"));
        QCOMPARE(after.groups.size(), 2);
    }

    // A copy takes the reroute points of the links between its nodes, moved with them.
    void aCopyKeepsTheReroutePointsOfItsLinks() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"gen", "save"});
        canvas.setPointer(1300, 1080); // gen's place, 1000 down and right
        const QStringList pasted = canvas.paste(canvas.copySelection());
        const QString save = canvas.document().graph.nodes.value(pasted[0]).type == "fugue/save-image" ? pasted[0] : pasted[1];
        const QJsonArray points = canvas.document().layout.value("links").toObject().value(save + ".image")
                                      .toObject().value("points").toArray();
        QCOMPARE(points, (QJsonArray{QJsonArray{1600, 1200}}));
    }

    void pastingTextThatIsNotAWorkflowDoesNothing() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        QVERIFY(canvas.paste("a red fox").isEmpty());
        QVERIFY(canvas.paste("{}").isEmpty());
        QVERIFY(!canvas.canUndo());
    }

    void cloneCopiesTheSelectionBesideIt() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"prompt", "gen"});
        const QStringList clones = canvas.cloneSelection();
        QCOMPARE(clones.size(), 2);
        QCOMPARE(canvas.selectedIds().size(), 2);
        QVERIFY(!canvas.selectedIds().contains("prompt"));
        QStringList positions;
        for (const QString &id : clones)
            positions << QStringLiteral("%1,%2").arg(canvas.position(id).x()).arg(canvas.position(id).y());
        positions.sort();
        QCOMPARE(positions, (QStringList{"20,120", "320,100"}));
    }

    void anAltDragCloneAndItsMoveAreOneStep() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        const QByteArray before = bytes(canvas);
        canvas.beginCloneMove("other");
        QCOMPARE(canvas.selectedIds().size(), 1);
        const QString clone = canvas.selectedIds().first();
        QVERIFY(clone != "other");
        canvas.moveBy(0, 200);
        canvas.endMove();
        QCOMPARE(canvas.position(clone), QPointF(0, 600));
        QCOMPARE(canvas.position("other"), QPointF(0, 400));
        QCOMPARE(canvas.undoStack()->count(), 1);
        canvas.undo();
        QCOMPARE(bytes(canvas), before);
    }

    void bypassAndMuteGoToEveryNodeUnlessAllHaveThem() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        auto mode = [&](const QString &id) { return nodeModeName(canvas.document().graph.nodes.value(id).mode); };
        canvas.select({"gen"});
        canvas.toggleMode("mute");
        canvas.select({"gen", "save"});
        canvas.toggleMode("bypass");
        QCOMPARE(mode("gen"), QString("bypass"));
        QCOMPARE(mode("save"), QString("bypass"));
        QCOMPARE(canvas.nodes()->data(canvas.nodes()->index(1, 0), NodeList::Mode).isValid(), true);
        canvas.toggleMode("bypass");
        QCOMPARE(mode("gen"), QString("normal"));
        QCOMPARE(mode("save"), QString("normal"));
        canvas.undo();
        canvas.undo();
        QCOMPARE(mode("gen"), QString("mute"));
        QCOMPARE(mode("save"), QString("normal"));
        QVERIFY(canvas.document().save().contains("\"mode\": \"mute\""));
    }

    void collapseFoldsTheSelectionAndItsWiresMeetTheTitle() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        const QByteArray before = bytes(canvas);
        canvas.select({"gen"});
        canvas.toggleCollapsed();
        QVERIFY(canvas.document().layout.value("nodes").toObject().value("gen").toObject().value("collapsed").toBool());
        QCOMPARE(link(canvas, "save.image").value("y1").toDouble(), 80 + canvas.headerHeight() / 2);
        // Folded, the header fits its title, and the wire leaves from its right end.
        QModelIndex gen;
        for (int i = 0; i < canvas.nodes()->rowCount(); ++i)
            if (canvas.nodes()->data(canvas.nodes()->index(i, 0), NodeList::NodeId).toString() == "gen")
                gen = canvas.nodes()->index(i, 0);
        const qreal folded = canvas.nodes()->data(gen, NodeList::Width).toDouble();
        QVERIFY2(folded > 40 && folded < 60 * canvas.textSize() / 3.0, qPrintable(QString::number(folded)));
        QCOMPARE(link(canvas, "save.image").value("x1").toDouble(), 300 + folded);
        canvas.toggleCollapsed();
        QCOMPARE(bytes(canvas), before); // expanded again, the key is gone
        canvas.undo();
        canvas.undo();
        QCOMPARE(bytes(canvas), before);
    }

    void ctrlAltClickOnAPortTakesItsLinks() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        QVERIFY(canvas.disconnectPort("prompt", "text", true));
        QCOMPARE(from(canvas, "gen", "prompt"), QString());
        QCOMPARE(from(canvas, "join", "a"), QString());
        QCOMPARE(canvas.links()->rowCount(), 1);
        QVERIFY(canvas.disconnectPort("save", "image", false));
        QCOMPARE(canvas.links()->rowCount(), 0);
        QVERIFY(!canvas.disconnectPort("save", "image", false));
        QCOMPARE(canvas.undoStack()->count(), 2);
    }

    void shiftDragMovesEveryLinkOfAnOutput() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.beginLinksMove("prompt", "text");
        QCOMPARE(canvas.linkDrag().value("output").toBool(), false);
        // gen's image output cannot feed a text input: it says why, and takes nothing.
        QVERIFY(!canvas.dragProblem("gen", "image", true).isEmpty());
        QVERIFY(canvas.dragProblem("other", "text", true).isEmpty());
        QVERIFY(!canvas.endLinkDrag("gen", "image", true));
        QCOMPARE(from(canvas, "gen", "prompt"), QString("prompt.text"));
        QVERIFY(!canvas.canUndo());

        canvas.beginLinksMove("prompt", "text");
        QVERIFY(canvas.endLinkDrag("other", "text", true));
        QCOMPARE(from(canvas, "gen", "prompt"), QString("other.text"));
        QCOMPARE(from(canvas, "join", "a"), QString("other.text"));
        QCOMPARE(canvas.undoStack()->count(), 1);
        canvas.undo();
        QCOMPARE(from(canvas, "join", "a"), QString("prompt.text"));
    }

    void addNodeIsUndone() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        const QString id = canvas.addNode("fugue/number", 10, 20);
        QCOMPARE(canvas.nodes()->rowCount(), 6);
        canvas.undo();
        QCOMPARE(canvas.nodes()->rowCount(), 5);
        canvas.redo();
        QCOMPARE(canvas.position(id), QPointF(10, 20));
    }

    void selectAllSelectsEveryNode() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.selectAll();
        QCOMPARE(canvas.selectedIds().size(), 5);
    }

    void anUndoneEditBringsTheContentBack() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        const QString saved = contentHash(canvas.document());
        QVERIFY(canvas.connectPorts("other", "text", "join", "b"));
        QVERIFY(contentHash(canvas.document()) != saved);
        canvas.undo();
        QCOMPARE(contentHash(canvas.document()), saved);
        // The viewport is not content, and no edit.
        canvas.setView(10, 20, 2);
        QCOMPARE(contentHash(canvas.document()), saved);
        QCOMPARE(canvas.undoStack()->count(), 1);
    }

    void theUndoStackKeepsTheLastFiftySteps() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        for (int i = 1; i <= 60; ++i) {
            canvas.beginMove("gen");
            canvas.moveBy(i, 0);
            canvas.endMove();
        }
        QCOMPARE(canvas.undoStack()->count(), 50);
        while (canvas.canUndo())
            canvas.undo();
        QCOMPARE(canvas.position("gen"), QPointF(355, 80)); // after the tenth move: 300 + 1 + 2 + ... + 10
        QCOMPARE(canvas.takeUndoStack()->undoLimit(), 50);
        QCOMPARE(canvas.undoStack()->undoLimit(), 50);
    }

    void anUndoStackCanBeHandedOverAndBack() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"gen"});
        canvas.deleteSelection();
        const Document edited = canvas.document();
        std::unique_ptr<QUndoStack> kept = canvas.takeUndoStack();
        QVERIFY(!canvas.canUndo());
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        canvas.addNode("fugue/number", 0, 0);
        canvas.setDocument(edited);
        canvas.setUndoStack(std::move(kept));
        QVERIFY(canvas.canUndo());
        canvas.undo();
        QVERIFY(canvas.document().graph.nodes.contains("gen"));
    }

    void editedFiresForEveryChangeUndoIncluded() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        QSignalSpy edited(&canvas, &CanvasModel::edited);
        canvas.select({"gen"});
        QCOMPARE(edited.count(), 0);
        canvas.deleteSelection();
        canvas.undo();
        canvas.redo();
        QCOMPARE(edited.count(), 3);
    }

    void undoFinishesAGestureFirst() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.beginMove("gen");
        canvas.moveBy(100, 0);
        canvas.undo(); // the drag ends, then is undone
        QCOMPARE(canvas.position("gen"), QPointF(300, 80));
        canvas.moveBy(50, 0); // the rest of the drag moves nothing
        canvas.endMove();
        QCOMPARE(canvas.position("gen"), QPointF(300, 80));
        QVERIFY(canvas.canRedo());
    }

    // Reroute points (layout.links) belong to a link: they go with it, never to the next
    // link into the same input, and a list item's points follow it when items before it go.
    void reroutePointsGoWithTheirLink() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        auto points = [&] { return canvas.document().layout.value("links").toObject(); };
        QVERIFY(canvas.disconnectInput("save", "image"));
        QVERIFY(!points().contains("save.image"));
        QVERIFY(canvas.connectPorts("gen", "image", "save", "image"));
        QVERIFY(canvas.reroutes().isEmpty());
        canvas.undo();
        canvas.undo();
        QVERIFY(points().contains("save.image")); // undo brings them back
        // The source going takes them too.
        canvas.select({"gen"});
        canvas.deleteSelection();
        QVERIFY(!points().contains("save.image"));
        canvas.undo();
        QVERIFY(canvas.disconnectPort("gen", "image", true));
        QVERIFY(!points().contains("save.image"));

        Document listed = document(R"({"fugue": 1, "graph": {"nodes": {
            "a": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {}},
            "b": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {}},
            "edit": {"type": "fugue/edit-image", "version": "1.0.0",
                     "inputs": {"images": [{"from": "a.image"}, {"from": "b.image"}]}}}},
          "layout": {"nodes": {"a": {"pos": [0, 0]}, "b": {"pos": [0, 300]}, "edit": {"pos": [400, 0]}}}})");
        // The second item's points (built here: moc misreads a '#' inside a raw string).
        listed.layout.insert("links", QJsonObject{{QStringLiteral("edit.images") + QChar('#') + "1",
                                                   QJsonObject{{"points", QJsonArray{QJsonArray{300, 400}}}}}});
        canvas.setDocument(listed);
        QVERIFY(canvas.disconnectInput("edit", "images", 0));
        QCOMPARE(points().keys(), QStringList{QStringLiteral("edit.images") + QChar('#') + "0"});
        QCOMPARE(canvas.reroutes().size(), 1);
    }

    // A gesture's undo step holds the gesture alone: an edit made while it is open (a key,
    // a run's seeds) ends it first and is a step of its own.
    void anEditDuringAGestureIsAStepOfItsOwn() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"other"});
        canvas.beginMove("gen");
        canvas.moveBy(100, 0);
        canvas.toggleMode("bypass");
        canvas.moveBy(150, 0); // the move is over
        canvas.endMove();
        QCOMPARE(canvas.undoStack()->count(), 2);
        QCOMPARE(canvas.undoText(), QString("Bypass"));
        canvas.undo();
        QCOMPARE(canvas.position("gen"), QPointF(400, 80));

        canvas.beginLinkDrag("other", "text", true);
        canvas.widgets()->advanceSeeds();
        QVERIFY(canvas.linkDrag().isEmpty());
        canvas.beginResize("gen");
        canvas.resizeBy(40, 0);
        canvas.deleteSelection(); // other
        QCOMPARE(canvas.undoText(), QString("Delete"));
        canvas.undo();
        QCOMPARE(canvas.undoText(), QString("Resize"));
    }

    // A new workflow that gets a node and loses it again is as it was: not unsaved.
    void deletingEveryNodeLeavesNoEmptyLayout() {
        CanvasModel canvas(&m_registry);
        const Document empty = document(R"({"fugue": 1, "graph": {"nodes": {}}})");
        canvas.setDocument(empty);
        const QString id = canvas.addNode("fugue/text", 0, 0);
        canvas.select({id});
        canvas.deleteSelection();
        QCOMPARE(contentHash(canvas.document()), contentHash(empty));
        QVERIFY(!canvas.document().layout.contains("nodes"));
    }

    // At 300 nodes (spec 12.1) whole-graph edits change each list once, not once per row,
    // an edit that moves no reroute point leaves them alone, and menu checks stay cheap.
    void largeEditsChangeEachListOnce() {
        QJsonObject nodes, positions;
        nodes.insert("src", QJsonObject{{"type", "fugue/load-image"}, {"version", "1.0.0"}});
        for (int i = 0; i < 300; ++i) {
            const QString id = QStringLiteral("n%1").arg(i);
            const QString base = i > 0 ? QStringLiteral("n%1").arg(i - 1) : QStringLiteral("src");
            nodes.insert(id, QJsonObject{{"type", "fugue/blend"}, {"version", "1.0.0"},
                                         {"inputs", QJsonObject{{"base", QJsonObject{{"from", base + ".image"}}},
                                                                {"top", QJsonObject{{"from", "src.image"}}}}}});
            positions.insert(id, QJsonObject{{"pos", QJsonArray{(i % 20) * 260, (i / 20) * 160}}});
        }
        const Result<Document> made = Document::fromJson(QJsonObject{
            {"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}, {"layout", QJsonObject{{"nodes", positions}}}});
        QVERIFY(made.ok());
        CanvasModel canvas(&m_registry);
        canvas.setDocument(made.value());
        QCOMPARE(canvas.links()->rowCount(), 600);

        QSignalSpy reroutes(&canvas, &CanvasModel::reroutesChanged);
        canvas.select({"n5"});
        canvas.toggleMode("bypass");
        QCOMPARE(reroutes.count(), 0);

        canvas.selectAll();
        qint64 start = test_support::cpuMs();
        for (int i = 0; i < 10; ++i)
            canvas.selectionAll("bypass");
        const qint64 checks = test_support::cpuMs() - start;

        QSignalSpy nodeRemovals(canvas.nodes(), &QAbstractItemModel::rowsRemoved);
        QSignalSpy linkRemovals(canvas.links(), &QAbstractItemModel::rowsRemoved);
        start = test_support::cpuMs();
        canvas.deleteSelection();
        const qint64 deleted = test_support::cpuMs() - start;
        QCOMPARE(canvas.nodes()->rowCount(), 0);
        QCOMPARE(nodeRemovals.count(), 1);
        QCOMPARE(linkRemovals.count(), 1);
        qInfo("301 nodes, CPU time: 10 menu checks %lld ms, delete all %lld ms", checks, deleted);
        QVERIFY2(checks < 50, qPrintable(QString::number(checks)));
    }

    void fitFramesTheSelectionOrEverything() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        QVERIFY(canvas.fitView(1000, 1000));
        const QVariantMap all = canvas.view();
        canvas.select({"other"});
        QVERIFY(canvas.fitView(1000, 1000));
        const QVariantMap one = canvas.view();
        QVERIFY(one.value("zoom").toDouble() > all.value("zoom").toDouble());
        QCOMPARE(one.value("center").toList().value(0).toDouble(), canvas.defaultWidth() / 2);
        // Its 240 world units of width (60u) fill three quarters of the 1000 pixels.
        QCOMPARE(one.value("zoom").toDouble(), 750.0 / 240);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(!canvas.fitView(1000, 1000));
    }

    // #714: Apply LoRAs shows the rows it has and one empty row of a LoRA alone. Picking a LoRA in the last row makes the
    // next; setting one to None, or taking its link away, takes its row out with its strength; each is one undo step, and
    // the other rows keep their numbers.
    void repeatingRowsGrowAndShrinkAsOneStepEach() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "pick": {"type": "local/missing", "version": "1.0.0", "inputs": {}},
            "apply": {"type": "fugue/apply-loras", "version": "1.0.0",
                      "inputs": {"lora_1": {"from": "pick.lora"}, "strength_1": 0.7}}}},
          "layout": {"nodes": {"pick": {"pos": [0, 0]}, "apply": {"pos": [300, 0]}}}})"));
        QCOMPARE(inputNames(canvas, "apply"), (QStringList{"model", "lora_1", "strength_1", "enabled_1", "lora_2"}));
        const QVariantMap empty = canvas.inputPort("apply", "lora_2");
        QCOMPARE(empty.value("label").toString(), QStringLiteral("Add LoRA"));
        QVERIFY(empty.value("grows").toBool());
        QCOMPARE(canvas.inputPort("apply", "lora_1").value("label").toString(), QStringLiteral("LoRA 1"));
        const QVariantMap lora{{"resource", "fugue/lora"}, {"name", "detail.safetensors"}};
        QCOMPARE(canvas.widgets()->setModel("apply", "lora_2", lora), QString());
        QCOMPARE(inputNames(canvas, "apply"),
                 (QStringList{"model", "lora_1", "strength_1", "enabled_1", "lora_2", "strength_2", "enabled_2", "lora_3"}));
        QCOMPARE(canvas.undoStack()->count(), 1);
        // A row turned off is drawn faded, its switch not.
        QCOMPARE(canvas.widgets()->setValue("apply", "enabled_2", false), QString());
        QVERIFY(canvas.inputPort("apply", "strength_2").value("rowOff").toBool());
        QVERIFY(!canvas.inputPort("apply", "enabled_2").value("rowOff").toBool());
        canvas.undo();
        QVERIFY(!canvas.inputPort("apply", "strength_2").value("rowOff").toBool());

        QVERIFY(canvas.disconnectInput("apply", "lora_1"));
        QCOMPARE(inputNames(canvas, "apply"), (QStringList{"model", "lora_2", "strength_2", "enabled_2", "lora_3"}));
        QVERIFY(!canvas.document().graph.nodes.value("apply").inputs.contains("strength_1")); // its values went with it
        QCOMPARE(canvas.undoStack()->count(), 2);
        canvas.undo();
        QCOMPARE(from(canvas, "apply", "lora_1"), QStringLiteral("pick.lora"));
        QCOMPARE(canvas.document().graph.nodes.value("apply").inputs.value("strength_1").literal(), QJsonValue(0.7));

        canvas.widgets()->unset("apply", "lora_2");
        QCOMPARE(inputNames(canvas, "apply"), (QStringList{"model", "lora_1", "strength_1", "enabled_1", "lora_2"}));
        QCOMPARE(canvas.undoStack()->count(), 2); // the redo of the disconnect is gone, the clearing is a step of its own
        canvas.undo();
        QCOMPARE(inputNames(canvas, "apply").size(), 8);
    }

    // A row's link picked up and put down on the empty "Add LoRA" row moves the row: its strength and On go with the LoRA, in one
    // undo step. Put down nowhere, the row goes with its values (6.1.2).
    void movingARowsLinkToTheEmptyRowTakesItsValues() {
        CanvasModel canvas(&m_registry);
        const Document start = document(R"({"fugue": 1, "graph": {"nodes": {
            "pick": {"type": "io.example.lora/pick", "version": "1.0.0", "inputs": {}},
            "apply": {"type": "fugue/apply-loras", "version": "1.0.0",
                      "inputs": {"lora_1": {"from": "pick.lora"}, "strength_1": 0.3, "enabled_1": false,
                                 "lora_3": {"resource": "fugue/lora", "name": "ink.safetensors"}, "strength_3": 0.8}}}},
          "layout": {"nodes": {"pick": {"pos": [0, 0]}, "apply": {"pos": [300, 0]}}}})");
        canvas.setDocument(start);
        QCOMPARE(inputNames(canvas, "apply"),
                 (QStringList{"model", "lora_1", "strength_1", "enabled_1", "lora_3", "strength_3", "enabled_3", "lora_4"}));
        canvas.beginLinkDrag("apply", "lora_1", false);
        QVERIFY(canvas.endLinkDrag("apply", "lora_4", false));
        const Node moved = canvas.document().graph.nodes.value("apply");
        QCOMPARE(from(canvas, "apply", "lora_4"), QStringLiteral("pick.lora"));
        QCOMPARE(moved.inputs.value("strength_4").literal(), QJsonValue(0.3));
        QCOMPARE(moved.inputs.value("enabled_4").literal(), QJsonValue(false));
        QVERIFY(!moved.inputs.contains("strength_1") && !moved.inputs.contains("lora_1"));
        QCOMPARE(moved.inputs.value("strength_3").literal(), QJsonValue(0.8)); // the row it did not touch keeps its own
        QCOMPARE(canvas.undoStack()->count(), 1);
        canvas.undo();
        QCOMPARE(canvas.document().graph.nodes.value("apply").inputs, start.graph.nodes.value("apply").inputs);

        // Let go on nothing: the link and its row are gone, as when the link is taken away.
        canvas.beginLinkDrag("apply", "lora_1", false);
        QVERIFY(!canvas.endLinkDrag());
        QVERIFY(!canvas.document().graph.nodes.value("apply").inputs.contains("strength_1"));
        // A later link into an empty key does not bring the old row's values back.
        QVERIFY(canvas.connectPorts("pick", "lora", "apply", "lora_4"));
        QVERIFY(!canvas.document().graph.nodes.value("apply").inputs.contains("strength_4"));
    }

    // The document a run from `from` starts with: the seeds to hold, held in it (what CanvasRuns does for each run of a batch).
    static Document heldRun(const CanvasModel &canvas, const QString &from, const WidgetEdit::SeedUsed &seedUsed, QSet<QString> *held) {
        return canvas.widgets()->withHeldSeeds(canvas.widgets()->seedsToHold(from, seedUsed), held);
    }

    // Run from here holds what is upstream in the flattened graph (3.4): the Randomize seeds inside a group instance before the
    // node, and, inside a group, the nodes outside that feed it; never the end point's own, nor those inside a group card that
    // is the end point. A seed written in a definition with two instances is held for that instance only (runFromHereHoldsSeedsOfGroupsThatRunTwice): holding
    // it in the definition would hold it in the other copy too.
    void runFromHereHoldsSeedsThroughGroups() {
        const char *pass = R"({"fugue": 1, "graph": {"nodes": {
            "pre": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "a", "seed": {"auto": "random"}}},
            "g7": {"type": "local/pass", "version": "1.0.0", "inputs": {"image": {"from": "pre.image"}}},
            "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "g7.image"}}}%1}},
          "groups": {"local/pass": {"version": "1.0.0", "name": "Pass",
            "inputs": [{"name": "image", "targets": ["first.image"]}], "outputs": [{"name": "image", "from": "second.image"}],
            "nodes": {"first": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "b", "seed": {"auto": "random"}}},
                      "second": {"type": "fugue/generate-image", "version": "1.0.0",
                                 "inputs": {"prompt": "c", "seed": 9, "image": {"from": "first.image"}}}}}}})";
        const QHash<QString, double> shown{{"pre/seed", 1}, {"g7/first/seed", 2}, {"g7/second/seed", 9}, {"g8/first/seed", 3}};
        const WidgetEdit::SeedUsed seedUsed = [&shown](const QString &path, const QString &input) -> std::optional<double> {
            const auto it = shown.constFind(path + "/" + input);
            return it == shown.constEnd() ? std::nullopt : std::optional<double>(*it);
        };
        const auto seed = [](const Document &doc, const QString &group, const QString &node) {
            const Node &n = group.isEmpty() ? doc.graph.nodes.value(node) : doc.group(group)->nodes.value(node);
            return n.inputs.value("seed").toJson();
        };
        const QJsonValue random = QJsonObject{{"auto", "random"}};

        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(QString::fromUtf8(pass).arg("").toUtf8().constData()));
        QSet<QString> held;
        Document run = heldRun(canvas, "save", seedUsed, &held);
        QCOMPARE(seed(run, {}, "pre"), QJsonValue(1.0));
        QCOMPARE(seed(run, "local/pass", "first"), QJsonValue(2.0)); // inside the instance before the node
        QCOMPARE(seed(run, "local/pass", "second"), QJsonValue(9.0));
        QVERIFY(held.isEmpty()); // no increment seeds
        QCOMPARE(seed(canvas.document(), "local/pass", "first"), random); // the canvas's own document is as it was
        // The group's card as the end point: everything inside runs as in any run.
        run = heldRun(canvas, "g7", seedUsed, &held);
        QCOMPARE(seed(run, {}, "pre"), QJsonValue(1.0));
        QCOMPARE(seed(run, "local/pass", "first"), random);
        // Inside the group: what feeds it from outside is upstream too.
        QVERIFY(canvas.groups()->enterGroup("g7"));
        run = heldRun(canvas, "g7/second", seedUsed, &held);
        QCOMPARE(seed(run, {}, "pre"), QJsonValue(1.0));
        QCOMPARE(seed(run, "local/pass", "first"), QJsonValue(2.0));

        // A second instance of the definition: its inner seeds are not held in the document, the outer ones still are.
        canvas.setDocument(document(QString::fromUtf8(pass)
                                        .arg(R"(, "g8": {"type": "local/pass", "version": "1.0.0", "inputs": {"image": {"from": "pre.image"}}},
                                               "save2": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "g8.image"}}})")
                                        .toUtf8().constData()));
        run = heldRun(canvas, "save", seedUsed, &held);
        QCOMPARE(seed(run, {}, "pre"), QJsonValue(1.0));
        QCOMPARE(seed(run, "local/pass", "first"), random);
        QVERIFY(canvas.groups()->enterGroup("g7"));
        run = heldRun(canvas, "g7/second", seedUsed, &held);
        QCOMPARE(seed(run, {}, "pre"), QJsonValue(1.0));
        QCOMPARE(seed(run, "local/pass", "first"), random);
    }

    // A seed written in a definition that runs twice is held per instance (3.4): the definition is not edited, the run's compile
    // is given the value for that instance's node (CompileOptions::overrides), at the seed that instance showed.
    void runFromHereHoldsSeedsOfGroupsThatRunTwice() {
        const char *def = R"("groups": {"local/pass": {"version": "1.0.0", "name": "Pass",
            "inputs": [{"name": "image", "targets": ["first.image"]}], "outputs": [{"name": "image", "from": "second.image"}],
            "nodes": {"first": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "b", "seed": {"auto": "random"}}},
                      "second": {"type": "fugue/generate-image", "version": "1.0.0",
                                 "inputs": {"prompt": "c", "seed": 9, "image": {"from": "first.image"}}}}}})";
        const char *side = R"({"fugue": 1, "graph": {"nodes": {
            "pre": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "a"}},
            "g7": {"type": "local/pass", "version": "1.0.0", "inputs": {"image": {"from": "pre.image"}}},
            "g8": {"type": "local/pass", "version": "1.0.0", "inputs": {"image": {"from": "pre.image"}}},
            "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "g7.image"}}},
            "save2": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "g8.image"}}}}},
          %1})";
        const char *chain = R"({"fugue": 1, "graph": {"nodes": {
            "pre": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "a"}},
            "g7": {"type": "local/pass", "version": "1.0.0", "inputs": {"image": {"from": "pre.image"}}},
            "g8": {"type": "local/pass", "version": "1.0.0", "inputs": {"image": {"from": "g7.image"}}},
            "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "g8.image"}}}}},
          %1})";
        const QHash<QString, double> shown{{"g7/first/seed", 2}, {"g8/first/seed", 3}};
        const WidgetEdit::SeedUsed seedUsed = [&shown](const QString &path, const QString &input) -> std::optional<double> {
            const auto it = shown.constFind(path + "/" + input);
            return it == shown.constEnd() ? std::nullopt : std::optional<double>(*it);
        };
        // The value the run's compile gives the seed of an inner node: what the scheduler keys the node on.
        const auto compiledSeed = [this](const Document &doc, const WidgetEdit::HeldSeeds &seeds, const QString &from,
                                         const QString &path) {
            CompileOptions options;
            options.from = from;
            options.allowUnsetInputs = true;
            options.overrides = WidgetEdit::heldOverrides(seeds);
            const Compilation compiled = compile(doc, m_registry, options);
            return compiled.ok() ? compiled.value().nodes.value(path).inputs.value("seed").toJson() : QJsonValue(QStringLiteral("no compile"));
        };
        const QJsonValue random = QJsonObject{{"auto", "random"}};

        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(QString::fromUtf8(side).arg(def).toUtf8().constData()));
        // Downstream of one instance: that one is held at its own shown seed, the other is left alone.
        WidgetEdit::HeldSeeds seeds = canvas.widgets()->seedsToHold("save", seedUsed);
        QCOMPARE(WidgetEdit::heldOverrides(seeds).size(), 1);
        QCOMPARE(compiledSeed(canvas.document(), seeds, "save", "g7/first"), QJsonValue(2.0));
        QCOMPARE(compiledSeed(canvas.document(), seeds, "save", "g7/second"), QJsonValue(9.0));
        // The overrides are for the run only: the document the run starts from still has the definition as it was.
        QSet<QString> held;
        Document run = canvas.widgets()->withHeldSeeds(seeds, &held);
        QCOMPARE(run.group("local/pass")->nodes.value("first").inputs.value("seed").toJson(), random);
        QCOMPARE(compiledSeed(run, {}, "save", "g7/first"), random); // no overrides: it draws
        seeds = canvas.widgets()->seedsToHold("save2", seedUsed);
        QCOMPARE(compiledSeed(canvas.document(), seeds, "save2", "g8/first"), QJsonValue(3.0));

        // Both upstream: each held at its own value.
        canvas.setDocument(document(QString::fromUtf8(chain).arg(def).toUtf8().constData()));
        seeds = canvas.widgets()->seedsToHold("save", seedUsed);
        QCOMPARE(WidgetEdit::heldOverrides(seeds).size(), 2);
        QCOMPARE(compiledSeed(canvas.document(), seeds, "save", "g7/first"), QJsonValue(2.0));
        QCOMPARE(compiledSeed(canvas.document(), seeds, "save", "g8/first"), QJsonValue(3.0));
        // From the second instance's card: only the first is upstream.
        seeds = canvas.widgets()->seedsToHold("g8", seedUsed);
        QCOMPARE(compiledSeed(canvas.document(), seeds, "g8", "g7/first"), QJsonValue(2.0));
        QCOMPARE(compiledSeed(canvas.document(), seeds, "g8", "g8/first"), random);
    }

    // Deeper and exposed: the seed of a node two groups down, in a definition used twice, is held at its own instance path; an
    // exposed input on a nested instance that drives it is held on that instance (its definition runs once per outer instance, but
    // the value is written on the nested instance's node in the outer definition, which runs twice).
    void runFromHereHoldsSeedsTwoGroupsDeepAndThroughAnExposedInput() {
        const char *text = R"({"fugue": 1, "graph": {"nodes": {
            "g7": {"type": "local/outer", "version": "1.0.0", "inputs": {}},
            "g8": {"type": "local/outer", "version": "1.0.0", "inputs": {}},
            "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "g7.image"}}},
            "save2": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "g8.image"}}}}},
          "groups": {
            "local/inner": {"version": "1.0.0", "name": "Inner", "inputs": [{"name": "seed", "targets": ["k.seed"]}],
              "outputs": [{"name": "image", "from": "k.image"}],
              "nodes": {"k": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "b", "seed": {"auto": "random"}}}}},
            "local/outer": {"version": "1.0.0", "name": "Outer", "inputs": [], "outputs": [{"name": "image", "from": "inner.image"}],
              "nodes": {"inner": {"type": "local/inner", "version": "1.0.0", "inputs": {"seed": {"auto": "random"}}}}}}})";
        const QHash<QString, double> shown{{"g7/inner/k/seed", 21}, {"g8/inner/k/seed", 22}};
        const WidgetEdit::SeedUsed seedUsed = [&shown](const QString &path, const QString &input) -> std::optional<double> {
            const auto it = shown.constFind(path + "/" + input);
            return it == shown.constEnd() ? std::nullopt : std::optional<double>(*it);
        };
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(text));
        const auto overrides = WidgetEdit::heldOverrides(canvas.widgets()->seedsToHold("save", seedUsed));
        // The seed is written on the nested instance's exposed input, in the outer definition that has two instances: so the
        // override is on the instance g7/inner, input seed, which drives g7/inner/k.
        QCOMPARE(overrides.size(), 1);
        QCOMPARE(overrides.first().path, QStringLiteral("g7/inner"));
        QCOMPARE(overrides.first().input, QStringLiteral("seed"));
        QCOMPARE(overrides.first().value.literal(), QJsonValue(21.0));
        CompileOptions options;
        options.from = "save";
        options.allowUnsetInputs = true;
        options.overrides = overrides;
        const Compilation compiled = compile(canvas.document(), m_registry, options);
        QVERIFY2(compiled.ok(), "compile");
        QCOMPARE(compiled.value().nodes.value("g7/inner/k").inputs.value("seed").toJson(), QJsonValue(21.0));

        // Not exposed: the seed is written on k itself, two levels down, and held at g7/inner/k.
        QString plain = QString::fromUtf8(text);
        plain.replace(R"("inputs": [{"name": "seed", "targets": ["k.seed"]}])", R"("inputs": [])")
            .replace(R"("inputs": {"seed": {"auto": "random"}}}}}}})", R"("inputs": {}}}}}})");
        canvas.setDocument(document(plain.toUtf8().constData()));
        const auto deep = WidgetEdit::heldOverrides(canvas.widgets()->seedsToHold("save", seedUsed));
        QCOMPARE(deep.size(), 1);
        QCOMPARE(deep.first().path, QStringLiteral("g7/inner/k"));
        QCOMPARE(deep.first().value.literal(), QJsonValue(21.0));
    }

    // A definition's Increment seed held per instance advances only when an instance that ran used the definition's own value.
    void aDefinitionsIncrementSeedHeldInEveryInstanceThatRanDoesNotAdvance() {
        const char *text = R"({"fugue": 1, "graph": {"nodes": {
            "pre": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "a"}},
            "g7": {"type": "local/pass", "version": "1.0.0", "inputs": {"image": {"from": "pre.image"}}},
            "g8": {"type": "local/pass", "version": "1.0.0", "inputs": {"image": {"from": "pre.image"}}},
            "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "g7.image"}}},
            "save2": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "g8.image"}}}}},
          "groups": {"local/pass": {"version": "1.0.0", "name": "Pass",
            "inputs": [{"name": "image", "targets": ["first.image"]}], "outputs": [{"name": "image", "from": "second.image"}],
            "nodes": {"first": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "b", "seed": 10}},
                      "second": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "c", "image": {"from": "first.image"}}}}}},
          "layout": {"groups": {"local/pass": {"nodes": {"first": {"increment": ["seed"]}}}}}})";
        const QHash<QString, double> shown{{"g7/first/seed", 12}, {"g8/first/seed", 13}};
        const WidgetEdit::SeedUsed seedUsed = [&shown](const QString &path, const QString &input) -> std::optional<double> {
            const auto it = shown.constFind(path + "/" + input);
            return it == shown.constEnd() ? std::nullopt : std::optional<double>(*it);
        };
        const auto inDefinition = [](const CanvasModel &canvas) {
            return canvas.document().group("local/pass")->nodes.value("first").inputs.value("seed").toJson();
        };
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(text));
        QVERIFY(canvas.groups()->enterGroup("g7"));
        // A batch of four runs from g7's second node: g7 is held, g8 is not in the run.
        const WidgetEdit::HeldSeeds seeds = canvas.widgets()->seedsToHold("g7/second", seedUsed);
        QCOMPARE(WidgetEdit::heldOverrides(seeds).size(), 1);
        for (int i = 0; i < 4; ++i) {
            QSet<QString> held;
            canvas.widgets()->withHeldSeeds(seeds, &held);
            canvas.widgets()->advanceSeedsExcept(held);
        }
        QCOMPARE(inDefinition(canvas), QJsonValue(10.0));
        // Run from the sink after g7: still only g7 runs, held, so the seed is kept again.
        const QHash<QString, double> onlyG7{{"g7/first/seed", 12}};
        const WidgetEdit::SeedUsed seedUsedG7 = [&onlyG7](const QString &path, const QString &input) -> std::optional<double> {
            const auto it = onlyG7.constFind(path + "/" + input);
            return it == onlyG7.constEnd() ? std::nullopt : std::optional<double>(*it);
        };
        const WidgetEdit::HeldSeeds both = canvas.widgets()->seedsToHold("save", seedUsedG7); // g8 is not in this run either
        QSet<QString> held;
        canvas.widgets()->withHeldSeeds(both, &held);
        QVERIFY(held.contains("first/seed"));
        // g8 after g7, and g8's own seed has no shown result: that instance runs on the definition's own value, so it advances.
        QString chained = QString::fromUtf8(text);
        chained.replace(R"("g8": {"type": "local/pass", "version": "1.0.0", "inputs": {"image": {"from": "pre.image"}}})",
                        R"("g8": {"type": "local/pass", "version": "1.0.0", "inputs": {"image": {"from": "g7.image"}}})");
        canvas.setDocument(document(chained.toUtf8().constData()));
        QVERIFY(canvas.groups()->enterGroup("g7"));
        const WidgetEdit::HeldSeeds chain = canvas.widgets()->seedsToHold("g8/second", seedUsedG7);
        QCOMPARE(WidgetEdit::heldOverrides(chain).size(), 1);
        held.clear();
        canvas.widgets()->withHeldSeeds(chain, &held);
        QVERIFY(held.isEmpty());
        // A run that holds nothing (a plain run) advances it as always.
        canvas.setDocument(document(text));
        QVERIFY(canvas.groups()->enterGroup("g7"));
        canvas.widgets()->advanceSeedsExcept({});
        QCOMPARE(inDefinition(canvas), QJsonValue(11.0));
    }

    // An override that names no node, or an input the node or the group instance does not have, is an error, as in the document.
    void aCompileOverrideNamingNothingIsAnError() {
        const char *text = R"({"fugue": 1, "graph": {"nodes": {
            "g7": {"type": "local/pass", "version": "1.0.0", "inputs": {}},
            "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "g7.image"}}}}},
          "groups": {"local/pass": {"version": "1.0.0", "name": "Pass", "inputs": [], "outputs": [{"name": "image", "from": "first.image"}],
            "nodes": {"first": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "b", "seed": 3}}}}}})";
        const Document doc = document(text);
        const auto codes = [&](const InputOverride &given) {
            CompileOptions options;
            options.allowUnsetInputs = true;
            options.overrides = {given};
            QStringList found;
            const Compilation compiled = compile(doc, m_registry, options);
            for (const Error &error : compiled.problems())
                found << error.code;
            return found;
        };
        const InputValue seven = InputValue::literal(7);
        QVERIFY(codes({"g7/first", "seed", seven}).isEmpty());
        QCOMPARE(codes({"g7/nope", "seed", seven}), QStringList{"usage"});
        QCOMPARE(codes({"g7/first", "sedd", seven}), QStringList{"unknown_input"});
        QCOMPARE(codes({"g7", "seed", seven}), QStringList{"unknown_input"}); // the instance exposes nothing
    }

    // What else decides a held place: a library group named only by its type id is held inside as the run embeds it; an exposed
    // seed that also drives the end point is not held (it would pin the end point's own seed); an Increment seed never set is
    // held on its node.
    void runFromHereHoldsLibraryGroupsAndNeverTheEndPointsSeed() {
        // One exposed seed drives both inner nodes, so both used the same one.
        const QHash<QString, double> shown{{"pre/seed", 1}, {"g7/first/seed", 2}, {"g7/second/seed", 2}};
        const WidgetEdit::SeedUsed seedUsed = [&shown](const QString &path, const QString &input) -> std::optional<double> {
            const auto it = shown.constFind(path + "/" + input);
            return it == shown.constEnd() ? std::nullopt : std::optional<double>(*it);
        };
        const char *both = R"({"fugue": 1, "graph": {"nodes": {
            "pre": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "a"}},
            "g7": {"type": "local/pass", "version": "1.0.0", "inputs": {"image": {"from": "pre.image"}, "seed": {"auto": "random"}}},
            "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "g7.image"}}}}},
          "groups": {"local/pass": {"version": "1.0.0", "name": "Pass",
            "inputs": [{"name": "image", "targets": ["first.image"]}, {"name": "seed", "targets": ["first.seed", "second.seed"]}],
            "outputs": [{"name": "image", "from": "second.image"}],
            "nodes": {"first": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "b"}},
                      "second": {"type": "fugue/generate-image", "version": "1.0.0",
                                 "inputs": {"prompt": "c", "image": {"from": "first.image"}}}}}},
          "layout": {"nodes": {"pre": {"increment": ["seed"]}}}})";
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(both));
        QSet<QString> held;
        Document run = heldRun(canvas, "save", seedUsed, &held);
        QCOMPARE(run.graph.nodes.value("pre").inputs.value("seed").toJson(), QJsonValue(1.0)); // Increment, never set
        QCOMPARE(held, QSet<QString>{"pre/seed"});
        QCOMPARE(run.graph.nodes.value("g7").inputs.value("seed").toJson(), QJsonValue(2.0)); // drives two nodes, both before
        QVERIFY(canvas.groups()->enterGroup("g7"));
        held.clear();
        run = heldRun(canvas, "g7/second", seedUsed, &held);
        QCOMPARE(run.graph.nodes.value("pre").inputs.value("seed").toJson(), QJsonValue(1.0));
        QCOMPARE(run.graph.nodes.value("g7").inputs.value("seed").toJson(), QJsonValue(QJsonObject{{"auto", "random"}}));
        QVERIFY(held.isEmpty()); // pre's is not on the level shown: no advance to skip

        // A group from the library, the workflow naming only its type id: its inner Randomize seed is held in the run's copy.
        const char *lib = R"({"fugue": 1, "graph": {"nodes": {
            "pre": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": "a"}},
            "g7": {"type": "local/pass", "version": "1.0.0", "inputs": {"image": {"from": "pre.image"}}},
            "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "g7.image"}}}}},
          "groups": {"local/pass": {"version": "1.0.0", "name": "Pass",
            "inputs": [{"name": "image", "targets": ["first.image"]}], "outputs": [{"name": "image", "from": "first.image"}],
            "nodes": {"first": {"type": "fugue/generate-image", "version": "1.0.0",
                                "inputs": {"prompt": "b", "seed": {"auto": "random"}}}}}}})";
        QTemporaryDir packs;
        QVERIFY(saveToLibrary(document(lib), "local/pass", m_registry, packs.filePath("local")).ok());
        Registry library;
        nodes::addBuiltinNodeTypes(library);
        loadPacks(library, {packs.path()});
        Document bare = document(lib);
        bare.groups.clear();
        CanvasModel fromLibrary(&library);
        fromLibrary.setDocument(bare);
        run = heldRun(fromLibrary, "save", seedUsed, nullptr);
        QVERIFY(run.group("local/pass"));
        QCOMPARE(run.group("local/pass")->nodes.value("first").inputs.value("seed").toJson(), QJsonValue(2.0));
        QVERIFY(fromLibrary.document().groups.isEmpty()); // the canvas's own document is as it was
    }

    // A row whose key the group's face drives is there inside the group (6.1.2, 5.4): Make group moved lora_2's link to the
    // instance, and an edit inside keeps row 2's strength and On, which the compiler reads with the instance's link.
    void aRowWhoseKeyTheGroupDrivesKeepsItsValuesInside() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {
            "pick": {"type": "io.example.lora/pick", "version": "1.0.0", "inputs": {}},
            "apply": {"type": "fugue/apply-loras", "version": "1.0.0",
                      "inputs": {"lora_1": {"resource": "fugue/lora", "name": "ink.safetensors"},
                                 "lora_2": {"from": "pick.lora"}, "strength_2": 0.3, "enabled_2": false}}}},
          "layout": {"nodes": {"pick": {"pos": [0, 0]}, "apply": {"pos": [300, 0]}}}})"));
        canvas.select({"apply"});
        const QString instance = canvas.groups()->makeGroup();
        QVERIFY2(!instance.isEmpty(), qPrintable(canvas.groups()->problem()));
        QCOMPARE(from(canvas, instance, "lora_2"), QStringLiteral("pick.lora"));
        QVERIFY(canvas.groups()->enterGroup(instance));
        QCOMPARE(inputNames(canvas, "apply"),
                 (QStringList{"model", "lora_1", "strength_1", "enabled_1", "lora_2", "strength_2", "enabled_2", "lora_3"}));
        QCOMPARE(canvas.widgets()->setValue("apply", "strength_1", 0.5), QString());
        const Document whole = canvas.document();
        const Node inner = whole.group(whole.graph.nodes.value(instance).type)->nodes.value("apply");
        QCOMPARE(inner.inputs.value("strength_1").literal(), QJsonValue(0.5));
        QCOMPARE(inner.inputs.value("strength_2").literal(), QJsonValue(0.3));
        QCOMPARE(inner.inputs.value("enabled_2").literal(), QJsonValue(false));
    }
};

QTEST_MAIN(TestCanvasEdits)
#include "test_canvas_edits.moc"
