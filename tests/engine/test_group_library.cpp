// The group library (spec 5.3): groups saved to the reserved `local` pack, loaded back as node
// types of runtime.kind "group", versioned by 3.6, and updates of a workflow's definition.
//
// Seams: saveToLibrary, loadPacks (the first path's `local` folder is the library),
// loadLibraryGroup, groupChanges and updateGroup, observed through the files written, the
// registry and the documents returned; compile() for a library group a workflow does not hold.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/group_library.h"
#include "engine/groups.h"
#include "engine/json.h"
#include "engine/pack_loader.h"
#include "engine/registry.h"

using namespace fugue;

namespace {

QJsonObject json(const QByteArray &text) {
    QByteArray bytes(text);
    bytes.replace('\'', '"');
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
        qFatal("bad test JSON: %s", text.constData());
    return document.object();
}

QString dump(const Errors &errors) {
    return QString::fromUtf8(QJsonDocument(errorEnvelope(errors)).toJson(QJsonDocument::Compact));
}

void addTypes(Registry &r) {
    const char *manifests[] = {
        "{'manifest': 1, 'id': 'fugue/text', 'version': '1.0.0', 'name': 'Text', 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': [{'name': 'text', 'type': 'text'}]}",
        "{'manifest': 1, 'id': 'fugue/generate', 'version': '1.0.0', 'name': 'Generate', 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'prompt', 'type': 'text'}, {'name': 'steps', 'type': 'number', 'integer': true, 'default': 4}],"
        " 'outputs': [{'name': 'image', 'type': 'image'}]}",
        "{'manifest': 1, 'id': 'fugue/save', 'version': '1.0.0', 'name': 'Save', 'sink': true, 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'image', 'type': 'image'}], 'outputs': []}",
    };
    for (const char *manifest : manifests)
        if (hasErrors(r.addManifest(json(manifest))))
            qFatal("bad fake manifest");
}

// prompt -> [gen] -> save, gen made into the group local/pass.
Document workflow(const Registry &r, QString *instance) {
    const Result<Document> doc = Document::fromJson(json(
        "{'fugue': 1, 'graph': {'nodes': {"
        " 'prompt': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'a fox'}},"
        " 'gen': {'type': 'fugue/generate', 'version': '1.0.0', 'inputs': {'prompt': {'from': 'prompt.text'}, 'steps': 8}},"
        " 'save': {'type': 'fugue/save', 'version': '1.0.0', 'inputs': {'image': {'from': 'gen.image'}}}}},"
        " 'layout': {'nodes': {'prompt': {'pos': [0, 0]}, 'gen': {'pos': [300, 0]}, 'save': {'pos': [600, 0]}}}}"));
    Result<GroupMade> made = makeGroup(*doc, {"gen"}, r);
    Document out = *renameGroup(made->document, made->group, "Pass", r);
    *instance = made->instance;
    return out;
}

} // namespace

class TestGroupLibrary : public QObject {
    Q_OBJECT

    Registry m_registry;

private slots:
    void initTestCase() { addTypes(m_registry); }

    void aSavedGroupLoadsBackAsANodeType() {
        QTemporaryDir packs;
        QString instance;
        const Document doc = workflow(m_registry, &instance);
        QVERIFY(doc.group("local/pass"));
        const Result<SavedGroup> saved = saveToLibrary(doc, "local/pass", m_registry, packs.filePath("local"));
        QVERIFY2(saved.ok(), qPrintable(dump(saved.errors())));
        QCOMPARE(saved->id, QStringLiteral("local/pass"));
        QCOMPARE(saved->version.toString(), QStringLiteral("1.0.0"));
        QVERIFY(QFile::exists(packs.filePath("local/pass/node.json")));
        QVERIFY(QFile::exists(packs.filePath("local/pass/group.fugue")));

        Registry library;
        addTypes(library);
        const PackReport report = loadPacks(library, {packs.path()});
        QCOMPARE(report.packs.size(), 1);
        QVERIFY2(report.packs[0].enabled && report.packs[0].disabled.isEmpty(),
                 qPrintable(dump(report.packs[0].errors) + report.packs.value(0).disabled.value(0).reason));
        const NodeType *type = library.nodeType("local/pass");
        QVERIFY(type);
        QCOMPARE(type->runtime.value("kind").toString(), QStringLiteral("group"));
        QCOMPARE(type->name, QStringLiteral("Pass"));
        QVERIFY(type->input("prompt"));
        QVERIFY(type->output("image"));

        const Result<Document> file = loadLibraryGroup(*type);
        QVERIFY2(file.ok(), qPrintable(dump(file.errors())));
        QCOMPARE(file->group("local/pass")->nodes.keys(), QList<QString>{"gen"});
        QVERIFY(file->layout.value("groups").toObject().contains("local/pass"));

        // Saving it again unchanged keeps the version.
        const Result<SavedGroup> again = saveToLibrary(doc, "local/pass", m_registry, packs.filePath("local"));
        QVERIFY(again.ok());
        QVERIFY(!again->changed);
        QCOMPARE(again->version.toString(), QStringLiteral("1.0.0"));
    }

    void versionsFollowTheRulesOfThreeSix() {
        QTemporaryDir packs;
        const QString dir = packs.filePath("local");
        QString instance;
        Document doc = workflow(m_registry, &instance);
        QVERIFY(saveToLibrary(doc, "local/pass", m_registry, dir).ok());

        // An inner value changes: patch.
        doc.groups["local/pass"].nodes["gen"].inputs.insert("steps", InputValue::literal(9));
        Result<SavedGroup> saved = saveToLibrary(doc, "local/pass", m_registry, dir);
        QCOMPARE(saved->version.toString(), QStringLiteral("1.0.1"));
        // A new exposed input: minor.
        doc = *exposeInput(doc, "local/pass", {"gen", "steps"}, m_registry);
        saved = saveToLibrary(doc, "local/pass", m_registry, dir);
        QCOMPARE(saved->version.toString(), QStringLiteral("1.1.0"));
        // One goes: major.
        doc = *unexposeInput(doc, "local/pass", {"gen", "prompt"});
        saved = saveToLibrary(doc, "local/pass", m_registry, dir);
        QCOMPARE(saved->version.toString(), QStringLiteral("2.0.0"));
        const QJsonObject pack = *readJsonObject(dir + "/pack.json");
        QCOMPARE(pack.value("id").toString(), QStringLiteral("local"));
        QCOMPARE(pack.value("nodes").toArray(), QJsonArray{"pass"});
    }

    void anUpdateShowsWhatChangesAndMovesEveryInstance() {
        QString instance;
        const Document mine = workflow(m_registry, &instance);
        // The library's newer version: steps exposed, prompt no longer, output renamed.
        Document theirs = *exposeInput(mine, "local/pass", {"gen", "steps"}, m_registry);
        theirs = *unexposeInput(theirs, "local/pass", {"gen", "prompt"});
        theirs = *renameExposedOutput(theirs, "local/pass", "image", "picture");
        theirs.groups["local/pass"].version = Version{2, 0, 0};

        const GroupChanges changes = groupChanges(mine, theirs, "local/pass", m_registry);
        QCOMPARE(changes.from.toString(), QStringLiteral("1.0.0"));
        QCOMPARE(changes.to.toString(), QStringLiteral("2.0.0"));
        QCOMPARE(changes.addedInputs, QStringList{"steps"});
        QCOMPARE(changes.removedInputs, QStringList{"prompt"});
        QCOMPARE(changes.addedOutputs, QStringList{"picture"});
        QCOMPARE(changes.removedOutputs, QStringList{"image"});

        const Document updated = updateGroup(mine, theirs, "local/pass", m_registry);
        const Node node = updated.graph.nodes.value(instance);
        QCOMPARE(node.version.toString(), QStringLiteral("2.0.0"));
        QVERIFY(!node.inputs.contains("prompt"));
        QVERIFY(!updated.graph.nodes.value("save").inputs.contains("image"));
        QCOMPARE(updated.group("local/pass")->version.toString(), QStringLiteral("2.0.0"));
        QVERIFY(updated.group("local/pass")->outputs.value(0).name == QStringLiteral("picture"));
    }

    void aNestedGroupThatClashesComesInUnderANewId() {
        QString instance;
        Document theirs = workflow(m_registry, &instance);
        // The library's local/pass holds an instance of its own local/inner.
        GroupDefinition inner;
        inner.version = Version{1, 0, 0};
        inner.name = "Inner";
        inner.nodes.insert("t", Node{"fugue/text", Version{1, 0, 0}, {}, {}, {}, {}, {}});
        theirs.groups.insert("local/inner", inner);
        theirs.groups["local/pass"].nodes.insert("i", Node{"local/inner", Version{1, 0, 0}, {}, {}, {}, {}, {}});
        theirs.groups["local/pass"].version = Version{1, 1, 0};
        // The workflow has another local/inner.
        Document mine = workflow(m_registry, &instance);
        GroupDefinition other = inner;
        other.name = "Other";
        mine.groups.insert("local/inner", other);

        const Document updated = updateGroup(mine, theirs, "local/pass", m_registry);
        QCOMPARE(updated.group("local/inner")->name, QStringLiteral("Other"));
        const QString type = updated.group("local/pass")->nodes.value("i").type;
        QCOMPARE(type, QStringLiteral("local/inner-2"));
        QCOMPARE(updated.group(type)->name, QStringLiteral("Inner"));
    }

    void theLibraryHoldsGroupsOnlyAndNoOtherFolderIsLocal() {
        QTemporaryDir first, second;
        auto write = [](const QString &path, const QByteArray &text) {
            QDir().mkpath(QFileInfo(path).path());
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(json(text)).toJson());
        };
        write(first.filePath("local/pack.json"),
              "{'manifest': 1, 'id': 'local', 'version': '1.0.0', 'name': 'Library', 'nodes': ['run']}");
        write(first.filePath("local/run/node.json"),
              "{'manifest': 1, 'id': 'local/run', 'version': '1.0.0', 'name': 'Run',"
              " 'runtime': {'kind': 'command', 'entry': 'run.sh'}, 'inputs': [], 'outputs': []}");
        write(second.filePath("local/pack.json"),
              "{'manifest': 1, 'id': 'local', 'version': '1.0.0', 'name': 'Fake', 'nodes': []}");
        Registry r;
        const PackReport report = loadPacks(r, {first.path(), second.path()});
        QCOMPARE(report.packs.size(), 2);
        QVERIFY(report.packs[0].enabled);
        QCOMPARE(report.packs[0].disabled.size(), 1);
        QVERIFY(report.packs[0].disabled[0].reason.contains("groups only"));
        QVERIFY(!r.nodeType("local/run"));
        QVERIFY(!report.packs[1].enabled); // only the first path's `local` is the library
    }

    void aLibraryGroupTheWorkflowDoesNotHoldDoesNotCompile() {
        QTemporaryDir packs;
        QString instance;
        const Document doc = workflow(m_registry, &instance);
        QVERIFY(saveToLibrary(doc, "local/pass", m_registry, packs.filePath("local")).ok());
        Registry library;
        addTypes(library);
        loadPacks(library, {packs.path()});
        Document bare = doc;
        bare.groups.clear();
        const Compilation compiled = compile(bare, library);
        QVERIFY(!compiled.ok());
        QCOMPARE(compiled.error().code, QStringLiteral("unknown_node_type"));
        QCOMPARE(compiled.error().node, instance);
        QVERIFY(compiled.error().hint.contains("library"));
    }
};

QTEST_GUILESS_MAIN(TestGroupLibrary)
#include "test_group_library.moc"
