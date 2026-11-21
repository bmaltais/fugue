// A node on an older installed major of its type is offered the newer one (spec 3.6, #90, #118):
// the model says which version, what moving changes, moves it in one undo step, and the command
// is enabled only for such a node.
//
// Seam: CanvasModel over a registry with two majors of a type side by side; the node's role.

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

#include "canvas_app.h"
#include "canvas/canvas_model.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;

namespace {

NodeType sharpen(const char *version, bool withMask) {
    NodeType type;
    type.id = "io.example.tools/sharpen";
    type.version = *Version::parse(QString::fromLatin1(version));
    type.name = "Sharpen";
    type.runtime = QJsonObject{{"kind", "builtin"}};
    InputSpec image;
    image.name = "image";
    image.type = {*PortType::parse("image"), false};
    type.inputs.append(image);
    if (withMask) {
        InputSpec mask = image;
        mask.name = "mask";
        mask.optional = true;
        type.inputs.append(mask);
    }
    OutputSpec out;
    out.name = "image";
    out.type = {*PortType::parse("image"), false};
    type.outputs.append(out);
    return type;
}

const char *flow = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "old": {"type": "io.example.tools/sharpen", "version": "1.0.0"},
    "new": {"type": "io.example.tools/sharpen", "version": "2.0.0"},
    "text": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a fox"}}
  }},
  "layout": {"nodes": {"old": {"pos": [0, 200]}, "new": {"pos": [300, 200]}, "text": {"pos": [600, 200]}}}
})";

QVariant role(const CanvasModel &model, const QString &node, int role) {
    const NodeList *nodes = model.nodes();
    for (int row = 0; row < nodes->rowCount(); ++row)
        if (nodes->data(nodes->index(row), NodeList::NodeId).toString() == node)
            return nodes->data(nodes->index(row), role);
    return {};
}

} // namespace

class TestCanvasVersions : public QObject {
    Q_OBJECT

private slots:
    void aNodeOnAnOlderMajorIsOfferedTheNewestAndMovesInOneUndoStep() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        QVERIFY(registry.add(sharpen("1.2.0", false)).isEmpty());
        QVERIFY(registry.add(sharpen("2.1.0", true)).isEmpty());
        CanvasModel model(&registry);
        model.setDocument(Document::load(QByteArray(flow)).value());

        QCOMPARE(model.newerMajor("old"), QStringLiteral("2.1.0"));
        QCOMPARE(role(model, "old", NodeList::NewerMajor).toString(), QStringLiteral("2.1.0"));
        QCOMPARE(model.newerMajor("new"), QString()); // already the newest
        QCOMPARE(model.newerMajor("text"), QString()); // one major only
        QCOMPARE(model.newerMajor("nobody"), QString());
        QVERIFY(model.majorMove("new").isEmpty());
        QVERIFY(!model.moveToNewerMajor("new"));

        // What it changes is shown before anything moves.
        const QVariantMap changes = model.majorMove("old");
        QCOMPARE(changes.value("name").toString(), QStringLiteral("Sharpen"));
        QCOMPARE(changes.value("from").toString(), QStringLiteral("1.0.0"));
        QCOMPARE(changes.value("to").toString(), QStringLiteral("2.1.0"));
        QCOMPARE(changes.value("addedInputs").toStringList(), QStringList{"mask"});
        QVERIFY(changes.value("removedInputs").toStringList().isEmpty());
        QCOMPARE(model.document().graph.nodes.value("old").version.toString(), QStringLiteral("1.0.0"));

        // The command asks first, for the selected node only.
        QSignalSpy asked(&model, &CanvasModel::nodeMoveAsked);
        model.select({"new"});
        model.requestMajorMove();
        QCOMPARE(asked.size(), 0);
        model.select({"old"});
        model.requestMajorMove();
        QCOMPARE(asked.size(), 1);
        QCOMPARE(asked.first().at(0).toString(), QStringLiteral("old"));
        QCOMPARE(asked.first().at(1).toMap().value("to").toString(), QStringLiteral("2.1.0"));

        QVERIFY(model.moveToNewerMajor("old"));
        QCOMPARE(model.document().graph.nodes.value("old").version.toString(), QStringLiteral("2.1.0"));
        QCOMPARE(model.newerMajor("old"), QString());
        model.undo();
        QCOMPARE(model.document().graph.nodes.value("old").version.toString(), QStringLiteral("1.0.0"));
        QCOMPARE(model.newerMajor("old"), QStringLiteral("2.1.0"));
    }

    // The window: the node says so with a chip, a click on it asks what changes, and moving there
    // is the chip's Move.
    void theChipOnTheNodeAsksAndMoves() {
        canvas_test::App app(canvas_test::load(flow), [](Registry &registry) {
            registry.add(sharpen("1.2.0", false));
            registry.add(sharpen("2.1.0", true));
        });
        QVERIFY(app.open());
        app.workflows.dismissPendingTrust(); // the test pack is not trusted: its question is not under test
        QQuickItem *old = canvas_test::item(app.canvas, "node-old");
        QVERIFY(old);
        QQuickItem *chip = canvas_test::item(old, "updateChip");
        QTRY_VERIFY_WITH_TIMEOUT(chip = canvas_test::item(old, "updateChip"), 3000);
        QTRY_VERIFY(chip->isVisible());
        QVERIFY(!canvas_test::item(canvas_test::item(app.canvas, "node-new"), "updateChip")); // the newest has none
        QSignalSpy asked(&app.model, &CanvasModel::nodeMoveAsked);
        const QPointF at = chip->mapToScene(QPointF(chip->width() / 2, chip->height() / 2));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, at.toPoint());
        QCOMPARE(asked.size(), 1);
        QCOMPARE(asked.first().at(0).toString(), QStringLiteral("old"));
        // The question says what moves; its Move rewrites the version.
        QQuickItem *ok = nullptr;
        QTRY_VERIFY((ok = canvas_test::item(app.window->contentItem(), "groupUpdateOk")) && ok->isVisible());
        QCOMPARE(ok->property("text").toString(), QStringLiteral("Move"));
        QTest::mouseClick(app.window, Qt::LeftButton, {},
                          ok->mapToScene(QPointF(ok->width() / 2, ok->height() / 2)).toPoint());
        QTRY_COMPARE(app.model.document().graph.nodes.value("old").version.toString(), QStringLiteral("2.1.0"));
        QTRY_VERIFY(!canvas_test::item(old, "updateChip") || !canvas_test::item(old, "updateChip")->isVisible());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // A node whose recorded major is not installed needs review instead: no offer to move.
    void aNodeOnAMajorThatIsNotInstalledIsNotOffered() {
        Registry registry;
        QVERIFY(registry.add(sharpen("2.1.0", true)).isEmpty());
        CanvasModel model(&registry);
        model.setDocument(Document::load(QByteArray(flow)).value());
        QCOMPARE(model.newerMajor("old"), QString());
    }
};

QTEST_MAIN(TestCanvasVersions)
#include "test_canvas_versions.moc"
