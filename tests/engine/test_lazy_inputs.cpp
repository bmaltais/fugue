// Lazy inputs (spec 3.2, 9.9): a node asks, once its other inputs are in, which lazy inputs it needs, and only what
// feeds those runs. The first user is the If/Else Switch; here a fake one with the same manifest shape.
//
// Seam: runGraph() and checkGraph() on the fake kernels of scheduler_support.h, observed through which kernels were
// called and the events. Nothing sleeps.

#include <QtTest>

#include <atomic>

#include <vips/vips8>

#include "engine/vips.h"

#include "scheduler_support.h"

using namespace fugue;
using namespace fugue::test;

namespace {

// condition: a boolean from a text ("yes" is true); pick: the Switch; chain: a join of its input and a mark, to
// make a branch several nodes deep.
void addTypes(World &world) {
    world.type("fugue/flag", "'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': [{'name': 'flag', 'type': 'boolean'}]",
               [](const NodeContext &context) -> Result<Values> {
                   return Values{{"flag", Value(context.input("text").text() == QLatin1String("yes"))}};
               });
    const QByteArray pick =
        "'inputs': [{'name': 'condition', 'type': 'boolean'},"
        " {'name': 'on_true', 'type': 'any', 'optional': true, 'lazy': true},"
        " {'name': 'on_false', 'type': 'any', 'optional': true, 'lazy': true}],"
        "'outputs': [{'name': 'value', 'type': 'any', 'follows': ['on_true', 'on_false']}]";
    world.type(
        "fugue/pick", pick,
        [](const NodeContext &context) -> Result<Values> {
            const bool yes = context.input("condition").json().toBool();
            return Values{{"value", context.input(yes ? "on_true" : "on_false")}};
        },
        [](const Values &ready) { return QStringList{ready.value("condition").json().toBool() ? "on_true" : "on_false"}; });
    // The same shape with no hook: every lazy input is asked for.
    world.type("fugue/both", pick, [](const NodeContext &context) -> Result<Values> {
        return Values{{"value", context.input("on_true")}};
    });
}

// A branch of `depth` joins from text `name`: <name>1 is the text, <name>2 ... join the one before.
QByteArray chain(const QString &name, int depth) {
    QString text = QStringLiteral("'%1': {'type': 'fugue/text', 'inputs': {'text': '%1'}}").arg(name + "1");
    for (int i = 2; i <= depth; ++i)
        text += QStringLiteral(", '%1%2': {'type': 'fugue/join', 'inputs': {'a': {'from': '%1%3.text'}, 'b': '+'}}")
                    .arg(name).arg(i).arg(i - 1);
    return text.toUtf8();
}

// A Switch on the flag of `answer`, between the ends of the chains t (3 deep) and f (3 deep), into a sink.
QByteArray graphFor(const QString &answer, const QString &more = {}, const QString &type = QStringLiteral("fugue/pick")) {
    return "{'c': {'type': 'fugue/text', 'inputs': {'text': '" + answer.toUtf8() + "'}},"
           " 'flag': {'type': 'fugue/flag', 'inputs': {'text': {'from': 'c.text'}}}, " + chain("t", 3) + ", " + chain("f", 3) + ","
           " 'sw': {'type': '" + type.toUtf8() + "', 'inputs': {'condition': {'from': 'flag.flag'},"
           " 'on_true': {'from': 't3.text'}, 'on_false': {'from': 'f3.text'}}},"
           " 'out': {'type': 'fugue/save', 'inputs': {'value': {'from': 'sw.value'}}}" + (more.isEmpty() ? "" : "," + more.toUtf8()) + "}";
}

QString textOf(const Ran &ran, const QString &node, const QString &output = QStringLiteral("text")) {
    return ran.outcome.outputs.value(node).value(output).text();
}

} // namespace

class TestLazyInputs : public QObject {
    Q_OBJECT

private slots:
    // The condition is computed during the run; the side it does not choose never starts, however deep.
    void theUnchosenSideNeverRunsAndIsSkippedAsNotChosen() {
        World world;
        addTypes(world);
        const Ran ran = world.run(world.graph(graphFor("yes")));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        for (const char *node : {"t1", "t2", "t3", "sw", "out"})
            QCOMPARE(world.calls.count(node), 1);
        for (const char *node : {"f1", "f2", "f3"}) {
            QCOMPARE(world.calls.count(node), 0);
            QCOMPARE(ran.outcome.states.value(node), NodeState::Skipped);
            QCOMPARE(traceOf(ran.events, {node}), QStringList{QStringLiteral("node.skipped %1 not_chosen").arg(node)});
            QVERIFY(!ran.outcome.keys.contains(node)); // it never ran, so it has no key
        }
        QCOMPARE(textOf(ran, "sw", "value"), QStringLiteral("t1++"));
        QCOMPARE(ran.state.value("status").toString(), QStringLiteral("ok"));
    }

    void theOtherAnswerChoosesTheOtherSide() {
        World world;
        addTypes(world);
        const Ran ran = world.run(world.graph(graphFor("no")));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(world.calls.count("f3"), 1);
        QCOMPARE(world.calls.count("t1"), 0);
        QCOMPARE(ran.outcome.states.value("t3"), NodeState::Skipped);
        QCOMPARE(textOf(ran, "sw", "value"), QStringLiteral("f1++"));
    }

    // A node both sides read, or one a node that always runs reads, runs once.
    void aNodeSharedByBothSidesRunsOnce() {
        World world;
        addTypes(world);
        const QByteArray graph =
            "{'shared': {'type': 'fugue/text', 'inputs': {'text': 'shared'}},"
            " 'x': {'type': 'fugue/join', 'inputs': {'a': {'from': 'shared.text'}, 'b': 'x'}},"
            " 'y': {'type': 'fugue/join', 'inputs': {'a': {'from': 'shared.text'}, 'b': 'y'}},"
            " 'flag': {'type': 'fugue/flag', 'inputs': {'text': 'yes'}},"
            " 'sw': {'type': 'fugue/pick', 'inputs': {'condition': {'from': 'flag.flag'},"
            " 'on_true': {'from': 'x.text'}, 'on_false': {'from': 'y.text'}}},"
            " 'out': {'type': 'fugue/save', 'inputs': {'value': {'from': 'sw.value'}}}}";
        const Ran ran = world.run(world.graph(graph));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(world.calls.count("shared"), 1);
        QCOMPARE(world.calls.count("x"), 1);
        QCOMPARE(world.calls.count("y"), 0);
        QCOMPARE(ran.outcome.states.value("y"), NodeState::Skipped);
        QCOMPARE(textOf(ran, "sw", "value"), QStringLiteral("sharedx"));
    }

    // ComfyUI's output nodes: a sink is always a root, so what feeds it runs whichever side is chosen.
    void aSinkOnTheUnchosenSideStillRunsItsNodes() {
        World world;
        addTypes(world);
        const Ran ran = world.run(world.graph(graphFor("yes", "'side': {'type': 'fugue/save', 'inputs': {'value': {'from': 'f2.text'}}}")));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(world.calls.count("f1"), 1);
        QCOMPARE(world.calls.count("f2"), 1);
        QCOMPARE(world.calls.count("side"), 1);
        QCOMPARE(world.calls.count("f3"), 0); // only the node the sink does not need is left out
        QCOMPARE(ran.outcome.states.value("f3"), NodeState::Skipped);
    }

    // A rerun with the answer flipped runs only the side now chosen; the old side is not run.
    void aFlippedConditionRunsOnlyTheNewSide() {
        World world;
        addTypes(world);
        const Ran first = world.run(world.graph(graphFor("yes")));
        QCOMPARE(first.outcome.status, RunStatus::Ok);
        const int before = world.calls.total();
        const Ran flipped = world.run(world.graph(graphFor("no")));
        QCOMPARE(flipped.outcome.status, RunStatus::Ok);
        // c is another text, so flag, the switch and the sink run again; f1..f3 run; t1..t3 are not touched.
        QCOMPARE(world.calls.total() - before, 1 + 1 + 3 + 1 + 1);
        QCOMPARE(world.calls.count("t1"), 1);
        QCOMPARE(world.calls.count("f1"), 1);
        QCOMPARE(flipped.outcome.states.value("t3"), NodeState::Skipped);
        // And back: the first side is a cache hit now, the second is skipped.
        const Ran back = world.run(world.graph(graphFor("yes")));
        QCOMPARE(world.calls.count("t1"), 1);
        QCOMPARE(traceOf(back.events, {"t3", "f3"}), (QStringList{"node.skipped f3 not_chosen", "node.cached t3"}));
    }

    // The key of the Switch is its condition and the chosen value; the side not chosen is not in it.
    void theUnchosenValueIsNotInTheKey() {
        World world;
        addTypes(world);
        const Ran first = world.run(world.graph(graphFor("yes")));
        QByteArray changed = graphFor("yes");
        changed.replace("'text': 'f1'", "'text': 'different'");
        const Ran second = world.run(world.graph(changed));
        QCOMPARE(second.outcome.keys.value("sw"), first.outcome.keys.value("sw"));
        QCOMPARE(world.calls.count("sw"), 1);
    }

    // A node with lazy inputs and no hook asks for all of them.
    void aNodeWithoutAHookAsksForEveryLazyInput() {
        World world;
        addTypes(world);
        const Ran ran = world.run(world.graph(graphFor("no", {}, "fugue/both")));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        for (const char *node : {"t3", "f3"})
            QCOMPARE(world.calls.count(node), 1);
    }

    // Under validate the condition is not known: both sides are shown as they would run, never as skipped.
    void validateTakesBothSidesAsNeeded() {
        World world;
        addTypes(world);
        const CompiledGraph graph = world.graph(graphFor("yes"));
        GraphCheck check = checkGraph(graph, world.environment());
        for (const char *node : {"t1", "t3", "f1", "f3", "sw", "out"})
            QCOMPARE(check.states.value(node), NodeState::Stale);
        QCOMPARE(world.calls.total(), 0);
        world.run(graph);
        check = checkGraph(graph, world.environment());
        QCOMPARE(check.states.value("t3"), NodeState::Cached);
        QCOMPARE(check.states.value("f3"), NodeState::Stale); // not run, so still to be made if the answer flips
        // The condition is known by now, so the Switch's key is the one the run made (the side it declined is not in it): it and what
        // reads it are cached, not stale after every run.
        for (const char *node : {"flag", "sw", "out"})
            QCOMPARE(check.states.value(node), NodeState::Cached);
    }

    // A condition that fails: the Switch is skipped, and so is everything that only it could have asked for.
    void aFailedConditionEndsTheRunWithBothSidesSkipped() {
        World world;
        addTypes(world);
        world.kernel("fugue/flag", [](const NodeContext &) -> Result<Values> { return makeError("node_failed", "No flag."); });
        const Ran ran = world.run(world.graph(graphFor("yes")));
        QCOMPARE(ran.outcome.status, RunStatus::Failed);
        QCOMPARE(ran.outcome.states.value("flag"), NodeState::Error);
        for (const char *node : {"sw", "out", "t1", "f3"})
            QCOMPARE(ran.outcome.states.value(node), NodeState::Skipped);
        QCOMPARE(world.calls.count("t1") + world.calls.count("f1"), 0);
        // Nothing upstream of a side failed: it is let go, not failed; the Switch and what reads it are the ones that could not go on.
        QCOMPARE(traceOf(ran.events, {"f3", "t1"}), (QStringList{"node.skipped f3 not_chosen", "node.skipped t1 not_chosen"}));
        QCOMPARE(traceOf(ran.events, {"sw", "out"}), (QStringList{"node.skipped sw upstream_failed", "node.skipped out upstream_failed"}));
    }

    // A condition that is linked but came back empty: nothing is asked of the Switch, so no side runs (it is skipped as upstream_empty).
    void anEmptyConditionAsksForNoSide() {
        World world;
        addTypes(world);
        world.type("fugue/noflag", "'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': [{'name': 'flag', 'type': 'boolean', 'optional': true}]",
                   [](const NodeContext &) -> Result<Values> { return Values{}; });
        QByteArray graph = graphFor("yes");
        graph.replace("'type': 'fugue/flag'", "'type': 'fugue/noflag'");
        const Ran ran = world.run(world.graph(graph));
        QCOMPARE(ran.outcome.states.value("sw"), NodeState::Skipped);
        QCOMPARE(traceOf(ran.events, {"sw"}), QStringList{"node.skipped sw upstream_empty"});
        for (const char *node : {"t1", "t3", "f1", "f3"}) {
            QCOMPARE(world.calls.count(node), 0);
            QCOMPARE(traceOf(ran.events, {node}), QStringList{QStringLiteral("node.skipped %1 not_chosen").arg(node)});
        }
    }

    // A chosen side that is empty: the Switch gets no value, and its kernel says so.
    void aChosenSideThatCameBackEmptyIsLeftToTheNode() {
        World world;
        addTypes(world);
        world.type("fugue/nothing", "'inputs': [], 'outputs': [{'name': 'text', 'type': 'text', 'optional': true}]",
                   [](const NodeContext &) -> Result<Values> { return Values{}; });
        QByteArray graph = graphFor("yes");
        graph.replace("'t3': {'type': 'fugue/join', 'inputs': {'a': {'from': 't2.text'}, 'b': '+'}}",
                      "'t3': {'type': 'fugue/nothing'}");
        world.kernel("fugue/pick", [](const NodeContext &context) -> Result<Values> {
            if (context.input("on_true").isEmpty())
                return makeInputError("switch_empty", "on_true", "on_true has no value.");
            return Values{{"value", context.input("on_true")}};
        }, [](const Values &) { return QStringList{"on_true"}; });
        const Ran ran = world.run(world.graph(graph));
        QCOMPARE(ran.outcome.states.value("sw"), NodeState::Error);
        QCOMPARE(ran.outcome.errors.first().input, QStringLiteral("on_true"));
    }

    // The output of a node that follows its inputs has their type: a text input accepts a text, and a wrong type is
    // the compiler's error where it is read.
    void anAnyOutputTakesTheTypeOfWhatFeedsIt() {
        World world;
        addTypes(world);
        world.type("fugue/takes-number", "'inputs': [{'name': 'n', 'type': 'number'}], 'outputs': []",
                   [](const NodeContext &) -> Result<Values> { return Values{}; });
        const QByteArray bad =
            "{'t': {'type': 'fugue/text', 'inputs': {'text': 'a'}},"
            " 'sw': {'type': 'fugue/pick', 'inputs': {'condition': true, 'on_true': {'from': 't.text'}}},"
            " 'n': {'type': 'fugue/takes-number', 'inputs': {'n': {'from': 'sw.value'}}}}";
        QJsonObject nodes = json(bad);
        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            QJsonObject node = it.value().toObject();
            node.insert("version", "1.0.0");
            it.value() = node;
        }
        const Result<Document> document = Document::fromJson(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}});
        QVERIFY(document);
        const Compilation compiled = compile(*document, world.registry);
        QVERIFY(!compiled.ok());
        QCOMPARE(compiled.errors().first().code, QStringLiteral("type_mismatch"));
        QCOMPARE(compiled.errors().first().node, QStringLiteral("n"));
        QVERIFY(compiled.errors().first().message.contains("text"));
    }

    // 9.2: a node the Switch declined but another node reads is let go of by the Switch at once, so its decoded image
    // is released when that other node finishes, while the Switch still waits on its chosen side.
    void aDeclinedNodesImagesAreReleasedWhenTheOtherReadersFinish() {
        QVERIFY(initVips());
        World world;
        addTypes(world);
        std::atomic<bool> finalized{false};
        std::optional<bool> releasedWhenTheChosenSideRan;
        world.type("fugue/picture", "'inputs': [{'name': 'text', 'type': 'text', 'optional': true}], 'outputs': [{'name': 'image', 'type': 'image'}]",
                   [&](const NodeContext &context) -> Result<Values> {
                       if (context.node == QLatin1String("f"))
                           releasedWhenTheChosenSideRan = finalized.load(); // the chosen side runs after c finished
                       const auto made = MediaHandle::fromImage(vips::VImage::black(8, 6).cast(VIPS_FORMAT_UCHAR));
                       if (!made)
                           return made.error();
                       if (context.node == QLatin1String("p"))
                           g_object_weak_ref(G_OBJECT(made.value().image().value().get_image()),
                                             [](gpointer flag, GObject *) { static_cast<std::atomic<bool> *>(flag)->store(true); }, &finalized);
                       return Values{{"image", made.value()}};
                   });
        world.type("fugue/eye", "'inputs': [{'name': 'image', 'type': 'image'}], 'outputs': [{'name': 'text', 'type': 'text'}]",
                   [](const NodeContext &) -> Result<Values> { return Values{{"text", Value(QStringLiteral("seen"))}}; });
        // p is read by the Switch (lazily, declined) and by c; the chosen side f waits for c through d.
        const QByteArray graph =
            "{'p': {'type': 'fugue/picture'},"
            " 'c': {'type': 'fugue/eye', 'inputs': {'image': {'from': 'p.image'}}},"
            " 'd': {'type': 'fugue/join', 'inputs': {'a': {'from': 'c.text'}, 'b': 'd'}},"
            " 'e': {'type': 'fugue/join', 'inputs': {'a': {'from': 'd.text'}, 'b': 'e'}},"
            " 'f': {'type': 'fugue/picture', 'inputs': {'text': {'from': 'e.text'}}},"
            " 'flag': {'type': 'fugue/flag', 'inputs': {'text': 'no'}},"
            " 'sw': {'type': 'fugue/pick', 'inputs': {'condition': {'from': 'flag.flag'},"
            " 'on_true': {'from': 'p.image'}, 'on_false': {'from': 'f.image'}}},"
            " 'out': {'type': 'fugue/save', 'inputs': {'value': {'from': 'sw.value'}}}}";
        const Ran ran = world.run(world.graph(graph));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(world.calls.count("p"), 1);
        QVERIFY(releasedWhenTheChosenSideRan.has_value());
        QVERIFY(*releasedWhenTheChosenSideRan);
    }
};

QTEST_GUILESS_MAIN(TestLazyInputs)
#include "test_lazy_inputs.moc"
