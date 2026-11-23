// Groups and Frames on the canvas (spec 5): entering and leaving a group, edits inside that
// change its definition, undo across levels, Make group (and its sinks), Ungroup, Make unique,
// exposing inner ports, Make group inside a group, Frames, the folded Advanced section, the
// recursion guard, groups in node search, and the library round trip.
//
// Seams: CanvasModel's group and Frame calls, observed through document() (the whole workflow),
// scopeDocument() (the level shown), the node rows, compile() and node search.

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <QUndoStack>

#include "canvas/canvas_model.h"
#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/group_library.h"
#include "engine/pack_loader.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "search/node_search.h"

using namespace fugue;

namespace {

Document document(const char *json) {
    const Result<Document> loaded = Document::load(QByteArray(json));
    if (!loaded.ok())
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

// prompt -> gen -> save, prompt -> join; all placed.
const char *chain = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0",
            "inputs": {"prompt": {"from": "prompt.text"}, "steps": 8, "seed": 3}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}, "name": "fox"}},
    "join": {"type": "fugue/text-concat", "version": "1.0.0", "inputs": {"a": {"from": "prompt.text"}}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [0, 100]}, "gen": {"pos": [300, 80]},
                       "save": {"pos": [640, 120]}, "join": {"pos": [300, 600]}}}
})";

QStringList rowIds(const CanvasModel &canvas) {
    QStringList ids;
    for (int i = 0; i < canvas.nodes()->rowCount(); ++i)
        ids << canvas.nodes()->data(canvas.nodes()->index(i, 0), NodeList::NodeId).toString();
    ids.sort();
    return ids;
}

QVariantList inputsOf(const CanvasModel &canvas, const QString &node) {
    for (int i = 0; i < canvas.nodes()->rowCount(); ++i) {
        const QModelIndex index = canvas.nodes()->index(i, 0);
        if (canvas.nodes()->data(index, NodeList::NodeId).toString() == node)
            return canvas.nodes()->data(index, NodeList::Inputs).toList();
    }
    return {};
}

QVariantMap port(const CanvasModel &canvas, const QString &node, const QString &name, bool output = false) {
    for (int i = 0; i < canvas.nodes()->rowCount(); ++i) {
        const QModelIndex index = canvas.nodes()->index(i, 0);
        if (canvas.nodes()->data(index, NodeList::NodeId).toString() != node)
            continue;
        for (const QVariant &p : canvas.nodes()->data(index, output ? NodeList::Outputs : NodeList::Inputs).toList())
            if (p.toMap().value("name") == name)
                return p.toMap();
    }
    return {};
}

// The compiled nodes, path ids with their first part dropped from grouped ones, so a graph and
// the same graph grouped compare.
QStringList compiledTypes(const Document &doc, const Registry &registry) {
    const Compilation compiled = compile(doc, registry);
    if (!compiled.ok())
        return {QStringLiteral("does not compile: ") + compiled.error().message};
    QStringList types;
    for (auto it = compiled->nodes.cbegin(); it != compiled->nodes.cend(); ++it)
        types << it.key().section('/', -1) + '=' + it->type;
    types.sort();
    return types;
}

} // namespace

class TestCanvasGroups : public QObject {
    Q_OBJECT

    Registry m_registry;

private slots:
    void initTestCase() { nodes::addBuiltinNodeTypes(m_registry); }

    void enteringShowsTheInsideAndEditsChangeTheDefinition() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"gen"});
        const QString instance = canvas.groups()->makeGroup();
        QVERIFY2(!instance.isEmpty(), qPrintable(canvas.groups()->problem()));
        QStringList top{"join", "prompt", "save", instance};
        top.sort();
        QCOMPARE(rowIds(canvas), top);
        const QString group = canvas.document().graph.nodes.value(instance).type;

        QSignalSpy scope(&canvas, &CanvasModel::scopeChanged);
        QVERIFY(canvas.groups()->enterGroup(instance));
        QCOMPARE(scope.count(), 1);
        QVERIFY(canvas.inGroup());
        QCOMPARE(canvas.scopePath(), instance);
        QCOMPARE(canvas.scopeGroup(), group);
        QCOMPARE(rowIds(canvas), QStringList{"gen"});
        QCOMPARE(canvas.breadcrumb().size(), 1);
        QCOMPARE(canvas.breadcrumb().first().toMap().value("title").toString(), QStringLiteral("Group"));
        // The face drives gen's prompt: marked inside.
        QCOMPARE(port(canvas, "gen", "prompt").value("exposedAs").toString(), QStringLiteral("prompt"));
        QCOMPARE(port(canvas, "gen", "image", true).value("exposedAs").toString(), QStringLiteral("image"));

        // An edit inside is an edit of the definition, one undo step.
        canvas.select({"gen"});
        canvas.toggleMode("bypass");
        QCOMPARE(canvas.document().group(group)->nodes.value("gen").mode, NodeMode::Bypass);
        QCOMPARE(canvas.scopeDocument().graph.nodes.value("gen").mode, NodeMode::Bypass);
        // The whole workflow is what is saved: the instance and the rest are there.
        QVERIFY(canvas.document().graph.nodes.contains("save"));

        QVERIFY(canvas.groups()->leaveGroup());
        QVERIFY(!canvas.inGroup());
        QCOMPARE(canvas.selectedIds(), QStringList{instance});
        QCOMPARE(canvas.document().group(group)->nodes.value("gen").mode, NodeMode::Bypass);
        // Undo after leaving restores the definition; the canvas stays at the top.
        canvas.undo();
        QCOMPARE(canvas.document().group(group)->nodes.value("gen").mode, NodeMode::Normal);
        QVERIFY(!canvas.inGroup());
        QVERIFY(!canvas.groups()->leaveGroup());
    }

    void undoingTheMakeGroupWhileInsideGoesBackUp() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"gen"});
        const QString instance = canvas.groups()->makeGroup();
        QVERIFY(canvas.groups()->enterGroup(instance));
        canvas.undo();
        QVERIFY(!canvas.inGroup());
        QCOMPARE(rowIds(canvas), (QStringList{"gen", "join", "prompt", "save"}));
        QVERIFY(canvas.document().groups.isEmpty());
        canvas.redo();
        QCOMPARE(canvas.document().groups.size(), 1);
        QVERIFY(!canvas.inGroup());
    }

    void makeGroupAsksAboutSinksAndLeavesThemOutside() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"gen", "save"});
        QSignalSpy asked(canvas.groups(), &GroupEdit::makeGroupAsked);
        canvas.groups()->requestMakeGroup();
        QCOMPARE(asked.count(), 1);
        QCOMPARE(asked.first().first().toStringList(), QStringList{"Save Image"});
        const QString instance = canvas.groups()->makeGroup(true);
        const Document doc = canvas.document();
        QVERIFY(doc.graph.nodes.contains("save"));
        QCOMPARE(doc.graph.nodes.value("save").inputs.value("image").from().toString(), instance + ".image");
        // The grouped workflow runs what the original did.
        QCOMPARE(compiledTypes(doc, m_registry), compiledTypes(document(chain), m_registry));
    }

    void ungroupPutsTheNodesBackAsOneStep() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        const QByteArray before = canvas.document().save();
        canvas.select({"gen", "join"});
        const QString instance = canvas.groups()->makeGroup();
        canvas.select({instance});
        const int steps = canvas.undoStack()->count();
        const QStringList back = canvas.groups()->ungroupSelection();
        QCOMPARE(back, (QStringList{"gen", "join"}));
        QCOMPARE(canvas.undoStack()->count(), steps + 1);
        QCOMPARE(canvas.selectedIds(), (QStringList{"gen", "join"}));
        const Document doc = canvas.document();
        QVERIFY(doc.groups.isEmpty()); // nothing else used it
        QCOMPARE(doc.save(), before);
    }

    void makeUniqueDetachesOneInstance() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"gen"});
        const QString first = canvas.groups()->makeGroup();
        canvas.select({first});
        const QString second = canvas.cloneSelection().value(0);
        QCOMPARE(canvas.document().graph.nodes.value(second).type, canvas.document().graph.nodes.value(first).type);
        canvas.select({second});
        const QString unique = canvas.groups()->makeUnique();
        QCOMPARE(unique, QStringLiteral("local/group-2"));
        QCOMPARE(canvas.document().graph.nodes.value(second).type, unique);
        QCOMPARE(canvas.document().graph.nodes.value(first).type, QStringLiteral("local/group"));
    }

    // #72: an expose inside a group tells the view at once: the node's ports change, so its
    // exposed mark shows without leaving and entering the group again. Likewise un-exposing.
    void exposingInsideAGroupRefreshesTheNodesPortsAtOnce() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"gen"});
        const QString instance = canvas.groups()->makeGroup();
        QVERIFY(canvas.groups()->enterGroup(instance));
        QSignalSpy changed(canvas.nodes(), &QAbstractItemModel::dataChanged);
        auto inputsChanged = [&](const QString &id) {
            for (const QList<QVariant> &call : changed) {
                const QModelIndex index = call.at(0).toModelIndex();
                const QList<int> roles = call.at(2).value<QList<int>>();
                if (index.data(NodeList::NodeId).toString() == id && (roles.isEmpty() || roles.contains(NodeList::Inputs)))
                    return true;
            }
            return false;
        };
        QVERIFY(canvas.groups()->exposeInput("gen", "seed"));
        QVERIFY(inputsChanged("gen"));
        changed.clear();
        QVERIFY(canvas.groups()->unexposeInput("gen", "seed"));
        QVERIFY(inputsChanged("gen"));
        QCOMPARE(port(canvas, "gen", "seed").value("exposedAs").toString(), QString());
        // An output too: un-exposed, its mark goes at once.
        QCOMPARE(port(canvas, "gen", "image", true).value("exposedAs").toString(), QStringLiteral("image"));
        changed.clear();
        QVERIFY(canvas.groups()->unexposeOutput("image"));
        QVERIFY(inputsChanged("gen")); // the whole row: its outputs with it
        QCOMPARE(port(canvas, "gen", "image", true).value("exposedAs").toString(), QString());
    }

    // #72: a port of a group's face is renamed where it shows: on the group card (outside) or on
    // the exposed mark of the inner port (inside). The label changes, the name stays, one step.
    void anExposedPortIsRenamedWhereItShows() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"gen"});
        const QString instance = canvas.groups()->makeGroup();
        QCOMPARE(canvas.groups()->portToRename(instance, "image", true).value("name").toString(), QStringLiteral("image"));
        QVERIFY(canvas.groups()->portToRename("prompt", "text", true).isEmpty()); // not a group's port
        QVERIFY(canvas.groups()->renamePort(instance, "image", true, "Picture"));
        QCOMPARE(port(canvas, instance, "image", true).value("label").toString(), QStringLiteral("Picture"));
        QCOMPARE(canvas.groups()->portToRename(instance, "image", true).value("label").toString(), QStringLiteral("Picture"));
        canvas.undo();
        QVERIFY(port(canvas, instance, "image", true).value("label").toString() != QStringLiteral("Picture"));
        QVERIFY(canvas.groups()->enterGroup(instance));
        QCOMPARE(canvas.groups()->portToRename("gen", "image", true).value("name").toString(), QStringLiteral("image"));
        QVERIFY(canvas.groups()->portToRename("gen", "steps", false).isEmpty()); // not exposed
        QVERIFY(canvas.groups()->renamePort("gen", "image", true, "Result"));
        // #74: the exposed mark inside shows the new label at once, as the card does.
        QCOMPARE(port(canvas, "gen", "image", true).value("exposedLabel").toString(), QStringLiteral("Result"));
        QCOMPARE(port(canvas, "gen", "image", true).value("exposedAs").toString(), QStringLiteral("image"));
        QVERIFY(canvas.groups()->leaveGroup());
        QCOMPARE(port(canvas, instance, "image", true).value("label").toString(), QStringLiteral("Result"));
    }

    void exposedPortsShowOnTheFaceAndInterfaceEditsReachInstances() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"gen"});
        const QString instance = canvas.groups()->makeGroup();
        QVERIFY(canvas.groups()->enterGroup(instance));
        QVERIFY2(canvas.groups()->exposeInput("gen", "seed"), qPrintable(canvas.groups()->problem()));
        QCOMPARE(canvas.groups()->exposedName("gen", "seed", false), QStringLiteral("seed"));
        QCOMPARE(port(canvas, "gen", "seed").value("exposedAs").toString(), QStringLiteral("seed"));
        QVERIFY(!canvas.groups()->exposeInput("gen", "seed"));
        QVERIFY(!canvas.groups()->problem().isEmpty());
        // steps goes in Advanced, renamed.
        QVERIFY(canvas.groups()->exposeInput("gen", "steps"));
        QVERIFY(canvas.groups()->editExposedInput("steps", {{"name", "quality"}, {"label", "Quality"}, {"section", "Advanced"},
                                                  {"min", 2}, {"max", 20}}));
        const QVariantMap face = canvas.groups()->groupInterface();
        QCOMPARE(face.value("inputs").toList().size(), 3);
        QCOMPARE(face.value("inputs").toList().last().toMap().value("name").toString(), QStringLiteral("quality"));
        QVERIFY(canvas.groups()->moveExposed(false, "quality", 0));
        QCOMPARE(canvas.groups()->groupInterface().value("inputs").toList().first().toMap().value("name").toString(),
                 QStringLiteral("quality"));
        QVERIFY(!canvas.groups()->editExposedInput("quality", {{"name", "Not A Name"}}));
        QVERIFY(canvas.groups()->renameGroup("Two pass"));
        QCOMPARE(canvas.scopeGroup(), QStringLiteral("local/two-pass"));
        QCOMPARE(canvas.breadcrumb().first().toMap().value("title").toString(), QStringLiteral("Two pass"));

        canvas.groups()->leaveGroup();
        QCOMPARE(canvas.document().graph.nodes.value(instance).type, QStringLiteral("local/two-pass"));
        // On the face: quality first, in a folded Advanced section, with the narrowed range.
        const QVariantMap quality = port(canvas, instance, "quality");
        QCOMPARE(quality.value("label").toString(), QStringLiteral("Quality"));
        QCOMPARE(quality.value("header").toString(), QStringLiteral("Advanced"));
        QVERIFY(quality.value("hidden").toBool());
        QCOMPARE(quality.value("folded").toInt(), 1);
        canvas.toggleAdvanced(instance);
        QVERIFY(!port(canvas, instance, "quality").value("hidden").toBool());
        QVERIFY(port(canvas, instance, "seed").value("widget").toBool());

        // Deleting an exposed node inside drops it from the face, and the instance's value.
        canvas.groups()->enterGroup(instance);
        canvas.select({"gen"});
        canvas.deleteSelection();
        QVERIFY(canvas.groups()->groupInterface().value("inputs").toList().isEmpty());
        canvas.groups()->leaveGroup();
        QVERIFY(!canvas.document().graph.nodes.value(instance).inputs.contains("prompt"));
        QVERIFY(!canvas.document().graph.nodes.value("save").inputs.contains("image"));
    }

    void anUnresolvedAdvancedInputStaysFoldedOnTheFace() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"gen"});
        const QString instance = canvas.groups()->makeGroup();
        QVERIFY(canvas.groups()->enterGroup(instance));
        QVERIFY(canvas.groups()->exposeInput("gen", "steps"));
        QVERIFY(canvas.groups()->editExposedInput("steps", {{"section", "Advanced"}}));
        canvas.groups()->leaveGroup();
        // The pack of the inner node is not installed: the same document on an empty registry.
        const Registry none;
        CanvasModel bare(&none);
        bare.setDocument(canvas.document());
        const QVariantMap steps = port(bare, instance, "steps");
        QVERIFY(!steps.isEmpty());
        QVERIFY(!steps.value("widget").toBool()); // really unresolved
        QCOMPARE(steps.value("header").toString(), QStringLiteral("Advanced"));
        QVERIFY(steps.value("hidden").toBool());
        QCOMPARE(steps.value("folded").toInt(), 1);
    }

    void aGroupMadeInsideAGroupKeepsTheOuterFaceWorking() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        const QStringList flat = compiledTypes(canvas.document(), m_registry);
        canvas.select({"gen", "join"});
        const QString outer = canvas.groups()->makeGroup();
        QVERIFY(canvas.groups()->enterGroup(outer));
        canvas.select({"gen"});
        const QString inner = canvas.groups()->makeGroup();
        QVERIFY2(!inner.isEmpty(), qPrintable(canvas.groups()->problem()));
        // The outer group's prompt drove gen.prompt; now it drives the inner instance.
        const GroupDefinition *face = canvas.document().group(canvas.scopeGroup());
        bool through = false;
        for (const GroupInput &input : face->inputs)
            for (const PortRef &target : input.targets)
                through |= target.node == inner;
        QVERIFY(through);
        QCOMPARE(face->outputs.value(0).from.node, inner);
        // Nested, it still runs the same nodes; and saves and loads.
        QCOMPARE(compiledTypes(canvas.document(), m_registry), flat);
        const Result<Document> reloaded = Document::load(canvas.document().save());
        QVERIFY(reloaded.ok());
        QCOMPARE(compiledTypes(*reloaded, m_registry), flat);
        // Entering the inner one: two crumbs; the first one leads back to the top.
        QVERIFY(canvas.groups()->enterGroup(inner));
        QCOMPARE(canvas.scopePath(), outer + "/" + inner);
        QCOMPARE(canvas.breadcrumb().size(), 2);
        canvas.groups()->leaveTo(0);
        QVERIFY(!canvas.inGroup());
        // Ungrouping inside the outer group puts gen back and the face follows.
        QVERIFY(canvas.groups()->enterGroup(outer));
        canvas.select({inner});
        canvas.groups()->ungroupSelection();
        QCOMPARE(compiledTypes(canvas.document(), m_registry), flat);
        QCOMPARE(canvas.document().groups.size(), 1);
    }

    void aGroupCannotGoInsideItself() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        canvas.select({"gen"});
        const QString instance = canvas.groups()->makeGroup();
        const QString group = canvas.document().graph.nodes.value(instance).type;
        canvas.select({instance});
        const QString copy = canvas.copySelection();
        QVERIFY(canvas.groups()->enterGroup(instance));
        QVERIFY(canvas.paste(copy).isEmpty());
        QVERIFY(!canvas.groups()->problem().isEmpty());
        QVERIFY(canvas.addNode(group, 0, 0).isEmpty());
        QVERIFY(!canvas.canAddGroup(group));
        // Node search leaves it out inside, and lists it at the top.
        canvas.search()->openAt(0, 0);
        canvas.search()->setQuery("Group");
        for (int i = 0; i < canvas.search()->count(); ++i)
            QVERIFY(canvas.search()->results()->data(canvas.search()->results()->index(i), SearchResults::TypeId) != group);
        canvas.search()->close();
        canvas.groups()->leaveGroup();
        canvas.search()->openAt(0, 0);
        canvas.search()->setQuery("Group");
        int row = -1;
        for (int i = 0; i < canvas.search()->count(); ++i)
            if (canvas.search()->results()->data(canvas.search()->results()->index(i), SearchResults::TypeId) == group)
                row = i;
        QVERIFY(row >= 0);
        const QString added = canvas.search()->accept(row);
        QCOMPARE(canvas.document().graph.nodes.value(added).type, group);
    }

    void framesHoldNodesAndMoveWithThem() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        // What a Frame holds is what lies in it: prompt alone here (gen is right of it).
        canvas.select({"prompt"});
        const QString frame = canvas.frameSelection();
        QCOMPARE(frame, QStringLiteral("f1"));
        const QVariantMap made = canvas.frames().first().toMap();
        QVERIFY(made.value("x").toReal() < 0 && made.value("y").toReal() < 100);
        // Its title band picks it.
        QCOMPARE(canvas.frameAt(made.value("x").toReal() + 4, made.value("y").toReal() + 4, 30), frame);
        canvas.select({});
        canvas.beginFrameMove(frame);
        QCOMPARE(canvas.selectedIds(), QStringList{"prompt"});
        canvas.moveBy(50, 20);
        canvas.endMove();
        QCOMPARE(canvas.position("prompt"), QPointF(50, 120));
        QCOMPARE(canvas.position("gen"), QPointF(300, 80));
        QCOMPARE(canvas.frames().first().toMap().value("x").toReal(), made.value("x").toReal() + 50);
        canvas.undo();
        QCOMPARE(canvas.position("prompt"), QPointF(0, 100));
        QCOMPARE(canvas.frames().first().toMap().value("x").toReal(), made.value("x").toReal());
        // A copy of everything it holds brings it along.
        canvas.select({"prompt"});
        canvas.setPointer(0, 1000);
        const QStringList pasted = canvas.paste(canvas.copySelection());
        QCOMPARE(canvas.frames().size(), 2);
        const QVariantMap second = canvas.frames().last().toMap();
        QCOMPARE(second.value("id").toString(), QStringLiteral("f2"));
        QCOMPARE(canvas.scopeDocument().layout.value("frames").toArray().last().toObject().value("nodes").toArray(),
                 QJsonArray{pasted.value(0)});
        QCOMPARE(second.value("y").toReal() - made.value("y").toReal(), canvas.position(pasted.value(0)).y() - 100);
        canvas.undo();
        QCOMPARE(canvas.frames().size(), 1);
        canvas.renameFrame(frame, "Inputs");
        QCOMPARE(canvas.frames().first().toMap().value("title").toString(), QStringLiteral("Inputs"));
        canvas.deleteFrame(frame);
        QVERIFY(canvas.frames().isEmpty());
        QVERIFY(canvas.document().graph.nodes.contains("prompt"));
    }

    void advancedInputsFoldUnlessOpenOrLinked() {
        CanvasModel canvas(&m_registry);
        canvas.setDocument(document(chain));
        // Generate Image's steps and guidance are advanced (6.1).
        QVERIFY(port(canvas, "gen", "steps").value("hidden").toBool());
        QCOMPARE(port(canvas, "gen", "steps").value("folded").toInt(), 2);
        const qreal folded = canvas.nodes()->data(canvas.nodes()->index(1, 0), NodeList::Height).toReal();
        Q_UNUSED(folded);
        canvas.toggleAdvanced("gen");
        QVERIFY(!port(canvas, "gen", "steps").value("hidden").toBool());
        QVERIFY(canvas.scopeDocument().layout.value("nodes").toObject().value("gen").toObject().value("advanced").toBool());
        canvas.undo();
        QVERIFY(port(canvas, "gen", "steps").value("hidden").toBool());
        // A linked advanced input shows anyway.
        QVERIFY(canvas.connectPorts("prompt", "text", "join", "b"));
        QVERIFY(inputsOf(canvas, "gen").size() > 0);
    }

    // #74: renaming a group card's title renames the group, as ComfyUI renames a subgraph from
    // its node's title; when the group is in the library, the window is offered to rename it
    // there too, which renames the library entry and keeps the workflow pointing at it.
    void renamingAGroupCardRenamesTheGroupAndOffersTheLibrary() {
        QTemporaryDir packs;
        CanvasModel canvas(&m_registry);
        canvas.groups()->setLibraryDir(packs.filePath("local"));
        canvas.setDocument(document(chain));
        canvas.select({"gen"});
        const QString instance = canvas.groups()->makeGroup();
        QSignalSpy offered(canvas.groups(), &GroupEdit::libraryRenameOffered);
        canvas.renameNode(instance, "Two pass");
        QCOMPARE(canvas.nodeTitle(instance), QStringLiteral("Two pass"));
        const QString type = canvas.document().graph.nodes.value(instance).type;
        QCOMPARE(canvas.document().group(type)->name, QStringLiteral("Two pass"));
        QCOMPARE(offered.size(), 0); // not in the library: nothing to offer
        QVERIFY(!canvas.document().layout.value("nodes").toObject().value(instance).toObject().contains("title"));
        // One undo step.
        canvas.undo();
        QCOMPARE(canvas.nodeTitle(instance), QStringLiteral("Group"));
        canvas.redo();
        QCOMPARE(canvas.nodeTitle(instance), QStringLiteral("Two pass"));

        canvas.select({instance});
        QVERIFY(canvas.groups()->saveGroupToLibrary());
        canvas.renameNode(instance, "Final pass");
        QCOMPARE(offered.size(), 1);
        const QString libraryId = offered.first().at(0).toString();
        QCOMPARE(libraryId, QStringLiteral("local/two-pass"));
        QVERIFY2(canvas.groups()->renameLibraryGroup(libraryId, "Final pass"), qPrintable(canvas.groups()->problem()));
        QCOMPARE(libraryGroups(packs.filePath("local")).first().id, QStringLiteral("local/final-pass"));
        const QString now = canvas.document().graph.nodes.value(instance).type;
        QCOMPARE(canvas.document().group(now)->extra.value(kLibraryKey).toString(), QStringLiteral("local/final-pass"));
        // The library's folder moved: no undo step brings back the old id.
        canvas.undo();
        const QString back = canvas.document().graph.nodes.value(instance).type;
        QCOMPARE(canvas.document().group(back)->extra.value(kLibraryKey).toString(), QStringLiteral("local/final-pass"));
        canvas.redo();

        // Renamed from the group's own name field too, the library is offered.
        QVERIFY(canvas.groups()->enterGroup(instance));
        QVERIFY(canvas.groups()->renameGroup("Last pass"));
        QCOMPARE(offered.size(), 2);
        QCOMPARE(offered.last().at(0).toString(), QStringLiteral("local/final-pass"));
        QVERIFY(canvas.groups()->leaveGroup());
        // A rename made in Settings (another window's) reaches this workflow's link to it.
        canvas.groups()->followLibraryRename("local/final-pass", "local/elsewhere");
        const QString later = canvas.document().graph.nodes.value(instance).type;
        QCOMPARE(canvas.document().group(later)->extra.value(kLibraryKey).toString(), QStringLiteral("local/elsewhere"));
    }

    void theLibraryRoundTrip() {
        QTemporaryDir packs;
        CanvasModel canvas(&m_registry);
        canvas.groups()->setLibraryDir(packs.filePath("local"));
        canvas.setDocument(document(chain));
        canvas.select({"gen"});
        const QString instance = canvas.groups()->makeGroup();
        canvas.select({instance});
        QVERIFY2(canvas.groups()->saveGroupToLibrary(), qPrintable(canvas.groups()->problem()));
        QVERIFY(canvas.groups()->notice().contains("local/group"));

        // Another window's registry has it as a node type; adding it embeds the definition.
        Registry library;
        nodes::addBuiltinNodeTypes(library);
        loadPacks(library, {packs.path()});
        QVERIFY(library.nodeType("local/group"));
        CanvasModel other(&library);
        other.setDocument(document(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        const QString added = other.addNode("local/group", 100, 100);
        QVERIFY2(!added.isEmpty(), qPrintable(other.groups()->problem()));
        QVERIFY(other.document().group("local/group"));
        QCOMPARE(other.updateAvailable(added), QString());

        // The library moves on: the other workflow sees an update, and takes it.
        canvas.groups()->enterGroup(instance);
        canvas.groups()->exposeInput("gen", "seed");
        canvas.groups()->leaveGroup();
        canvas.select({instance});
        QVERIFY(canvas.groups()->saveGroupToLibrary());
        Registry newer;
        nodes::addBuiltinNodeTypes(newer);
        loadPacks(newer, {packs.path()});
        CanvasModel later(&newer);
        later.setDocument(other.document());
        QCOMPARE(later.updateAvailable(added), QStringLiteral("1.1.0"));
        const QVariantMap changes = later.groups()->groupUpdate(added);
        QCOMPARE(changes.value("addedInputs").toStringList(), QStringList{"seed"});
        QSignalSpy asked(later.groups(), &GroupEdit::groupUpdateAsked);
        later.select({added});
        later.groups()->requestGroupUpdate();
        QCOMPARE(asked.count(), 1);
        QVERIFY(later.groups()->updateGroupInstance(added));
        QCOMPARE(later.document().graph.nodes.value(added).version.toString(), QStringLiteral("1.1.0"));
        QCOMPARE(later.updateAvailable(added), QString());
        QVERIFY(port(later, added, "seed").value("name").isValid());
    }
};

QTEST_MAIN(TestCanvasGroups)
#include "test_canvas_groups.moc"
