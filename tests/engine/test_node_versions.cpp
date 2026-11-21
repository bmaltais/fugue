// Moving a node to a newer major of its type (spec 3.6, #118): what changes between two
// versions' inputs and outputs, and when a node is offered a newer major.

#include <QtTest>

#include "engine/document.h"
#include "engine/node_type.h"
#include "engine/node_versions.h"
#include "engine/registry.h"
#include "engine/version.h"

using namespace fugue;

namespace {

PortSpec port(const char *type) { return PortSpec{*PortType::parse(QString::fromLatin1(type)), false}; }

NodeType type(const char *version, QList<std::pair<QString, const char *>> inputs, QList<std::pair<QString, const char *>> outputs) {
    NodeType out;
    out.id = "io.example.tools/sharpen";
    out.version = *Version::parse(QString::fromLatin1(version));
    out.name = "Sharpen";
    out.runtime = QJsonObject{{"kind", "builtin"}};
    for (const auto &[name, kind] : inputs) {
        InputSpec spec;
        spec.name = name;
        spec.type = port(kind);
        out.inputs.append(spec);
    }
    for (const auto &[name, kind] : outputs) {
        OutputSpec spec;
        spec.name = name;
        spec.type = port(kind);
        out.outputs.append(spec);
    }
    return out;
}

Node nodeAt(const char *version) {
    Node node;
    node.type = "io.example.tools/sharpen";
    node.version = *Version::parse(QString::fromLatin1(version));
    return node;
}

} // namespace

class TestNodeVersions : public QObject {
    Q_OBJECT

private slots:
    void portChangesNameWhatComesGoesAndIsRetyped() {
        const NodeType one = type("1.0.0", {{"image", "image"}, {"amount", "number"}}, {{"image", "image"}});
        const NodeType two = type("2.0.0", {{"image", "image"}, {"amount", "text"}, {"mask", "image"}},
                                  {{"result", "image"}});
        const PortChanges changes = portChanges(one, two);
        QCOMPARE(changes.addedInputs, (QStringList{"amount", "mask"})); // retyped is out, and in again
        QCOMPARE(changes.removedInputs, QStringList{"amount"});
        QCOMPARE(changes.addedOutputs, QStringList{"result"});
        QCOMPARE(changes.removedOutputs, QStringList{"image"});
        QVERIFY(changes.any());
        QVERIFY(!portChanges(one, one).any());
    }

    void aNodeIsOfferedANewerMajorOnlyWhileItsOwnMajorIsInstalled() {
        Registry registry;
        QVERIFY(registry.add(type("1.2.0", {{"image", "image"}}, {{"image", "image"}})).isEmpty());
        QVERIFY(!newerMajorFor(registry, nodeAt("1.0.0"))); // the newest already
        QVERIFY(registry.add(type("2.1.0", {{"image", "image"}}, {{"image", "image"}})).isEmpty());
        QCOMPARE(newerMajorFor(registry, nodeAt("1.0.0")), std::make_optional(*Version::parse("2.1.0")));
        QVERIFY(!newerMajorFor(registry, nodeAt("2.0.0")));
        // A major that is not installed needs review instead (node_version_mismatch): no offer.
        QVERIFY(!newerMajorFor(registry, nodeAt("0.9.0")));
        Node other = nodeAt("1.0.0");
        other.type = "io.example.tools/none";
        QVERIFY(!newerMajorFor(registry, other));
    }
};

QTEST_MAIN(TestNodeVersions)
#include "test_node_versions.moc"
