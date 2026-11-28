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
        " {'name': 'on_true', 'type': 'any', 'optional': true, 'lazy': true, 'takes_list': true},"
        " {'name': 'on_false', 'type': 'any', 'optional': true, 'lazy': true, 'takes_list': true}],"
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


// What the compiler says about a graph written with single quotes; the nodes get version 1.0.0.
Compilation compileOf(World &world, const QByteArray &graph) {
    QJsonObject nodes = json(graph);
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        QJsonObject node = it.value().toObject();
        node.insert("version", "1.0.0");
        it.value() = node;
    }
    const Result<Document> document = Document::fromJson(QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}});
    if (!document)
        qFatal("test document is invalid");
    return compile(*document, world.registry);
}

// A text list source, and sinks that take a list of text or a single text.
void addListTypes(World &world) {
    world.type("fugue/names", "'inputs': [], 'outputs': [{'name': 'items', 'type': 'text', 'list': true}]",
               [](const NodeContext &) -> Result<Values> { return Values{{"items", Value::list({Value("a"), Value("b")})}}; });
    world.type("fugue/takes-list", "'sink': true, 'inputs': [{'name': 'items', 'type': 'text', 'list': true}], 'outputs': []",
               [](const NodeContext &) -> Result<Values> { return Values{}; });
    world.type("fugue/takes-text", "'sink': true, 'inputs': [{'name': 'text', 'type': 'text'}], 'outputs': []",
               [](const NodeContext &) -> Result<Values> { return Values{}; });
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

    // Run from here on a group's card (3.4): every node inside it is demanded, so one the Switch after it declines runs too.
    void aGroupCardFromDemandsTheNodesInsideIt() {
        World world;
        addTypes(world);
        CompiledGraph graph = world.graph(graphFor("yes"));
        graph.nodes.insert("g/f3", graph.nodes.take("f3"));
        graph.nodes["sw"].inputs.insert("on_false", InputValue::link({"g/f3", "text"}));
        graph.from = "g";
        const Ran ran = world.run(graph);
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(world.calls.count("g/f3"), 1);
        QCOMPARE(textOf(ran, "sw", "value"), QStringLiteral("t1++")); // the Switch still takes its chosen side
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

    // A mute is the person's choice (#590): a muted source empties the lazy input it feeds, and only a Switch that picks that
    // side is skipped as upstream_muted. The other side runs as if nothing were muted.
    void aMutedUnchosenSideLeavesTheNodeRunning() {
        World world;
        addTypes(world);
        QByteArray graph = graphFor("yes");
        graph.replace("'f3': {'type': 'fugue/join',", "'f3': {'type': 'fugue/join', 'mode': 'mute',");
        const Ran ran = world.run(world.graph(graph));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(ran.outcome.states.value("sw"), NodeState::Cached);
        QCOMPARE(textOf(ran, "sw", "value"), QStringLiteral("t1++"));
    }

    void aMutedChosenSideSkipsTheNodeAsUpstreamMuted() {
        World world;
        addTypes(world);
        QByteArray graph = graphFor("no");
        graph.replace("'f3': {'type': 'fugue/join',", "'f3': {'type': 'fugue/join', 'mode': 'mute',");
        const Ran ran = world.run(world.graph(graph));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(traceOf(ran.events, {"sw"}), QStringList{QStringLiteral("node.skipped sw upstream_muted")});
        QCOMPARE(world.calls.count("t1"), 0); // the other side is let go, not run
        QCOMPARE(world.calls.count("out"), 0);
    }

    // Spec 3.5: what reads a Switch skipped for its muted chosen side is skipped upstream_muted too, as a compile-time mute does,
    // through a required input (the Save) and through an optional one (a Join's b), which would otherwise run without it.
    void aReaderOfASwitchSkippedForAMutedSideIsSkippedAsUpstreamMuted() {
        World world;
        addTypes(world);
        QByteArray graph = graphFor("no", "'j': {'type': 'fugue/join', 'inputs': {'a': 'x', 'b': {'from': 'sw.value'}}},"
                                       " 'out2': {'type': 'fugue/save', 'inputs': {'value': {'from': 'j.text'}}}");
        graph.replace("'f3': {'type': 'fugue/join',", "'f3': {'type': 'fugue/join', 'mode': 'mute',");
        const Ran ran = world.run(world.graph(graph));
        QCOMPARE(traceOf(ran.events, {"sw"}), QStringList{QStringLiteral("node.skipped sw upstream_muted")});
        QCOMPARE(traceOf(ran.events, {"out"}), QStringList{QStringLiteral("node.skipped out upstream_muted")});
        QCOMPARE(traceOf(ran.events, {"j"}), QStringList{QStringLiteral("node.skipped j upstream_muted")});
        QCOMPARE(world.calls.count("j"), 0);
    }

    // With no hook every lazy input is asked for, so the muted one skips the node.
    void aNodeWithoutAHookAsksForEveryLazyInputSoAMutedOneSkipsIt() {
        World world;
        addTypes(world);
        QByteArray graph = graphFor("yes");
        graph.replace("'f3': {'type': 'fugue/join',", "'f3': {'type': 'fugue/join', 'mode': 'mute',");
        graph.replace("fugue/pick", "fugue/both"); // asks for every lazy input: the muted one included
        const Ran ran = world.run(world.graph(graph));
        QCOMPARE(traceOf(ran.events, {"sw"}), QStringList{QStringLiteral("node.skipped sw upstream_muted")});
    }

    // #597 review: a Switch that chooses another Switch skipped as upstream_muted is skipped the same way, not failed with
    // switch_empty; the declined side of the first one is let go and not looked at.
    void aSwitchThatChoosesAMutedSwitchIsSkippedAsUpstreamMuted() {
        World world;
        addTypes(world);
        QByteArray graph = graphFor("no", "'sw2': {'type': 'fugue/pick', 'inputs': {'condition': true, 'on_true': {'from': 'sw.value'}}},"
                                          " 'out2': {'type': 'fugue/save', 'inputs': {'value': {'from': 'sw2.value'}}}");
        graph.replace("'f3': {'type': 'fugue/join',", "'f3': {'type': 'fugue/join', 'mode': 'mute',");
        const Ran ran = world.run(world.graph(graph));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(traceOf(ran.events, {"sw"}), QStringList{QStringLiteral("node.skipped sw upstream_muted")});
        QCOMPARE(traceOf(ran.events, {"sw2"}), QStringList{QStringLiteral("node.skipped sw2 upstream_muted")});
        QCOMPARE(traceOf(ran.events, {"out2"}), QStringList{QStringLiteral("node.skipped out2 upstream_muted")});
    }

    // Spec 10.4: a valid frozen result is served whatever the inputs are now, a Switch's chosen side newly muted included.
    void aFrozenSwitchIsServedWhenItsChosenSideIsMuted() {
        World world;
        addTypes(world);
        const Ran first = world.run(world.graph(graphFor("yes")));
        const QString key = first.outcome.keys.value("sw");
        QVERIFY(!key.isEmpty());
        QVERIFY(world.cache->freeze(key));
        QJsonObject nodes = json(graphFor("no").replace("'f3': {'type': 'fugue/join',", "'f3': {'type': 'fugue/join', 'mode': 'mute',"));
        QJsonObject sw = nodes.value("sw").toObject();
        sw.insert("frozen", key);
        nodes.insert("sw", sw);
        const Ran ran = world.run(world.graph(nodes));
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(ran.outcome.states.value("sw"), NodeState::Frozen);
        QCOMPARE(textOf(ran, "sw", "value"), QStringLiteral("t1++"));
    }

    // The check reads what a run would read: a side the answer declines is not looked at, so a muted one does not mute the Switch.
    void theCheckDoesNotMuteASwitchForItsDeclinedSide() {
        World world;
        addTypes(world);
        QByteArray graph = graphFor("yes");
        graph.replace("'f3': {'type': 'fugue/join',", "'f3': {'type': 'fugue/join', 'mode': 'mute',");
        const CompiledGraph compiled = world.graph(graph);
        world.run(compiled); // the condition is known now
        const GraphCheck check = checkGraph(compiled, world.environment());
        QCOMPARE(check.states.value("sw"), NodeState::Cached);
        QCOMPARE(check.states.value("out"), NodeState::Cached);
    }

    // A declined side that is itself a Switch skipped for its muted chosen side does not mute the Switch that declined it.
    void theCheckDoesNotMuteASwitchThatDeclinedAMutedSwitch() {
        World world;
        addTypes(world);
        const QByteArray graph =
            "{'m': {'type': 'fugue/join', 'mode': 'mute', 'inputs': {'a': 'x', 'b': '+'}},"
            " 'b': {'type': 'fugue/pick', 'inputs': {'condition': false, 'on_false': {'from': 'm.text'}}},"
            " 'c': {'type': 'fugue/text', 'inputs': {'text': 'c'}},"
            " 'a': {'type': 'fugue/pick', 'inputs': {'condition': true, 'on_true': {'from': 'c.text'}, 'on_false': {'from': 'b.value'}}},"
            " 'out': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.value'}}}}";
        const CompiledGraph compiled = world.graph(graph);
        const GraphCheck check = checkGraph(compiled, world.environment());
        QVERIFY(check.states.value("a") != NodeState::Skipped);
        QVERIFY(check.states.value("out") != NodeState::Skipped);
        const Ran ran = world.run(compiled);
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(ran.outcome.states.value("a"), NodeState::Cached);
    }

    // The check agrees with a run on a frozen Switch whose chosen side is muted: its frozen result stands.
    void theCheckShowsAFrozenSwitchWithAMutedSideAsFrozen() {
        World world;
        addTypes(world);
        const Ran first = world.run(world.graph(graphFor("yes")));
        const QString key = first.outcome.keys.value("sw");
        QVERIFY(world.cache->freeze(key));
        QJsonObject nodes = json(graphFor("no").replace("'f3': {'type': 'fugue/join',", "'f3': {'type': 'fugue/join', 'mode': 'mute',"));
        QJsonObject sw = nodes.value("sw").toObject();
        sw.insert("frozen", key);
        nodes.insert("sw", sw);
        const CompiledGraph compiled = world.graph(nodes);
        world.run(compiled); // the condition is known now
        const GraphCheck check = checkGraph(compiled, world.environment());
        QCOMPARE(check.states.value("sw"), NodeState::Frozen);
    }

    // A linked empty list is the value on an input that takes a list as it is, so a Switch passes it on (not switch_empty).
    void aSwitchPassesAnEmptyListThrough() {
        World world;
        addTypes(world);
        addListTypes(world);
        world.type("fugue/no-names", "'inputs': [], 'outputs': [{'name': 'items', 'type': 'text', 'list': true}]",
                   [](const NodeContext &) -> Result<Values> { return Values{{"items", Value::list({})}}; });
        const QByteArray graph =
            "{'n': {'type': 'fugue/no-names'}, 'sw': {'type': 'fugue/pick', 'inputs': {'condition': true, 'on_true': {'from': 'n.items'}}},"
            " 'out': {'type': 'fugue/takes-list', 'inputs': {'items': {'from': 'sw.value'}}}}";
        const Compilation compiled = compileOf(world, graph);
        QVERIFY2(compiled.ok(), qPrintable(compiled.errors().isEmpty() ? QString() : compiled.errors().first().message));
        const Ran ran = world.run(*compiled);
        QVERIFY2(ran.outcome.status == RunStatus::Ok, qPrintable(ran.outcome.errors.isEmpty() ? QString() : ran.outcome.errors.first().message));
        QCOMPARE(ran.outcome.states.value("sw"), NodeState::Cached);
        QVERIFY(ran.outcome.outputs.value("sw").value("value").isList());
        QCOMPARE(ran.outcome.outputs.value("sw").value("value").items().size(), 0);
    }

    // #592: a Switch switches lists. Its output is the list type, so a list input takes it and a single-value input does not.
    void aSwitchPassesAListThroughAsAList() {
        World world;
        addTypes(world);
        addListTypes(world);
        const QByteArray graph =
            "{'n': {'type': 'fugue/names'}, 'sw': {'type': 'fugue/pick', 'inputs': {'condition': true, 'on_true': {'from': 'n.items'}}},"
            " 'out': {'type': 'fugue/takes-list', 'inputs': {'items': {'from': 'sw.value'}}}}";
        const Compilation compiled = compileOf(world, graph);
        QVERIFY2(compiled.ok(), qPrintable(compiled.errors().isEmpty() ? QString() : compiled.errors().first().message));
        const Ran ran = world.run(*compiled);
        QVERIFY2(ran.outcome.status == RunStatus::Ok, qPrintable(ran.outcome.errors.isEmpty() ? QString() : ran.outcome.errors.first().message));
        QCOMPARE(ran.outcome.outputs.value("sw").value("value").items().size(), 2);
    }

    void aSwitchOfListsIsNotASingleValue() {
        World world;
        addTypes(world);
        addListTypes(world);
        const Compilation compiled = compileOf(
            world, "{'n': {'type': 'fugue/names'}, 'sw': {'type': 'fugue/pick', 'inputs': {'condition': true, 'on_true': {'from': 'n.items'}}},"
                   " 'out': {'type': 'fugue/takes-text', 'inputs': {'text': {'from': 'sw.value'}}}}");
        QVERIFY(!compiled.ok());
        QCOMPARE(compiled.errors().first().code, QStringLiteral("type_mismatch"));
        QCOMPARE(compiled.errors().first().node, QStringLiteral("out"));
    }

    void aListOnOneSideAndASingleValueOnTheOtherIsATypeMismatch() {
        World world;
        addTypes(world);
        addListTypes(world);
        const Compilation compiled = compileOf(
            world, "{'n': {'type': 'fugue/names'}, 't': {'type': 'fugue/text', 'inputs': {'text': 'x'}},"
                   " 'sw': {'type': 'fugue/pick', 'inputs': {'condition': true, 'on_true': {'from': 'n.items'}, 'on_false': {'from': 't.text'}}},"
                   " 'out': {'type': 'fugue/takes-list', 'inputs': {'items': {'from': 'sw.value'}}}}");
        QVERIFY(!compiled.ok());
        QCOMPARE(compiled.errors().first().code, QStringLiteral("type_mismatch"));
        QVERIFY(compiled.errors().first().message.contains("differ"));
    }

    // Two Switches that take each other's value follow forever: the compiler reports a cycle instead of overflowing the stack.
    void twoSwitchesThatFollowEachOtherAreACycle() {
        World world;
        addTypes(world);
        const Compilation compiled = compileOf(
            world, "{'a': {'type': 'fugue/pick', 'inputs': {'condition': true, 'on_true': {'from': 'b.value'}}},"
                   " 'b': {'type': 'fugue/pick', 'inputs': {'condition': true, 'on_true': {'from': 'a.value'}}},"
                   " 'out': {'type': 'fugue/save', 'inputs': {'value': {'from': 'a.value'}}}}");
        QVERIFY(!compiled.ok());
        bool cycle = false;
        for (const Error &error : compiled.errors())
            cycle = cycle || error.code == QLatin1String("cycle");
        QVERIFY(cycle);
    }

    // #592 (b): only the CLI or hand-edited JSON can write a literal on an any input (the canvas has no widget for one), and
    // it types nothing, so it is a compile error on the node that holds it rather than an unchecked value.
    void aLiteralOnASwitchSideIsACompileError() {
        World world;
        addTypes(world);
        const Compilation compiled = compileOf(
            world, "{'t': {'type': 'fugue/text', 'inputs': {'text': 'x'}},"
                   " 'sw': {'type': 'fugue/pick', 'inputs': {'condition': true, 'on_true': {'from': 't.text'}, 'on_false': 5}},"
                   " 'out': {'type': 'fugue/save', 'inputs': {'value': {'from': 'sw.value'}}}}");
        QVERIFY(!compiled.ok());
        QCOMPARE(compiled.errors().first().code, QStringLiteral("type_mismatch"));
        QCOMPARE(compiled.errors().first().node, QStringLiteral("sw"));
        QCOMPARE(compiled.errors().first().input, QStringLiteral("on_false"));
    }

    // The same error for a side a group exposes is placed on the group instance and its exposed input, as link errors are.
    void aLiteralOnAnExposedSwitchSideIsPlacedOnTheGroupInstance() {
        World world;
        addTypes(world);
        const QJsonObject doc = json(
            "{'fugue': 1, 'graph': {'nodes': {"
            " 't': {'type': 'fugue/text', 'version': '1.0.0', 'inputs': {'text': 'x'}},"
            " 'g': {'type': 'local/sided', 'version': '1.0.0', 'inputs': {'side': 5, 'other': {'from': 't.text'}}},"
            " 'out': {'type': 'fugue/save', 'version': '1.0.0', 'inputs': {'value': {'from': 'g.value'}}}}},"
            " 'groups': {'local/sided': {'version': '1.0.0', 'name': 'Sided',"
            "  'inputs': [{'name': 'side', 'targets': ['sw.on_false']}, {'name': 'other', 'targets': ['sw.on_true']}],"
            "  'outputs': [{'name': 'value', 'from': 'sw.value'}],"
            "  'nodes': {'sw': {'type': 'fugue/pick', 'version': '1.0.0', 'inputs': {'condition': true}}}}}}");
        const Result<Document> document = Document::fromJson(doc);
        QVERIFY(document.ok());
        const Compilation compiled = compile(*document, world.registry);
        QVERIFY(!compiled.ok());
        QCOMPARE(compiled.errors().first().code, QStringLiteral("type_mismatch"));
        QCOMPARE(compiled.errors().first().node, QStringLiteral("g"));
        QCOMPARE(compiled.errors().first().input, QStringLiteral("side"));
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
