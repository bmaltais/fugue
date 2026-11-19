// What a kernel is told about the run beyond its inputs (NodeContext::document,
// ::used, ::registry): the workflow document the run was started from, and the values
// used upstream (a randomized input made concrete, a provider node's engine and model).
// Seam: runGraph()/Run with a fake kernel that records its context, as the scheduler
// tests do.

#include <QtTest>

#include "scheduler_support.h"

using namespace fugue;
using namespace fugue::test;

namespace {

QJsonObject node(const QString &type, const QJsonObject &inputs) {
    return QJsonObject{{"type", type}, {"inputs", inputs}};
}
QJsonObject from(const QString &port) { return QJsonObject{{"from", port}}; }

struct Seen {
    QJsonObject document;
    QJsonObject used;
    const Registry *registry = nullptr;
};

} // namespace

class TestRunContext : public QObject {
    Q_OBJECT

private slots:
    void aKernelSeesTheDocumentTheRegistryAndTheSeedsUsedUpstream() {
        World world;
        auto seen = std::make_shared<Seen>();
        world.type("fugue/spy", "'sink': true, 'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': []",
                   [seen](const NodeContext &context) -> Result<Values> {
                       seen->document = context.document;
                       seen->used = context.usedUpstream;
                       seen->registry = context.registry;
                       return Values{};
                   });
        const QJsonObject nodes{
            {"a", node("fugue/seeded", {{"seed", QJsonObject{{"auto", "random"}}}})},
            {"b", node("fugue/seeded", {{"seed", 7}})},
            {"s", node("fugue/spy", {{"text", from("a.text")}})},
        };
        RunOptions options;
        options.document = json("{'fugue': 1, 'marker': 'the document'}");
        world.run(world.graph(nodes), options);

        QCOMPARE(seen->document.value("marker").toString(), QString("the document"));
        QCOMPARE(seen->registry, &world.registry);
        // The randomized seed of the node upstream, made concrete; nothing for a node that is not
        // upstream or that had no randomized input.
        QCOMPARE(seen->used.value("a").toObject().value("auto").toObject().value("seed").toInteger(), 1000);
        QVERIFY(!seen->used.contains("b"));
        QVERIFY(!seen->used.contains("s"));
    }

    void aKernelSeesTheEngineAndModelOfAnUpstreamProviderNode() {
        World world;
        const auto provider = world.provider("This machine", {"flux"});
        auto seen = std::make_shared<Seen>();
        world.type("fugue/spy", "'sink': true, 'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': []",
                   [seen](const NodeContext &context) -> Result<Values> {
                       seen->used = context.usedUpstream;
                       return Values{};
                   });
        const CompiledGraph graph = world.graph(QJsonObject{
            {"g", node("fugue/generate", {{"prompt", "a cat"}})},
            {"s", node("fugue/spy", {{"text", from("g.text")}})},
        });
        const RunFolder folder = world.folder();
        Run run(graph, folder, world.environment());
        run.start();
        QTRY_COMPARE(provider->jobs().size(), 1);
        provider->finish(provider->jobs().first(), {{"result.txt", "a picture"}});
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(run.outcome().status, RunStatus::Ok);

        const QJsonObject used = seen->used.value("g").toObject();
        QCOMPARE(used.value("engine").toString(), QString("fake"));
        QCOMPARE(used.value("model").toObject().value("name").toString(), QString("flux"));
        QVERIFY(!used.contains("instance")); // a local name, not part of what was used
    }

    void aRunWithoutADocumentGivesAnEmptyOne() {
        World world;
        auto seen = std::make_shared<Seen>();
        seen->document = json("{'x': 1}");
        world.type("fugue/spy", "'sink': true, 'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': []",
                   [seen](const NodeContext &context) -> Result<Values> {
                       seen->document = context.document;
                       return Values{};
                   });
        world.run(world.graph(QJsonObject{{"t", node("fugue/text", {{"text", "x"}})},
                                          {"s", node("fugue/spy", {{"text", from("t.text")}})}}));
        QVERIFY(seen->document.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestRunContext)
#include "test_run_context.moc"
