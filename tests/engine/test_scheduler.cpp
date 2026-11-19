// The scheduler (spec 9) with the result cache behind it (10): compiled
// graphs of fake node types run through fake kernels.
//
// Seams: runGraph() and Run (the entry points a worker and the canvas use)
// and checkGraph(), observed through the RunOutcome, the events and run.json
// in the run folder, and which fake kernels were called. Providers are the
// FakeProvider. Time and seeds are fakes; nothing sleeps.

#include <QSemaphore>
#include <QWaitCondition>
#include <QtTest>

#include <sys/stat.h>

#include <vips/vips8>

#include "engine/hash.h"
#include "engine/vips.h"
#include "scheduler_support.h"

using namespace fugue;
using namespace fugue::test;

namespace {

quint64 inode(const QString &path) {
    struct stat info {};
    return ::stat(QFile::encodeName(path).constData(), &info) == 0 ? quint64(info.st_ino) : 0;
}

QJsonObject eventOf(const QList<QJsonObject> &events, const QString &name, const QString &node = {}) {
    for (const QJsonObject &event : events)
        if (event.value("event").toString() == name && (node.isEmpty() || event.value("node").toString() == node))
            return event;
    return {};
}

// a -> b -> s, with d -> t beside it.
const char *twoBranches = "{'a': {'type': 'fugue/text', 'inputs': {'text': '%1'}},"
                          " 'b': {'type': 'fugue/join', 'inputs': {'a': {'from': 'a.text'}, 'b': '%2'}},"
                          " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'b.text'}}},"
                          " 'd': {'type': 'fugue/text', 'inputs': {'text': 'beside'}},"
                          " 't': {'type': 'fugue/save', 'inputs': {'value': {'from': 'd.text'}}}}";

QByteArray branches(const QString &a, const QString &b) { return QString::fromLatin1(twoBranches).arg(a, b).toUtf8(); }

} // namespace

class TestScheduler : public QObject {
    Q_OBJECT

private slots:
    // --- running a graph (9.2, 9.4, 9.7) ---

    // A list that arrives by link has as many items as it has, which the compiler cannot
    // count: the item limits are checked when the node is about to run, as for one value (R1).
    void aLinkedListOutsideTheItemLimitsFailsTheNode() {
        World world;
        world.type("fugue/many", "'inputs': [], 'outputs': [{'name': 'items', 'type': 'text', 'list': true}]",
                   [](const NodeContext &) -> Result<Values> {
                       return Values{{"items", Value::list({Value(QString("a")), Value(QString("b")), Value(QString("c"))})}};
                   });
        const auto taker = [](const NodeContext &) -> Result<Values> { return Values{{"text", Value(QString("took"))}}; };
        world.type("fugue/two",
                   "'inputs': [{'name': 'items', 'type': 'text', 'list': true, 'max_items': 2}], 'outputs': [{'name': 'text', 'type': 'text'}]",
                   taker);
        world.type("fugue/four",
                   "'inputs': [{'name': 'items', 'type': 'text', 'list': true, 'min_items': 4}], 'outputs': [{'name': 'text', 'type': 'text'}]",
                   taker);
        world.type("fugue/three",
                   "'inputs': [{'name': 'items', 'type': 'text', 'list': true, 'min_items': 1, 'max_items': 3}], 'outputs': [{'name': 'text', 'type': 'text'}]",
                   taker);
        for (const char *type : {"fugue/two", "fugue/four", "fugue/three"}) {
            const Ran ran = world.run(world.graph(QByteArray("{'m': {'type': 'fugue/many'},"
                                                             " 't': {'type': '") + type + "', 'inputs': {'items': {'from': 'm.items'}}},"
                                                             " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 't.text'}}}}"));
            if (QByteArray(type) == "fugue/three") {
                QCOMPARE(ran.outcome.status, RunStatus::Ok);
                continue;
            }
            QCOMPARE(ran.outcome.status, RunStatus::Failed);
            QCOMPARE(ran.outcome.errors.first().code, QStringLiteral("value_out_of_range"));
            QCOMPARE(ran.outcome.errors.first().node, QStringLiteral("t"));
            QCOMPARE(ran.outcome.errors.first().input, QStringLiteral("items"));
            QVERIFY(!world.calls.all().contains("t"));
        }
    }

    void aChainRunsInDependencyOrder() {
        World world;
        world.cpuSlots = 1;
        const Ran ran = world.run(world.graph("{'a': {'type': 'fugue/text', 'inputs': {'text': 'fox'}},"
                                              " 'b': {'type': 'fugue/join', 'inputs': {'a': {'from': 'a.text'}, 'b': '!'}},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'b.text'}}}}"));

        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QVERIFY(ran.outcome.errors.isEmpty());
        QCOMPARE(trace(ran.events, true),
                 (QStringList{"run.started", "node.queued a", "node.queued b", "node.queued s", "node.running a",
                              "node.done a", "node.running b", "node.done b", "node.running s", "node.done s",
                              "run.done ok"}));
        QCOMPARE(world.calls.all(), (QStringList{"a", "b", "s"}));
        QCOMPARE(ran.outcome.ran, (QStringList{"a", "b", "s"}));
        QCOMPARE(ran.outcome.outputs.value("b").value("text").text(), QStringLiteral("fox!"));

        const QJsonObject done = eventOf(ran.events, "node.done", "b");
        QCOMPARE(done.value("outputs").toObject().value("text").toString(), QStringLiteral("fox!"));
        QCOMPARE(done.value("seconds").toDouble(), 1.0); // the fake kernel moves the clock one second
        QCOMPARE(done.value("run").toString(), QFileInfo(ran.path).fileName());
    }

    void theRunFolderHoldsTheStateAndTheCompiledGraph() {
        World world;
        const CompiledGraph graph = world.graph("{'a': {'type': 'fugue/text', 'inputs': {'text': 'fox'}},"
                                                " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}}}");
        RunOptions options;
        options.workflow = QStringLiteral("/home/me/fox.fugue");
        const Ran ran = world.run(graph, options);

        QCOMPARE(ran.state.value("status").toString(), QStringLiteral("ok"));
        QCOMPARE(ran.state.value("pid").toInteger(), QCoreApplication::applicationPid());
        QCOMPARE(ran.state.value("started").toString(), QStringLiteral("2026-11-19T12:00:00.000Z"));
        QCOMPARE(ran.state.value("ended").toString(), QStringLiteral("2026-11-19T12:00:02.000Z"));
        QCOMPARE(ran.state.value("workflow").toObject().value("path").toString(), options.workflow);
        QCOMPARE(ran.state.value("workflow").toObject().value("hash").toString(), graph.source);
        const QJsonObject a = ran.state.value("nodes").toObject().value("a").toObject();
        QCOMPARE(a.value("state").toString(), QStringLiteral("cached"));
        QCOMPARE(a.value("key").toString(), ran.outcome.keys.value("a"));
        QCOMPARE(a.value("outputs").toObject().value("text").toString(), QStringLiteral("fox"));

        QFile compiled(ran.path + "/compiled.json");
        QVERIFY(compiled.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(compiled.readAll()).object(), graph.toJson());

        // The cache knows which workflow and node used each result.
        const QList<CacheEntry> entries = world.cache->entries();
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.first().usedBy, (QList<UsedBy>{UsedBy{options.workflow, "a"}}));
    }

    // --- caching and partial reruns (10.1) ---

    void aSecondRunOfTheSameGraphRunsNothing() {
        World world;
        const CompiledGraph graph = world.graph(branches("fox", "!"));
        const Ran first = world.run(graph);
        QCOMPARE(world.calls.total(), 5);

        const Ran second = world.run(graph);
        QCOMPARE(second.outcome.status, RunStatus::Ok);
        QCOMPARE(world.calls.total(), 5);
        QVERIFY(second.outcome.ran.isEmpty());
        QCOMPARE(trace(second.events),
                 (QStringList{"run.started", "node.cached a", "node.cached b", "node.cached d", "node.cached s",
                              "node.cached t", "run.done ok"}));
        QCOMPARE(eventOf(second.events, "node.cached", "b").value("outputs").toObject().value("text").toString(),
                 QStringLiteral("fox!"));
        QCOMPARE(second.outcome.keys, first.outcome.keys);
        QCOMPARE(second.outcome.states.value("b"), NodeState::Cached);
    }

    void changingOneInputRerunsOnlyWhatIsDownstream() {
        World world;
        world.run(world.graph(branches("fox", "!")));
        const QStringList before = world.calls.all();

        // b's own literal changes: a and the other branch stay cached.
        const Ran ran = world.run(world.graph(branches("fox", "?")));
        QCOMPARE(world.calls.all().mid(before.size()), (QStringList{"b", "s"}));
        QCOMPARE(traceOf(ran.events, {"a", "d", "t"}), (QStringList{"node.cached a", "node.cached d", "node.cached t"}));
        QCOMPARE(traceOf(ran.events, {"b", "s"}),
                 (QStringList{"node.running b", "node.done b", "node.running s", "node.done s"}));
        QCOMPARE(ran.outcome.outputs.value("b").value("text").text(), QStringLiteral("fox?"));
    }

    void changingTheFirstNodeRerunsItsWholeBranch() {
        World world;
        world.run(world.graph(branches("fox", "!")));
        const Ran ran = world.run(world.graph(branches("owl", "!")));
        QCOMPARE(ran.outcome.ran, (QStringList{"a", "b", "s"}));
        QCOMPARE(traceOf(ran.events, {"d", "t"}), (QStringList{"node.cached d", "node.cached t"}));
    }

    void anUpstreamThatRerunsToTheSameResultLeavesDownstreamCached() {
        World world;
        const char *graph = "{'a': {'type': 'fugue/constant', 'inputs': {'note': '%1'}},"
                            " 'b': {'type': 'fugue/join', 'inputs': {'a': {'from': 'a.text'}}},"
                            " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'b.text'}}}}";
        world.run(world.graph(QString::fromLatin1(graph).arg("one").toUtf8()));
        const Ran ran = world.run(world.graph(QString::fromLatin1(graph).arg("two").toUtf8()));
        QCOMPARE(ran.outcome.ran, QStringList{"a"});
        QCOMPARE(traceOf(ran.events, {"b", "s"}), (QStringList{"node.cached b", "node.cached s"}));
    }

    void aNodeTypeThatIsNotDeterministicRunsInEveryRun() {
        World world;
        const CompiledGraph graph = world.graph("{'a': {'type': 'fugue/dice', 'inputs': {'text': 'roll'}},"
                                                " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}}}");
        const Ran first = world.run(graph);
        const Ran second = world.run(graph);
        QCOMPARE(world.calls.count("a"), 2);
        QVERIFY(first.outcome.keys.value("a") != second.outcome.keys.value("a"));
        // It gave the same value, so what reads it is still a hit.
        QCOMPARE(world.calls.count("s"), 1);
    }

    void aFileReadByPathIsKeyedByItsContent() {
        World world;
        const QString first = world.dir.filePath("first.txt");
        const QString copy = world.dir.filePath("copy.txt");
        auto write = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(bytes);
        };
        write(first, "words");
        write(copy, "words");
        auto graph = [&](const QString &path) {
            return world.graph(QJsonObject{
                {"a", QJsonObject{{"type", "fugue/load"}, {"inputs", QJsonObject{{"path", path}}}}},
                {"s", QJsonObject{{"type", "fugue/save"}, {"inputs", QJsonObject{{"value", QJsonObject{{"from", "a.text"}}}}}}}});
        };
        world.run(graph(first));
        QCOMPARE(world.calls.count("a"), 1);
        // The same bytes under another name: a hit.
        world.run(graph(copy));
        QCOMPARE(world.calls.count("a"), 1);
        // Other bytes under the first name: a miss.
        write(first, "other words");
        const Ran ran = world.run(graph(first));
        QCOMPARE(world.calls.count("a"), 2);
        QCOMPARE(ran.outcome.outputs.value("a").value("text").text(), QStringLiteral("other words"));
    }

    // --- a randomized seed (10.1) ---

    void aRandomizedSeedIsMadeConcreteBeforeKeying() {
        World world;
        const CompiledGraph graph = world.graph("{'a': {'type': 'fugue/seeded', 'inputs': {'seed': {'auto': 'random'}}},"
                                                " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}}}");
        const Ran first = world.run(graph);
        QCOMPARE(first.outcome.outputs.value("a").value("text").text(), QStringLiteral("seed 1000"));
        // The seed used is an output too, and run.json records it.
        QCOMPARE(first.outcome.outputs.value("a").value("seed").number(), 1000.0);
        QCOMPARE(first.state.value("auto").toObject().value("a").toObject().value("seed").toInteger(), 1000);

        // A new seed each run, so it runs again.
        const Ran second = world.run(graph);
        QCOMPARE(second.outcome.outputs.value("a").value("text").text(), QStringLiteral("seed 1001"));

        // The concrete seed written into the graph reproduces the result from the cache.
        const Ran fixed = world.run(world.graph("{'a': {'type': 'fugue/seeded', 'inputs': {'seed': 1000}},"
                                                " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}}}"));
        QCOMPARE(world.calls.count("a"), 2);
        QCOMPARE(fixed.outcome.keys.value("a"), first.outcome.keys.value("a"));
        QCOMPARE(traceOf(fixed.events, {"a"}), QStringList{"node.cached a"});
    }

    // --- errors (9.5) ---

    void anErrorStopsItsBranchAndNoOther() {
        World world;
        const Ran ran = world.run(world.graph("{'a': {'type': 'fugue/fail', 'inputs': {'text': 'x'}},"
                                              " 'b': {'type': 'fugue/join', 'inputs': {'a': {'from': 'a.text'}}},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'b.text'}}},"
                                              " 'd': {'type': 'fugue/text', 'inputs': {'text': 'beside'}},"
                                              " 't': {'type': 'fugue/save', 'inputs': {'value': {'from': 'd.text'}}}}"));

        QCOMPARE(ran.outcome.status, RunStatus::Failed);
        QCOMPARE(traceOf(ran.events, {"a", "b", "s"}),
                 (QStringList{"node.running a", "node.error a node_failed", "node.skipped b upstream_failed",
                              "node.skipped s upstream_failed"}));
        QCOMPARE(traceOf(ran.events, {"d", "t"}),
                 (QStringList{"node.running d", "node.done d", "node.running t", "node.done t"}));
        QCOMPARE(trace(ran.events).last(), QStringLiteral("run.done failed"));

        QCOMPARE(ran.outcome.errors.size(), 1);
        QCOMPARE(ran.outcome.errors.first().code, QStringLiteral("node_failed"));
        QCOMPARE(ran.outcome.errors.first().node, QStringLiteral("a"));
        QCOMPARE(ran.outcome.states.value("a"), NodeState::Error);
        QCOMPARE(ran.outcome.states.value("b"), NodeState::Skipped);
        QCOMPARE(ran.outcome.states.value("t"), NodeState::Cached);
        const QJsonArray errors = eventOf(ran.events, "run.done").value("errors").toArray();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().toObject().value("node").toString(), QStringLiteral("a"));
        QCOMPARE(ran.state.value("nodes").toObject().value("b").toObject().value("reason").toString(),
                 QStringLiteral("upstream_failed"));

        // A failed node is not cached: it is tried again next time.
        world.run(world.graph("{'a': {'type': 'fugue/fail', 'inputs': {'text': 'x'}},"
                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}}}"));
        QCOMPARE(world.calls.count("a"), 2);
    }

    void anErrorInsideAGroupNamesThePathId() {
        World world;
        const auto document = Document::fromJson(
            json("{'fugue': 1, 'graph': {'nodes': {"
                 " 'g7': {'type': 'local/broken', 'version': '1.0.0', 'inputs': {}},"
                 " 's': {'type': 'fugue/save', 'version': '1.0.0', 'inputs': {'value': {'from': 'g7.text'}}}}},"
                 " 'groups': {'local/broken': {'version': '1.0.0', 'name': 'Broken', 'inputs': [],"
                 "   'outputs': [{'name': 'text', 'from': 'inner.text'}],"
                 "   'nodes': {'inner': {'type': 'fugue/fail', 'version': '1.0.0', 'inputs': {'text': 'x'}}}}}}"));
        QVERIFY2(document.ok(), qPrintable(World::dump(document.errors())));
        const Compilation compiled = compile(*document, world.registry);
        QVERIFY2(compiled.ok(), qPrintable(World::dump(compiled.errors())));

        const Ran ran = world.run(*compiled);
        QCOMPARE(ran.outcome.errors.size(), 1);
        QCOMPARE(ran.outcome.errors.first().node, QStringLiteral("g7/inner"));
        QVERIFY(QDir(ran.path + "/nodes/g7/inner").exists());
    }

    void aNodeThatReturnsTheWrongOutputsIsAnError() {
        World world;
        world.cpuSlots = 1; // one at a time, so the errors come in a known order
        world.kernel("fugue/text", [](const NodeContext &) -> Result<Values> { return Values{}; });
        world.kernel("fugue/join", [](const NodeContext &) -> Result<Values> { return Values{{"text", 7}}; });
        world.kernel("fugue/constant", [](const NodeContext &) -> Result<Values> {
            return Values{{"text", "x"}, {"extra", "y"}};
        });
        const Ran ran = world.run(world.graph("{'a': {'type': 'fugue/text', 'inputs': {'text': 'x'}},"
                                              " 'b': {'type': 'fugue/join', 'inputs': {'a': 'x'}},"
                                              " 'c': {'type': 'fugue/constant'},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}},"
                                              " 't': {'type': 'fugue/save', 'inputs': {'value': {'from': 'b.text'}}},"
                                              " 'u': {'type': 'fugue/save', 'inputs': {'value': {'from': 'c.text'}}}}"));
        QCOMPARE(ran.outcome.errors.size(), 3);
        for (const Error &error : ran.outcome.errors)
            QCOMPARE(error.code, QStringLiteral("node_bad_output"));
        QCOMPARE(ran.outcome.errors.at(0).output, QStringLiteral("text"));
        QCOMPARE(ran.outcome.errors.at(2).output, QStringLiteral("extra"));
    }

    void anExceptionFromAKernelIsANodeError() {
        World world;
        world.kernel("fugue/text", [](const NodeContext &) -> Result<Values> { throw std::runtime_error("vips broke"); });
        const Ran ran = world.run(world.graph("{'a': {'type': 'fugue/text', 'inputs': {'text': 'x'}},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}}}"));
        QCOMPARE(ran.outcome.status, RunStatus::Failed);
        QCOMPARE(ran.outcome.errors.first().code, QStringLiteral("node_failed"));
        QCOMPARE(ran.outcome.errors.first().message, QStringLiteral("vips broke"));
    }

    void aNodeTypeWithNoKernelIsAnErrorOnThatNode() {
        World world;
        world.type("fugue/orphan", "'inputs': [], 'outputs': [{'name': 'text', 'type': 'text'}]", nullptr);
        const Ran ran = world.run(world.graph("{'a': {'type': 'fugue/orphan'},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}}}"));
        QCOMPARE(ran.outcome.errors.size(), 1);
        QCOMPARE(ran.outcome.errors.first().code, QStringLiteral("internal"));
        QCOMPARE(ran.outcome.errors.first().node, QStringLiteral("a"));
    }

    // --- empty outputs (3.3) ---

    void aRequiredInputFedByAnEmptyOutputSkipsTheNode() {
        World world;
        const Ran ran = world.run(world.graph("{'a': {'type': 'fugue/maybe', 'inputs': {'text': 'none'}},"
                                              " 'b': {'type': 'fugue/join', 'inputs': {'a': {'from': 'a.text'}}},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'b.text'}}},"
                                              " 'c': {'type': 'fugue/join', 'inputs': {'a': 'kept', 'b': {'from': 'a.text'}}},"
                                              " 't': {'type': 'fugue/save', 'inputs': {'value': {'from': 'c.text'}}}}"));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(traceOf(ran.events, {"b", "s"}),
                 (QStringList{"node.skipped b upstream_empty", "node.skipped s upstream_empty"}));
        // An optional input treats it as absent.
        QCOMPARE(ran.outcome.outputs.value("c").value("text").text(), QStringLiteral("kept"));
        QCOMPARE(ran.outcome.states.value("t"), NodeState::Cached);
    }

    void whatTheCompilerLeftOutForAMuteIsToldAsSkipped() {
        World world;
        const Ran ran = world.run(world.graph("{'a': {'type': 'fugue/text', 'mode': 'mute', 'inputs': {'text': 'x'}},"
                                              " 'b': {'type': 'fugue/join', 'inputs': {'a': {'from': 'a.text'}}},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'b.text'}}},"
                                              " 'd': {'type': 'fugue/text', 'inputs': {'text': 'beside'}},"
                                              " 't': {'type': 'fugue/save', 'inputs': {'value': {'from': 'd.text'}}}}"));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(traceOf(ran.events, {"a", "b", "s"}),
                 (QStringList{"node.skipped a muted", "node.skipped b upstream_muted", "node.skipped s upstream_muted"}));
        QCOMPARE(world.calls.all(), (QStringList{"d", "t"}));
    }

    // --- lanes (9.2) ---

    void cpuNodesRunAtTheSameTime() {
        World world;
        world.cpuSlots = 2;
        // Each kernel waits until the other is inside too.
        struct Meeting {
            QMutex mutex;
            QWaitCondition changed;
            int inside = 0;
        };
        auto meeting = std::make_shared<Meeting>();
        world.kernel("fugue/text", [meeting](const NodeContext &) -> Result<Values> {
            QMutexLocker lock(&meeting->mutex);
            ++meeting->inside;
            meeting->changed.wakeAll();
            while (meeting->inside < 2)
                if (!meeting->changed.wait(&meeting->mutex, 5000))
                    return makeError("node_failed", "The other node never ran beside this one.");
            return Values{{"text", "together"}};
        });
        const Ran ran = world.run(world.graph("{'a': {'type': 'fugue/text', 'inputs': {'text': 'x'}},"
                                              " 'b': {'type': 'fugue/text', 'inputs': {'text': 'y'}},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}},"
                                              " 't': {'type': 'fugue/save', 'inputs': {'value': {'from': 'b.text'}}}}"));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
    }

    void noMoreCpuNodesRunThanThereAreSlots() {
        World world;
        world.cpuSlots = 1;
        const Ran ran = world.run(world.graph("{'a': {'type': 'fugue/text', 'inputs': {'text': 'x'}},"
                                              " 'b': {'type': 'fugue/text', 'inputs': {'text': 'y'}},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}},"
                                              " 't': {'type': 'fugue/save', 'inputs': {'value': {'from': 'b.text'}}}}"));
        QCOMPARE(traceOf(ran.events, {"a", "b"}),
                 (QStringList{"node.running a", "node.done a", "node.running b", "node.done b"}));
    }

    void gpuNodesRunOneAtATime() {
        World world;
        world.cpuSlots = 4;
        const Ran ran = world.run(world.graph("{'a': {'type': 'fugue/gpu', 'inputs': {'text': 'x'}},"
                                              " 'b': {'type': 'fugue/gpu', 'inputs': {'text': 'y'}},"
                                              " 'c': {'type': 'fugue/gpu', 'inputs': {'text': 'z'}},"
                                              " 'p': {'type': 'fugue/text', 'inputs': {'text': 'cpu'}},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}},"
                                              " 't': {'type': 'fugue/save', 'inputs': {'value': {'from': 'b.text'}}},"
                                              " 'u': {'type': 'fugue/save', 'inputs': {'value': {'from': 'c.text'}}},"
                                              " 'v': {'type': 'fugue/save', 'inputs': {'value': {'from': 'p.text'}}}}"));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        // All four are ready at once and started in one pass: the CPU node
        // starts beside the first GPU node, the other GPU nodes wait their turn.
        QCOMPARE(traceOf(ran.events, {"a", "b", "c"}),
                 (QStringList{"node.running a", "node.done a", "node.running b", "node.done b", "node.running c",
                              "node.done c"}));
        const QStringList all = trace(ran.events);
        QVERIFY(all.indexOf("node.running p") < all.indexOf("node.done a"));
    }

    void aGpuNodeCanOptIntoRunningBesideOthers() {
        World world;
        const Ran ran = world.run(world.graph("{'a': {'type': 'fugue/gpu-parallel', 'inputs': {'text': 'x'}},"
                                              " 'b': {'type': 'fugue/gpu-parallel', 'inputs': {'text': 'y'}},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}},"
                                              " 't': {'type': 'fugue/save', 'inputs': {'value': {'from': 'b.text'}}}}"));
        QCOMPARE(trace(ran.events).mid(1, 2), (QStringList{"node.running a", "node.running b"}));
    }

    // --- cancel (9.6) ---

    void cancelSkipsWhatHasNotRunAndKeepsWhatFinished() {
        World world;
        QSemaphore entered;
        QSemaphore released;
        world.kernel("fugue/join", [&](const NodeContext &context) -> Result<Values> {
            entered.release();
            released.acquire();
            if (context.cancelled())
                return makeError("cancelled", "Stopped.");
            return Values{{"text", "finished"}};
        });
        const CompiledGraph graph = world.graph(branches("fox", "!"));
        world.cpuSlots = 1; // a, then b (which waits), so d and t have not started

        const RunFolder folder = world.folder();
        Run run(graph, folder, world.environment());
        RunOutcome outcome;
        connect(&run, &Run::finished, this, [&](const RunOutcome &o) { outcome = o; });
        run.start();
        QTRY_VERIFY(entered.available() > 0);
        run.cancel();
        released.release();
        QTRY_VERIFY(run.isFinished());

        QCOMPARE(outcome.status, RunStatus::Cancelled);
        QCOMPARE(trace(folder.events()),
                 (QStringList{"run.started", "node.running a", "node.done a", "node.running b", "node.skipped d cancelled",
                              "node.skipped s cancelled", "node.skipped t cancelled", "node.skipped b cancelled",
                              "run.done cancelled"}));
        QCOMPARE(folder.state().value("status").toString(), QStringLiteral("cancelled"));
        QCOMPARE(outcome.states.value("a"), NodeState::Cached);
        QCOMPARE(outcome.states.value("b"), NodeState::Skipped);
        QVERIFY(outcome.errors.isEmpty());

        // What finished before the cancel stays cached.
        released.release();
        const Ran again = world.run(graph);
        QCOMPARE(again.outcome.status, RunStatus::Ok);
        QCOMPARE(world.calls.count("a"), 1);
        QCOMPARE(traceOf(again.events, {"a"}), QStringList{"node.cached a"});
    }

    void aNodeThatFinishesDespiteTheCancelIsKept() {
        World world;
        QSemaphore entered;
        QSemaphore released;
        world.kernel("fugue/text", [&](const NodeContext &context) -> Result<Values> {
            entered.release();
            released.acquire();
            return Values{{"text", context.input("text")}};
        });
        const CompiledGraph graph = world.graph("{'a': {'type': 'fugue/text', 'inputs': {'text': 'x'}},"
                                                " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}}}");
        const RunFolder folder = world.folder();
        Run run(graph, folder, world.environment());
        run.start();
        QTRY_VERIFY(entered.available() > 0);
        run.cancel();
        released.release();
        QTRY_VERIFY(run.isFinished());
        QCOMPARE(trace(folder.events()),
                 (QStringList{"run.started", "node.running a", "node.skipped s cancelled", "node.done a",
                              "run.done cancelled"}));
        QVERIFY(world.cache->contains(run.outcome().keys.value("a")));
    }

    void theWorkerEntryPointHonorsItsCancelFlag() {
        World world;
        std::atomic<bool> cancel{true};
        const RunFolder folder = world.folder();
        const RunOutcome outcome = runGraph(world.graph(branches("fox", "!")), folder, world.environment(), {}, &cancel);
        QCOMPARE(outcome.status, RunStatus::Cancelled);
        QCOMPARE(world.calls.total(), 0);
        QCOMPARE(trace(folder.events()).last(), QStringLiteral("run.done cancelled"));
    }

    void destroyingARunCancelsIt() {
        World world;
        QSemaphore entered;
        std::atomic<bool> sawCancel{false};
        world.kernel("fugue/text", [&](const NodeContext &context) -> Result<Values> {
            entered.release();
            while (!context.cancelled())
                QThread::yieldCurrentThread();
            sawCancel = true;
            return makeError("cancelled", "Stopped.");
        });
        {
            Run run(world.graph("{'a': {'type': 'fugue/text', 'inputs': {'text': 'x'}},"
                                " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}}}"),
                    world.folder(), world.environment());
            run.start();
            QTRY_VERIFY(entered.available() > 0);
        }
        QVERIFY(sawCancel);
    }

    // --- progress (9.4) ---

    void aKernelsProgressBecomesEvents() {
        World world;
        world.kernel("fugue/text", [](const NodeContext &context) -> Result<Values> {
            context.progress(0.5, "sampling", "step 2 of 4");
            return Values{{"text", context.input("text")}};
        });
        const Ran ran = world.run(world.graph("{'a': {'type': 'fugue/text', 'inputs': {'text': 'x'}},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}}}"));
        const QJsonObject progress = eventOf(ran.events, "node.progress", "a");
        QCOMPARE(progress.value("value").toDouble(), 0.5);
        QCOMPARE(progress.value("stage").toString(), QStringLiteral("sampling"));
        QCOMPARE(progress.value("message").toString(), QStringLiteral("step 2 of 4"));
    }

    // --- outputs into the store (9.7) ---

    void aNodesFileGoesIntoTheStoreAndIsPassedOnFromThere() {
        World world;
        const Ran ran = world.run(world.graph("{'r': {'type': 'fugue/render', 'inputs': {'text': 'pixels'}},"
                                              " 'm': {'type': 'fugue/read', 'inputs': {'latent': {'from': 'r.latent'}}},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'm.text'}}}}"));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        // The next node read the file at the store's path.
        QCOMPARE(ran.outcome.outputs.value("m").value("text").text(), QStringLiteral("pixels"));
        const QString passed = ran.outcome.outputs.value("m").value("path").text();
        QVERIFY2(passed.startsWith(world.cache->root() + "/blobs/"), qPrintable(passed));
        // A hard link stays in the node's folder.
        const QString left = ran.path + "/nodes/r/latent.bin";
        QVERIFY(QFile::exists(left));
        QCOMPARE(inode(left), inode(passed));
        const QJsonObject latent = eventOf(ran.events, "node.done", "r").value("outputs").toObject().value("latent").toObject();
        QCOMPARE(latent.value("path").toString(), passed);
        QCOMPARE(latent.value("hash").toString(), sha256(QByteArray("pixels")));

        // The handle still works once the run folder is gone.
        QVERIFY(QDir(ran.path).removeRecursively());
        world.kernel("fugue/read", [](const NodeContext &context) -> Result<Values> {
            QFile file(context.input("latent").json().toObject().value("path").toString());
            if (!file.open(QIODevice::ReadOnly))
                return makeError("node_failed", "The cached file is gone.");
            return Values{{"text", "again " + QString::fromUtf8(file.readAll())}, {"path", file.fileName()}};
        });
        world.cache->clear({}, "m");
        const Ran again = world.run(world.graph("{'r': {'type': 'fugue/render', 'inputs': {'text': 'pixels'}},"
                                                " 'm': {'type': 'fugue/read', 'inputs': {'latent': {'from': 'r.latent'}}},"
                                                " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'm.text'}}}}"));
        QCOMPARE(again.outcome.ran, (QStringList{"m", "s"}));
        QCOMPARE(again.outcome.outputs.value("m").value("text").text(), QStringLiteral("again pixels"));
    }

    void anImageHeldInMemoryIsWrittenWhenItsNodeFinishes() {
        QVERIFY(initVips());
        World world;
        world.type("fugue/picture", "'inputs': [], 'outputs': [{'name': 'image', 'type': 'image'}]",
                   [](const NodeContext &) -> Result<Values> {
                       const auto made = MediaHandle::fromImage(vips::VImage::black(8, 6).cast(VIPS_FORMAT_UCHAR));
                       if (!made)
                           return made.error();
                       return Values{{"image", made.value()}};
                   });
        const CompiledGraph graph = world.graph("{'p': {'type': 'fugue/picture'},"
                                                " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'p.image'}}}}");
        const Ran ran = world.run(graph);
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        const MediaHandle image = ran.outcome.outputs.value("p").value("image").media();
        QCOMPARE(image.width(), 8);
        // The pixels are let go once nothing more will read them; the file stays.
        QVERIFY(!image.hasImage());
        QVERIFY(image.path().startsWith(world.cache->root() + "/blobs/"));
        QVERIFY(image.path().endsWith(".png"));
        QCOMPARE(inode(ran.path + "/nodes/p/image.png"), inode(image.path()));

        const Ran again = world.run(graph);
        QCOMPARE(world.calls.count("p"), 1);
        QCOMPARE(again.outcome.outputs.value("p").value("image").media().hash(), image.hash());
    }

    // --- eviction (10.5) ---

    void aResultEvictedForSizeRunsAgain() {
        World world(12); // room for one of the two 10-byte files
        world.cpuSlots = 1;
        const CompiledGraph graph = world.graph("{'r1': {'type': 'fugue/render', 'inputs': {'text': 'aaaaaaaaaa'}},"
                                                " 'r2': {'type': 'fugue/render', 'inputs': {'text': 'bbbbbbbbbb'}},"
                                                " 's1': {'type': 'fugue/save', 'inputs': {'value': {'from': 'r1.latent'}}},"
                                                " 's2': {'type': 'fugue/save', 'inputs': {'value': {'from': 'r2.latent'}}}}");
        const Ran first = world.run(graph);
        // The run ends by fitting the store to its limit: r1 was used least recently.
        QCOMPARE(world.cache->size(), qint64(10));
        QVERIFY(!world.cache->contains(first.outcome.keys.value("r1")));
        QVERIFY(world.cache->contains(first.outcome.keys.value("r2")));

        const Ran second = world.run(graph);
        QCOMPARE(second.outcome.ran, QStringList{"r1"});
        QCOMPARE(traceOf(second.events, {"r2", "s1", "s2"}),
                 (QStringList{"node.cached r2", "node.cached s2", "node.cached s1"}));
    }

    // --- freeze (10.4) ---

    void aFrozenNodeKeepsItsResultWhateverChangesUpstream() {
        World world;
        const Ran first = world.run(world.graph(branches("fox", "!")));
        const QString key = first.outcome.keys.value("b");
        QVERIFY(world.cache->freeze(key));

        QJsonObject nodes = json(branches("owl", "?"));
        QJsonObject b = nodes.value("b").toObject();
        b.insert("frozen", key);
        nodes.insert("b", b);
        const Ran ran = world.run(world.graph(nodes));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(ran.outcome.states.value("b"), NodeState::Frozen);
        QCOMPARE(ran.outcome.outputs.value("b").value("text").text(), QStringLiteral("fox!"));
        QCOMPARE(world.calls.count("b"), 1);
        // a changed and ran; s reads the frozen value, which it has seen before.
        QCOMPARE(ran.outcome.ran, QStringList{"a"});
        QCOMPARE(traceOf(ran.events, {"b", "s"}), (QStringList{"node.cached b", "node.cached s"}));
        QCOMPARE(ran.state.value("nodes").toObject().value("b").toObject().value("state").toString(),
                 QStringLiteral("frozen"));
    }

    // 10.4: "never invalidated by upstream changes", a failure upstream included.
    void aFrozenNodeServesItsResultEvenWhenItsUpstreamFails() {
        World world;
        const Ran first = world.run(world.graph(branches("fox", "!")));
        const QString key = first.outcome.keys.value("b");
        QVERIFY(world.cache->freeze(key));

        QJsonObject nodes = json(branches("owl", "!"));
        QJsonObject b = nodes.value("b").toObject();
        b.insert("frozen", key);
        nodes.insert("b", b);
        world.kernel("fugue/text", [](const NodeContext &) -> Result<Values> {
            return makeError("node_failed", "The upstream node broke.");
        });
        const Ran ran = world.run(world.graph(nodes));
        QCOMPARE(ran.outcome.states.value("a"), NodeState::Error);
        QCOMPARE(ran.outcome.states.value("b"), NodeState::Frozen);
        QCOMPARE(ran.outcome.outputs.value("b").value("text").text(), QStringLiteral("fox!"));
        QCOMPARE(traceOf(ran.events, {"b", "s"}), (QStringList{"node.cached b", "node.cached s"}));
        QCOMPARE(ran.outcome.status, RunStatus::Failed); // a's error is still the run's
    }

    void warningsGivenWithTheRunAreReportedAndDoNotFailIt() {
        World world;
        RunOptions options;
        options.warnings = {makeError("unknown_provider", "Cloud has a provider type this Fugue does not have.")};
        const Ran ran = world.run(world.graph("{'a': {'type': 'fugue/text', 'inputs': {'text': 'fox'}},"
                                              " 's': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.text'}}}}"),
                                  options);
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(ran.outcome.errors.size(), 1);
        QCOMPARE(ran.outcome.errors.first().severity, Error::Severity::Warning);
        const QJsonArray told = ran.events.last().value("errors").toArray();
        QCOMPARE(told.size(), 1);
        QCOMPARE(told.at(0).toObject().value("code").toString(), QStringLiteral("unknown_provider"));
        QCOMPARE(told.at(0).toObject().value("severity").toString(), QStringLiteral("warning"));
    }

    void aMissingFrozenResultWarnsAndTheNodeRuns() {
        World world;
        QJsonObject nodes = json(branches("fox", "!"));
        QJsonObject b = nodes.value("b").toObject();
        b.insert("frozen", sha256(QByteArray("gone")));
        nodes.insert("b", b);
        const Ran ran = world.run(world.graph(nodes));

        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(ran.outcome.states.value("b"), NodeState::Cached);
        QCOMPARE(world.calls.count("b"), 1);
        QCOMPARE(ran.outcome.errors.size(), 1);
        QCOMPARE(ran.outcome.errors.first().code, QStringLiteral("frozen_result_missing"));
        QCOMPARE(ran.outcome.errors.first().severity, Error::Severity::Warning);
        QCOMPARE(ran.outcome.errors.first().node, QStringLiteral("b"));
        const QJsonArray reported = eventOf(ran.events, "run.done").value("errors").toArray();
        QCOMPARE(reported.first().toObject().value("severity").toString(), QStringLiteral("warning"));
    }
};

QTEST_GUILESS_MAIN(TestScheduler)
#include "test_scheduler.moc"
