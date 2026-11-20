// The canvas model's edits and their one undo stack (spec 12.4, 12.11): selection, delete,
// copy and paste, clone, bypass, mute, collapse, and undo and redo of each, checked against
// the document the model gives back (canvas_model.h).

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>
#include <QUndoStack>

#include "canvas/canvas_model.h"
#include "canvas/widget_edit.h"
#include "engine/document.h"
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

QString from(const CanvasModel &canvas, const QString &node, const QString &input) {
    const InputValue value = canvas.document().graph.nodes.value(node).inputs.value(input);
    return value.kind() == InputValue::Kind::Link ? value.from().toString() : QString();
}

} // namespace

class TestCanvasEdits : public QObject {
    Q_OBJECT

    Registry m_registry;

private slots:
    void initTestCase() { nodes::addBuiltinNodeTypes(m_registry); }

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
};

QTEST_MAIN(TestCanvasEdits)
#include "test_canvas_edits.moc"
