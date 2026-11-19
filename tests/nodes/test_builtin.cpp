// The compiled-in node manifests (src/nodes/manifests): every one loads cleanly,
// is a fugue/ node, and sits in the folder named after it.

#include <QDirIterator>
#include <QJsonObject>
#include <QtTest>

#include "engine/json.h"
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
            Registry registry;
            nodes::addBuiltinPortTypes(registry);
            const Errors errors = registry.addManifest(manifest.value());
            QVERIFY2(!hasErrors(errors), qPrintable(path + ": " + errors.value(0).message));
            const QString id = manifest.value().value("id").toString();
            QVERIFY2(id.startsWith("fugue/"), qPrintable(path));
            QCOMPARE(id.mid(6), QFileInfo(path).dir().dirName());
        }
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
