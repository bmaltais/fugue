// The Remove Background group of the pack examples/packs/io.github.bmaltais.fugue-remove-background
// (cutout/): it loads from the pack, has the ports the guide names, compiles with the pack, and
// without the pack says which pack is missing instead of failing in some other way.
//
// Seam: the real pack folder through loadPack and the built-in registry; no helper is started, so
// nothing here runs the model (the node runs on the fake helper and in the pack's own pytest).

#include <QDir>
#include <QtTest>

#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/group_library.h"
#include "engine/pack_loader.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;

namespace {

const QString kPack = QStringLiteral("io.github.bmaltais.fugue-remove-background");
const QString kGroup = kPack + QStringLiteral("/cutout");

Document instanceOf(const NodeType &group, const Registry &registry) {
    Document doc;
    Node instance;
    instance.type = group.id;
    instance.version = group.version;
    doc.graph.nodes.insert("cut", instance);
    embedGroupTypes(doc, registry);
    return doc;
}

} // namespace

class TestRemoveBackgroundGroup : public QObject {
    Q_OBJECT

    static LoadedPack loadInto(Registry &registry) {
        nodes::addBuiltinNodeTypes(registry);
        return loadPack(registry, QStringLiteral(FUGUE_SOURCE_DIR "/examples/packs/") + kPack);
    }

private slots:
    void thePackLoadsWithTheNodeAndTheGroup() {
        Registry registry;
        const LoadedPack pack = loadInto(registry);
        QVERIFY2(pack.enabled && pack.disabled.isEmpty(), qPrintable(pack.errors.value(0).message));
        QVERIFY(pack.nodes.contains(kGroup));
        QVERIFY(pack.nodes.contains(kPack + "/remove-background"));
        QCOMPARE(registry.nodeType(kPack + "/remove-background")->name, QString("Remove Background Mask"));
    }

    void theGroupHasTheCutoutAndMaskOutputsAndAdvancedInputs() {
        Registry registry;
        loadInto(registry);
        const NodeType *group = registry.nodeType(kGroup);
        QVERIFY(group);
        QCOMPARE(group->name, QString("Remove Background"));
        QCOMPARE(group->runtime.value("kind").toString(), QString("group"));
        QVERIFY(group->input("image"));
        QVERIFY(group->input("model")->optional);
        QCOMPARE(group->input("model")->type.toString(), kPack + "/birefnet-model");
        for (const char *name : {"threshold", "feather"}) {
            QVERIFY2(group->input(QLatin1String(name)), name);
            QCOMPARE(group->input(QLatin1String(name))->section, QString("Advanced"));
            QCOMPARE(group->input(QLatin1String(name))->defaultValue.toInt(), 0);
        }
        QVERIFY(group->input("image")->role.isEmpty());
        QCOMPARE(group->outputs.size(), qsizetype(2));
        QVERIFY(group->output("image") && group->output("image")->role.isEmpty());
        QCOMPARE(group->output("mask")->role, QString("mask"));
    }

    void theDefinitionMatchesItsManifestAndItsNodesExist() {
        Registry registry;
        loadInto(registry);
        const NodeType *group = registry.nodeType(kGroup);
        const Result<Document> file = loadLibraryGroup(*group);
        QVERIFY2(file.ok(), qPrintable(file.errors().value(0).message));
        const GroupDefinition *definition = file->group(kGroup);
        QVERIFY(definition);
        QCOMPARE(definition->nodes.size(), 3);
        for (const Node &node : definition->nodes) {
            const NodeType *type = registry.nodeTypeAt(node.type, node.version.major);
            QVERIFY2(type && type->version.minor >= node.version.minor, qPrintable(node.type));
        }
    }

    void anInstanceCompilesWithThePackAndOnlyWantsItsImage() {
        Registry registry;
        loadInto(registry);
        Document doc = instanceOf(*registry.nodeType(kGroup), registry);
        QVERIFY(doc.group(kGroup));
        const Compilation compiled = compile(doc, registry);
        for (const Error &error : compiled.errors())
            QVERIFY2(error.severity != Error::Severity::Error || error.code == QLatin1String("missing_input"),
                     qPrintable(error.code + ": " + error.message));
    }

    // The template embeds the definition. A machine without the pack still opens it and is told which type is missing.
    void withoutThePackTheEmbeddedGroupIsAnUnknownNodeTypeThatNamesIt() {
        Document doc;
        {
            Registry registry;
            loadInto(registry);
            doc = instanceOf(*registry.nodeType(kGroup), registry);
        }
        Registry bare;
        nodes::addBuiltinNodeTypes(bare);
        const Compilation compiled = compile(doc, bare);
        bool named = false;
        for (const Error &error : compiled.errors())
            if (error.severity == Error::Severity::Error && error.code == QLatin1String("unknown_node_type")
                && error.message.contains(kPack))
                named = true;
        QVERIFY2(named, compiled.errors().isEmpty() ? "no error" : qPrintable(compiled.errors().value(0).code + ": " + compiled.errors().value(0).message));
    }
};

QTEST_GUILESS_MAIN(TestRemoveBackgroundGroup)
#include "test_remove_background_group.moc"
