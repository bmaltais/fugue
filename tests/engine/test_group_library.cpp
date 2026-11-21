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

    // Copies the folder of a saved group under other names and lists them all in pack.json.
    static void copyAndList(const QString &dir, const QStringList &copies) {
        QJsonArray nodes{"pass"};
        for (const QString &name : copies) {
            QDir().mkpath(dir + "/" + name);
            QVERIFY(QFile::copy(dir + "/pass/node.json", dir + "/" + name + "/node.json"));
            QVERIFY(QFile::copy(dir + "/pass/group.fugue", dir + "/" + name + "/group.fugue"));
            nodes << name;
        }
        QJsonObject pack = readJsonObject(dir + "/pack.json").value();
        pack.insert("nodes", nodes);
        QVERIFY(!hasErrors(writeJsonFile(dir + "/pack.json", pack)));
    }

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

        // Another group of that name cannot overwrite it; the one saved can, and unchanged it
        // keeps the version.
        const Result<SavedGroup> other = saveToLibrary(doc, "local/pass", m_registry, packs.filePath("local"));
        QCOMPARE(other.error().code, QStringLiteral("group_invalid"));
        Document mine = doc;
        mine.groups["local/pass"].extra.insert(kLibraryKey, "local/pass");
        const Result<SavedGroup> again = saveToLibrary(mine, "local/pass", m_registry, packs.filePath("local"));
        QVERIFY(again.ok());
        QVERIFY(!again->changed);
        QCOMPARE(again->version.toString(), QStringLiteral("1.0.0"));
    }

    // #74: the library lists its groups, renames one (its folder and id follow the name, as a
    // group's id does in a workflow) and removes one, keeping pack.json in step.
    void theLibraryListsRenamesAndRemovesItsGroups() {
        QTemporaryDir packs;
        const QString dir = packs.filePath("local");
        QString instance;
        Document doc = workflow(m_registry, &instance);
        QVERIFY(saveToLibrary(doc, "local/pass", m_registry, dir).ok());
        QList<LibraryGroup> groups = libraryGroups(dir);
        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.first().id, QStringLiteral("local/pass"));
        QCOMPARE(groups.first().name, QStringLiteral("Pass"));
        QCOMPARE(groups.first().version.toString(), QStringLiteral("1.0.0"));

        const Result<QString> renamed = renameInLibrary("local/pass", "Two pass", m_registry, dir);
        QVERIFY2(renamed.ok(), qPrintable(dump(renamed.errors())));
        QCOMPARE(*renamed, QStringLiteral("local/two-pass"));
        QVERIFY(!QFileInfo::exists(dir + "/pass"));
        groups = libraryGroups(dir);
        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.first().id, QStringLiteral("local/two-pass"));
        QCOMPARE(groups.first().name, QStringLiteral("Two pass"));
        Registry library;
        addTypes(library);
        loadPacks(library, {packs.path()});
        QVERIFY(library.nodeType("local/two-pass"));
        QVERIFY(!library.nodeType("local/pass"));
        QVERIFY(loadLibraryGroup(*library.nodeType("local/two-pass")).ok());

        // A name another library group has is refused; so is an unknown group.
        QVERIFY(saveToLibrary(doc, "local/pass", m_registry, dir).ok());
        QCOMPARE(renameInLibrary("local/pass", "Two pass", m_registry, dir).error().code, QStringLiteral("group_invalid"));
        QCOMPARE(renameInLibrary("local/none", "X", m_registry, dir).error().code, QStringLiteral("group_invalid"));

        QVERIFY(!hasErrors(removeFromLibrary("local/two-pass", dir)));
        QVERIFY(!QFileInfo::exists(dir + "/two-pass"));
        groups = libraryGroups(dir);
        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.first().id, QStringLiteral("local/pass"));
        QVERIFY(hasErrors(removeFromLibrary("local/two-pass", dir)));

        // A hand-edited pack.json cannot make a removal reach outside the library.
        QFile pack(dir + "/pack.json");
        QVERIFY(pack.open(QIODevice::WriteOnly));
        pack.write(R"({"manifest": 1, "id": "local", "version": "1.0.0", "nodes": ["..", "pass"]})");
        pack.close();
        QVERIFY(hasErrors(removeFromLibrary("local/..", dir)));
        QVERIFY(QFileInfo::exists(dir + "/pass"));
        QCOMPARE(libraryGroups(dir).size(), 1);
    }

    void versionsFollowTheRulesOfThreeSix() {
        QTemporaryDir packs;
        const QString dir = packs.filePath("local");
        QString instance;
        Document doc = workflow(m_registry, &instance);
        QVERIFY(saveToLibrary(doc, "local/pass", m_registry, dir).ok());
        // As the canvas does after a save: the definition names its library entry.
        doc.groups["local/pass"].extra.insert(kLibraryKey, "local/pass");

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

    // A folder that went without pack.json being rewritten (by hand, an interrupted removal)
    // is dropped from the list when the library loads: quietly, once, and nothing else is touched.
    void theLibraryDropsEntriesWhoseFolderIsGone() {
        QTemporaryDir packs;
        const QString dir = packs.filePath("local");
        QString instance;
        Document doc = workflow(m_registry, &instance);
        QVERIFY(saveToLibrary(doc, "local/pass", m_registry, dir).ok());
        copyAndList(dir, {"other", "bare"});
        QVERIFY(QDir(dir + "/other").removeRecursively());  // gone
        QVERIFY(QFile::remove(dir + "/bare/node.json"));    // no definition left
        QDir().mkpath(dir + "/stray");                      // on disk, never listed
        QFile marker(dir + "/stray/keep.txt");
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();

        Registry library;
        addTypes(library);
        const PackReport report = loadPacks(library, {packs.path()});
        QCOMPARE(report.packs.size(), 1);
        QVERIFY(report.packs[0].enabled);
        QVERIFY(report.packs[0].disabled.isEmpty());
        QVERIFY(library.nodeType("local/pass"));
        const Result<QJsonObject> pack = readJsonObject(dir + "/pack.json");
        QVERIFY(pack.ok());
        QCOMPARE(pack->value("nodes").toArray(), QJsonArray{"pass"});
        // Nothing but pack.json changed: the folder with no definition and the stray one stay.
        QVERIFY(QFileInfo::exists(dir + "/bare/group.fugue"));
        QVERIFY(QFileInfo::exists(dir + "/stray/keep.txt"));
        QCOMPARE(healLibrary(dir), 0); // once: nothing more to drop

        // Another pack keeps its warning for the same fault.
        QTemporaryDir other;
        QDir().mkpath(other.filePath("io.example.tools"));
        QFile tools(other.filePath("io.example.tools/pack.json"));
        QVERIFY(tools.open(QIODevice::WriteOnly));
        tools.write(R"({"manifest": 1, "id": "io.example.tools", "version": "1.0.0", "name": "T", "nodes": ["gone"]})");
        tools.close();
        Registry r2;
        const PackReport second = loadPacks(r2, {other.path()});
        QCOMPARE(second.packs.value(0).disabled.size(), 1);
    }

    // Removing a group rewrites pack.json from what is on disk, even when the folder only
    // partly goes or an entry was dangling already.
    void removingRewritesTheListFromWhatIsOnDisk() {
        QTemporaryDir packs;
        const QString dir = packs.filePath("local");
        QString instance;
        Document doc = workflow(m_registry, &instance);
        QVERIFY(saveToLibrary(doc, "local/pass", m_registry, dir).ok());
        copyAndList(dir, {"dangling"});
        QVERIFY(QDir(dir + "/dangling").removeRecursively()); // pack.json still lists it
        QVERIFY(!hasErrors(removeFromLibrary("local/pass", dir)));
        const Result<QJsonObject> pack = readJsonObject(dir + "/pack.json");
        QVERIFY(pack.ok());
        QVERIFY(pack->value("nodes").toArray().isEmpty());

        // A folder that cannot be removed (read-only) stays listed, with an error, to try again.
        QVERIFY(saveToLibrary(doc, "local/pass", m_registry, dir).ok());
        QVERIFY(QFile::setPermissions(dir + "/pass", QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const Errors failed = removeFromLibrary("local/pass", dir);
        QFile::setPermissions(dir + "/pass", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        if (hasErrors(failed)) // (running as root removes it anyway)
            QCOMPARE(readJsonObject(dir + "/pack.json")->value("nodes").toArray(), QJsonArray{"pass"});
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
        // The CLI and runs embed it first (an agent wrote only the type id): then it compiles,
        // and the instance keeps the version it was written with.
        QCOMPARE(embedGroupTypes(bare, library), QStringList{"local/pass"});
        QVERIFY(bare.group("local/pass"));
        QCOMPARE(bare.graph.nodes.value(instance).version.toString(), QStringLiteral("1.0.0"));
        const Compilation embedded = compile(bare, library);
        QVERIFY2(embedded.ok(), qPrintable(dump(embedded.errors())));
    }

    void anUpdateDropsRetypedPortsAndOuterFacesFollow() {
        QString instance;
        const Document mine = workflow(m_registry, &instance);
        // The workflow nests local/pass in local/outer, whose face exposes pass's prompt.
        Result<GroupMade> outer = makeGroup(mine, {instance, "prompt"}, m_registry);
        QVERIFY(outer.ok());
        Document doc = *exposeInput(outer->document, outer->group, {"prompt", "text"}, m_registry);
        // pass's output now feeds save through outer's face; drop it in the library version.
        Document theirs = *unexposeOutput(doc, "local/pass", "image");
        theirs.groups["local/pass"].version = Version{2, 0, 0};
        const Document updated = updateGroup(doc, theirs, "local/pass", m_registry);
        // outer's output led from pass.image: it goes too, and save's link with it.
        QVERIFY(updated.group(outer->group)->outputs.isEmpty());
        QVERIFY(!updated.graph.nodes.value("save").inputs.contains("image"));
    }

    void renamingAnInnerGroupsInputReachesTheOuterFace() {
        QString instance;
        const Document mine = workflow(m_registry, &instance);
        Result<GroupMade> outer = makeGroup(mine, {instance, "prompt"}, m_registry);
        Document doc = *exposeInput(outer->document, outer->group, {instance, "prompt"}, m_registry);
        doc = *editExposedInput(doc, "local/pass", "prompt", {"words", {}, {}, {}, {}, {}, {}}, m_registry);
        const GroupInput &driven = doc.group(outer->group)->inputs.last();
        QCOMPARE(driven.targets.value(0).toString(), instance + ".words");
        doc = *renameExposedOutput(doc, "local/pass", "image", "picture");
        QCOMPARE(doc.group(outer->group)->outputs.value(0).from.toString(), instance + ".picture");
        // Stop exposing it inside: the outer face loses the target, the input with it.
        doc = *unexposeInput(doc, "local/pass", {"gen", "prompt"});
        QVERIFY(std::none_of(doc.group(outer->group)->inputs.begin(), doc.group(outer->group)->inputs.end(),
                             [&](const GroupInput &i) { return i.targets.contains(PortRef{instance, "words"}); }));
    }

    void anExposedInputTakesADefaultAndChoicesThatFit() {
        QString instance;
        Document doc = workflow(m_registry, &instance);
        doc = *exposeInput(doc, "local/pass", {"gen", "steps"}, m_registry);
        ExposedChange change{"steps", {}, {}, 1, 10, QJsonValue(6), std::nullopt};
        doc = *editExposedInput(doc, "local/pass", "steps", change, m_registry);
        QCOMPARE(doc.group("local/pass")->inputs.last().defaultValue->literal().toInt(), 6);
        change.defaultValue = QJsonValue(40);
        QCOMPARE(editExposedInput(doc, "local/pass", "steps", change, m_registry).error().code,
                 QStringLiteral("value_out_of_range"));
        change.defaultValue = QJsonValue::Undefined;
        change.choices = QStringList{"a"};
        QCOMPARE(editExposedInput(doc, "local/pass", "steps", change, m_registry).error().code,
                 QStringLiteral("group_invalid"));
    }
};

QTEST_GUILESS_MAIN(TestGroupLibrary)
#include "test_group_library.moc"
