// The compiled-in node manifests (src/nodes/manifests): every one loads cleanly,
// is a fugue/ node, and sits in the folder named after it.

#include <QDirIterator>
#include <QJsonObject>
#include <QtTest>

#include "engine/json.h"
#include "engine/pack_loader.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;

namespace {

QStringList manifestPaths() {
    QStringList paths;
    QDirIterator it(":/fugue/nodes", {"node.json"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        paths << it.next();
    return paths;
}

} // namespace

class TestBuiltin : public QObject {
    Q_OBJECT

private slots:
    void manifestsAreCompiledIn() { QVERIFY(!manifestPaths().isEmpty()); }

    void everyManifestLoadsWithoutErrors() {
        for (const QString &path : manifestPaths()) {
            const Result<QJsonObject> manifest = readJsonObject(path);
            QVERIFY2(manifest.ok(), qPrintable(path));
            const QString id = manifest.value().value("id").toString();
            QVERIFY2(id.startsWith("fugue/"), qPrintable(path));
            QCOMPARE(id.mid(6), QFileInfo(path).dir().dirName());
        }
    }

    // The fugue pack loads through the pack loader: every node.json is listed in pack.json (an
    // unlisted one is a warning in `errors`), none is disabled, and the declared type is there.
    void theFuguePackLoadsWithoutProblems() {
        Registry registry;
        const LoadedPack pack = loadPack(registry, ":/fugue/nodes", PackOrigin::BuiltIn);
        QVERIFY2(pack.enabled, qPrintable(pack.errors.value(0).message));
        for (const Error &error : pack.errors)
            QFAIL(qPrintable(error.message));
        for (const PackProblem &node : pack.disabled)
            QFAIL(qPrintable(node.node + ": " + node.reason));
        QCOMPARE(pack.nodes.size(), manifestPaths().size());
        QCOMPARE(registry.packVersion("fugue"), QString("1.0.0"));
        QVERIFY(registry.portType(*PortType::parse("fugue/image-model")) != nullptr);
    }

    void addBuiltinNodeTypesRegistersThemAll() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        QCOMPARE(registry.nodeTypes().size(), manifestPaths().size());
        QVERIFY(registry.nodeType("fugue/text") != nullptr);
    }
};

QTEST_GUILESS_MAIN(TestBuiltin)
#include "test_builtin.moc"
