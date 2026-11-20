// Live node types in the window (#70, spec 5.3, 6.5, 11.6): a pack copied into the packs folder
// or removed from it, and a group saved to the library, show in the open window with no restart.
//
// Seam: the real window (Main.qml, offscreen) wired as main() wires it (followNodeTypeReloads),
// its pack folders the test's own; what the nodes show read from the canvas model's roles.

#include <QSignalSpy>
#include <QtTest>

#include "canvas_app.h"
#include "disk_pack.h"
#include "engine/group_library.h"
#include "engine/pack_loader.h"

using namespace canvas_test;

namespace {

// A text into a node of a pack that is not installed yet.
const QByteArray kUsesSay = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "words": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "hello"}},
    "say": {"type": "io.example.tools/say", "version": "1.0.0", "inputs": {"words": {"from": "words.text"}}}
  }},
  "layout": {"nodes": {"words": {"pos": [0, 0]}, "say": {"pos": [300, 0]}}, "view": {"center": [300, 100], "zoom": 1}}
})";

const QByteArray kChain = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a fox"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": {"from": "prompt.text"}}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [0, 0]}, "gen": {"pos": [300, 0]}}}
})";

QVariant role(const CanvasModel &model, const QString &node, int role) {
    const NodeList *nodes = model.nodes();
    for (int row = 0; row < nodes->rowCount(); ++row)
        if (nodes->data(nodes->index(row), NodeList::NodeId).toString() == node)
            return nodes->data(nodes->index(row), role);
    return {};
}

QStringList portNames(const CanvasModel &model, const QString &node, int which) {
    QStringList names;
    for (const QVariant &port : role(model, node, which).toList())
        names << port.toMap().value("name").toString();
    return names;
}

} // namespace

class TestCanvasReload : public QObject {
    Q_OBJECT

private slots:
    void init() { QDir(fugue::packsDir()).removeRecursively(); }
    void cleanup() { QDir(fugue::packsDir()).removeRecursively(); }

    void aPlaceholderTurnsRealWhenItsPackComesAndBackWhenItGoes() {
        App app(load(kUsesSay));
        QVERIFY(app.open());
        app.reloader->setSettleMs(50);
        QVERIFY(role(app.model, "say", NodeList::Missing).toBool());
        QVERIFY(app.workflows.notice().contains("io.example.tools/say"));
        const int links = app.model.links()->rowCount();
        fugue::NodeSearch *search = app.model.search();
        search->openAt(0, 0);
        search->setQuery("say");
        QCOMPARE(search->count(), 0);

        // Copied in from outside the window (`fugue pack add`): the watch sees it.
        QVERIFY(writeSayPack(fugue::packsDir(), "v1"));
        QTRY_VERIFY_WITH_TIMEOUT(!role(app.model, "say", NodeList::Missing).toBool(), 5000);
        QCOMPARE(portNames(app.model, "say", NodeList::Outputs), QStringList{"text"});
        QCOMPARE(role(app.model, "say", NodeList::Title).toString(), QStringLiteral("Say"));
        QCOMPARE(app.model.links()->rowCount(), links);
        QCOMPARE(search->count(), 1); // node search, open all along, lists it
        QVERIFY(app.workflows.notice().isEmpty()); // nothing is missing any more
        QVERIFY(!app.model.canUndo()); // nothing of the document changed

        // Removed: a placeholder again, which keeps its inputs and links.
        QVERIFY(QDir(fugue::packsDir() + "/io.example.tools").removeRecursively());
        QTRY_VERIFY_WITH_TIMEOUT(role(app.model, "say", NodeList::Missing).toBool(), 5000);
        QCOMPARE(app.model.links()->rowCount(), links);
        QCOMPARE(app.model.document().graph.nodes.value("say").inputs.value("words").kind(),
                 fugue::InputValue::Kind::Link);
        QCOMPARE(search->count(), 0);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Another window (its own registry, as another process has) saves a group to the library:
    // this one lists it, and once the library has a newer version, its instance says so.
    void aGroupSavedToTheLibraryElsewhereShowsHereWithItsUpdate() {
        App here(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(here.open());
        here.reloader->setSettleMs(50);

        fugue::Registry registry;
        fugue::nodes::addBuiltinNodeTypes(registry);
        CanvasModel elsewhere(&registry);
        elsewhere.setDocument(load(kChain));
        elsewhere.select({"gen"});
        const QString instance = elsewhere.makeGroup();
        elsewhere.select({instance});
        QVERIFY2(elsewhere.saveGroupToLibrary(), qPrintable(elsewhere.groupProblem()));
        QVERIFY(elsewhere.groupNotice().contains("Every open workflow"));

        QTRY_VERIFY_WITH_TIMEOUT(here.registry.nodeType("local/group"), 5000);
        const QString added = here.model.addNode("local/group", 100, 100);
        QVERIFY2(!added.isEmpty(), qPrintable(here.model.groupProblem()));
        QCOMPARE(role(here.model, added, NodeList::UpdateAvailable).toString(), QString());

        elsewhere.enterGroup(instance);
        QVERIFY(elsewhere.exposeInput("gen", "seed"));
        elsewhere.leaveGroup();
        elsewhere.select({instance});
        QVERIFY(elsewhere.saveGroupToLibrary());
        QTRY_COMPARE_WITH_TIMEOUT(role(here.model, added, NodeList::UpdateAvailable).toString(), QStringLiteral("1.1.0"), 5000);
        QVERIFY2(here.warnings.isEmpty(), qPrintable(here.warnings.join('\n')));
    }

    // Saved here: the registry has it at once, with no wait for the watch, and node search in
    // another workflow lists it.
    void aGroupSavedToTheLibraryHereIsInNodeSearchAtOnce() {
        App app(load(kChain));
        QVERIFY(app.open());
        app.reloader->setSettleMs(60000); // only the save's own reload can bring it in time
        app.model.select({"gen"});
        const QString instance = app.model.makeGroup();
        app.model.select({instance});
        QVERIFY(app.model.saveGroupToLibrary());
        QTRY_VERIFY(app.registry.nodeType("local/group"));

        app.workflows.newWorkflow();
        fugue::NodeSearch *search = app.model.search();
        search->openAt(0, 0);
        search->setQuery("group");
        bool listed = false;
        for (int row = 0; row < search->count(); ++row)
            listed |= search->results()->data(search->results()->index(row), fugue::SearchResults::TypeId) == "local/group";
        QVERIFY(listed);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }
};

QTEST_MAIN(TestCanvasReload)
#include "test_canvas_reload.moc"
