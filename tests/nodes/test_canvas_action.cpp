// Canvas actions (#231, docs/canvas-actions-design.md 1): the `action` block parsed, checked against the graph with the
// built-in node types, and bound into the run's copy of the workflow. Pure: no window, no store, no run. The bundled
// Generative fill action (share/actions/generative-fill.fugue) is checked and bound here too.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

#include "engine/canvas_action.h"
#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;
using namespace fugue::canvas;

namespace {

const QString kBundled = QStringLiteral(FUGUE_SOURCE_DIR "/share/actions/generative-fill.fugue");

// A small fill: a Region, a Text Encode and a Sampler (stand-ins for the model chain), a Write.
QJsonObject fillWorkflow() {
    return QJsonDocument::fromJson(R"({
      "fugue": 1,
      "action": {"format": 1, "name": "Fill", "glyph": "sparkles", "selection": "required", "region": "region",
                 "inputs": [{"role": "prompt", "targets": ["encode.prompt"], "label": "Prompt"},
                            {"role": "seed", "targets": ["sample.seed"]},
                            {"role": "strength", "targets": ["sample.denoise"], "default": 1, "min": 0.05, "max": 1, "section": "Advanced"},
                            {"name": "steps", "targets": ["sample.steps"], "label": "Steps", "section": "Advanced"}]},
      "graph": {"nodes": {
        "region": {"type": "fugue/canvas-region", "version": "1.2.0",
                   "inputs": {"canvas": "", "revision": "head", "x": 0, "y": 0, "width": 64, "height": 64, "padding": 32}},
        "encode": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "old words", "references": [{"from": "region.image"}]}},
        "sample": {"type": "fugue/sampler", "version": "1.2.0", "inputs": {"conditioning": {"from": "encode.conditioning"}, "seed": {"auto": "random"}, "steps": 4}},
        "write": {"type": "fugue/canvas-write", "version": "1.3.0",
                  "inputs": {"canvas": {"from": "region.canvas"}, "image": {"from": "region.image"}, "stitch": {"from": "region.stitch"}}}
      }}
    })").object();
}

Document load(const QJsonObject &json) {
    const Result<Document> loaded = Document::fromJson(json);
    if (!loaded)
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

QJsonObject withAction(QJsonObject workflow, const std::function<void(QJsonObject &)> &change) {
    QJsonObject action = workflow.value("action").toObject();
    change(action);
    workflow.insert("action", action);
    return workflow;
}

QJsonObject withNode(QJsonObject workflow, const QString &id, const QJsonObject &node) {
    QJsonObject graph = workflow.value("graph").toObject();
    QJsonObject nodes = graph.value("nodes").toObject();
    if (node.isEmpty())
        nodes.remove(id);
    else
        nodes.insert(id, node);
    graph.insert("nodes", nodes);
    workflow.insert("graph", graph);
    return workflow;
}

QString firstMessage(const Errors &errors) { return errors.isEmpty() ? QString() : errors.first().message; }

} // namespace

class TestCanvasAction : public QObject {
    Q_OBJECT
    Registry m_registry;

    Result<ActionGraph> check(const QJsonObject &workflow) {
        const Document document = load(workflow);
        const Result<std::optional<Action>> action = actionOf(document);
        if (!action)
            return action.errors();
        return checkAction(**action, document, m_registry);
    }

private slots:
    void initTestCase() { nodes::addBuiltinNodeTypes(m_registry); }

    void theBlockParsesWithItsRolesAndPlainInputs() {
        const Result<std::optional<Action>> parsed = actionOf(load(fillWorkflow()));
        QVERIFY(parsed);
        QVERIFY(parsed->has_value());
        const Action &action = **parsed;
        QCOMPARE(action.name, QStringLiteral("Fill"));
        QCOMPARE(action.glyph, QStringLiteral("sparkles"));
        QCOMPARE(action.selection, QStringLiteral("required"));
        QCOMPARE(action.region, QStringLiteral("region"));
        QCOMPARE(action.inputs.size(), 4);
        QCOMPARE(action.inputs.at(0).key(), QStringLiteral("prompt"));
        QCOMPARE(action.inputs.at(3).key(), QStringLiteral("steps"));
        QCOMPARE(action.inputs.at(3).targets.first().toString(), QStringLiteral("sample.steps"));
        QCOMPARE(action.input("strength")->min.value(), 0.05);
        QCOMPARE(action.input("strength")->section, QStringLiteral("Advanced"));
        QVERIFY(action.needsPrompt()); // not optional, no fallback
    }

    void aWorkflowWithoutABlockIsNoAction() {
        QJsonObject plain = fillWorkflow();
        plain.remove("action");
        const Result<std::optional<Action>> parsed = actionOf(load(plain));
        QVERIFY(parsed);
        QVERIFY(!parsed->has_value());
    }

    // The block is a top-level key the document keeps (spec 4.1) and it is outside sourceHash: it changes no cache key.
    void theBlockSurvivesARoundTripAndIsOutsideTheSourceHash() {
        const Document with = load(fillWorkflow());
        QJsonObject bare = fillWorkflow();
        bare.remove("action");
        QCOMPARE(with.sourceHash(), load(bare).sourceHash());
        const Document again = Document::load(with.save()).value();
        QCOMPARE(again.extra.value("action"), fillWorkflow().value("action"));
    }

    void aMalformedBlockSaysWhatIsWrong_data() {
        QTest::addColumn<QJsonValue>("block");
        QTest::addColumn<QString>("words");
        const QJsonObject good = fillWorkflow().value("action").toObject();
        auto change = [&](const std::function<void(QJsonObject &)> &f) {
            QJsonObject b = good;
            f(b);
            return QJsonValue(b);
        };
        QTest::newRow("not an object") << QJsonValue(3) << "not an object";
        QTest::newRow("no name") << change([](QJsonObject &b) { b.remove("name"); }) << "no name";
        QTest::newRow("unknown role") << change([](QJsonObject &b) { b.insert("inputs", QJsonArray{QJsonObject{{"role", "colour"}, {"targets", QJsonArray{"a.b"}}}}); })
                                      << "does not know";
        QTest::newRow("no targets") << change([](QJsonObject &b) { b.insert("inputs", QJsonArray{QJsonObject{{"role", "seed"}}}); }) << "names no targets";
        QTest::newRow("bad target") << change([](QJsonObject &b) { b.insert("inputs", QJsonArray{QJsonObject{{"role", "seed"}, {"targets", QJsonArray{"nodot"}}}}); })
                                    << "not \"<node>.<input>\"";
        QTest::newRow("plain without name") << change([](QJsonObject &b) { b.insert("inputs", QJsonArray{QJsonObject{{"targets", QJsonArray{"a.b"}}}}); })
                                            << "no role and no name";
        QTest::newRow("twice") << change([](QJsonObject &b) {
            b.insert("inputs", QJsonArray{QJsonObject{{"role", "seed"}, {"targets", QJsonArray{"a.b"}}}, QJsonObject{{"role", "seed"}, {"targets", QJsonArray{"a.c"}}}});
        }) << "twice";
        QTest::newRow("selection") << change([](QJsonObject &b) { b.insert("selection", "maybe"); }) << "required, optional or bounds";
    }
    void aMalformedBlockSaysWhatIsWrong() {
        QFETCH(QJsonValue, block);
        QFETCH(QString, words);
        const Result<Action> parsed = parseAction(block);
        QVERIFY(!parsed);
        QCOMPARE(parsed.error().code, QStringLiteral("action_invalid"));
        QVERIFY2(parsed.error().message.contains(words), qPrintable(parsed.error().message));
    }

    void aLaterFormatNeedsANewerFugue() {
        const Result<Action> parsed = parseAction(withAction(fillWorkflow(), [](QJsonObject &a) { a.insert("format", 2); }).value("action"));
        QVERIFY(!parsed);
        QCOMPARE(parsed.error().code, QStringLiteral("unsupported_format_version"));
        QVERIFY(parsed.error().hint.contains("Needs a newer Fugue"));
    }

    void theCheckFindsTheRegionAndTheWrites() {
        const Result<ActionGraph> graph = check(fillWorkflow());
        QVERIFY2(graph, qPrintable(firstMessage(graph.errors())));
        QCOMPARE(graph->region, QStringLiteral("region"));
        QCOMPARE(graph->writes, QStringList{"write"});
        // With one Region the block need not name it.
        const Result<ActionGraph> unnamed = check(withAction(fillWorkflow(), [](QJsonObject &a) { a.remove("region"); }));
        QVERIFY(unnamed);
        QCOMPARE(unnamed->region, QStringLiteral("region"));
    }

    void aBindingThatDoesNotFitTheGraphIsBrokenWithTheReason_data() {
        QTest::addColumn<QJsonObject>("workflow");
        QTest::addColumn<QString>("words");
        const QJsonObject base = fillWorkflow();
        QTest::newRow("missing node") << withNode(base, "encode", {}) << "has no node encode";
        QTest::newRow("missing input") << withAction(base, [](QJsonObject &a) {
            a.insert("inputs", QJsonArray{QJsonObject{{"role", "seed"}, {"targets", QJsonArray{"sample.colour"}}}});
        }) << "has no input colour";
        QTest::newRow("wrong type") << withAction(base, [](QJsonObject &a) {
            a.insert("inputs", QJsonArray{QJsonObject{{"role", "prompt"}, {"targets", QJsonArray{"sample.steps"}}}});
        }) << "takes number, not text";
        QTest::newRow("mixed types") << withAction(base, [](QJsonObject &a) {
            a.insert("inputs", QJsonArray{QJsonObject{{"name", "mix"}, {"targets", QJsonArray{"sample.steps", "encode.prompt"}}}});
        }) << "different types";
        QTest::newRow("linked") << withAction(base, [](QJsonObject &a) {
            a.insert("inputs", QJsonArray{QJsonObject{{"name", "c"}, {"targets", QJsonArray{"sample.conditioning"}}}});
        }) << "linked to another node";
        QTest::newRow("no region") << withNode(withAction(base, [](QJsonObject &a) { a.remove("region"); }), "region", {}) << "no Canvas Region";
        QTest::newRow("two regions") << withNode(withAction(base, [](QJsonObject &a) { a.remove("region"); }), "second",
                                                 QJsonObject{{"type", "fugue/canvas-region"}, {"version", "1.2.0"}, {"inputs", QJsonObject{}}})
                                     << "does not say which one";
        QTest::newRow("region not a region") << withAction(base, [](QJsonObject &a) { a.insert("region", "encode"); }) << "not a Canvas Region";
        QTest::newRow("no write") << withNode(base, "write", {}) << "no Canvas Write";
        QTest::newRow("other canvas") << withNode(withNode(base, "elsewhere", QJsonObject{{"type", "fugue/canvas-region"}, {"version", "1.2.0"},
                                                                                         {"inputs", QJsonObject{{"canvas", "other"}}}}),
                                                  "write2", QJsonObject{{"type", "fugue/canvas-write"}, {"version", "1.3.0"},
                                                                        {"inputs", QJsonObject{{"canvas", QJsonObject{{"from", "elsewhere.canvas"}}},
                                                                                               {"image", QJsonObject{{"from", "region.image"}}}}}})
                                      << "writes to another canvas";
    }
    void aBindingThatDoesNotFitTheGraphIsBrokenWithTheReason() {
        QFETCH(QJsonObject, workflow);
        QFETCH(QString, words);
        const Result<ActionGraph> graph = check(workflow);
        QVERIFY(!graph);
        QCOMPARE(graph.error().code, QStringLiteral("action_invalid"));
        QVERIFY2(graph.error().message.contains(words), qPrintable(graph.error().message));
        QVERIFY(!graph.error().hint.isEmpty());
    }

    // A group instance's exposed input is a target too (design 1.2), of its inner input's type.
    void aGroupsExposedInputIsATarget() {
        QJsonObject workflow = fillWorkflow();
        workflow.insert("groups", QJsonObject{{"local/words", QJsonObject{
            {"version", "1.0.0"}, {"name", "Words"},
            {"inputs", QJsonArray{QJsonObject{{"name", "words"}, {"targets", QJsonArray{"t.text"}}}}},
            {"outputs", QJsonArray{QJsonObject{{"name", "text"}, {"from", "t.text"}}}},
            {"nodes", QJsonObject{{"t", QJsonObject{{"type", "fugue/text"}, {"version", "1.0.0"}, {"inputs", QJsonObject{}}}}}}}}});
        workflow = withNode(workflow, "g", QJsonObject{{"type", "local/words"}, {"version", "1.0.0"}, {"inputs", QJsonObject{}}});
        workflow = withAction(workflow, [](QJsonObject &a) {
            a.insert("inputs", QJsonArray{QJsonObject{{"role", "prompt"}, {"targets", QJsonArray{"g.words"}}}});
        });
        const Result<ActionGraph> graph = check(workflow);
        QVERIFY2(graph, qPrintable(firstMessage(graph.errors())));
    }

    // The binder: workflow and context give the run's document, exactly.
    void bindingPutsTheCanvasTheHeadTheRectangleAndTheValuesIn() {
        const Document document = load(fillWorkflow());
        const Action action = **actionOf(document);
        const ActionGraph graph = checkAction(action, document, m_registry).value();
        ActionBinding binding;
        binding.canvas = "c1";
        binding.revision = "sha256:" + QString(64, 'a');
        binding.rect = RouteRect{10, 20, 30, 40};
        binding.padding = 64;
        binding.scale = 2.5;
        binding.values = {{"prompt", "a red umbrella"}, {"seed", 1234}, {"strength", 3.0}};
        const Result<Document> bound = bindAction(document, action, graph, binding);
        QVERIFY2(bound, qPrintable(firstMessage(bound.errors())));
        const QJsonObject nodes = bound->toJson().value("graph").toObject().value("nodes").toObject();
        QCOMPARE(nodes.value("region").toObject().value("inputs").toObject(),
                 QJsonObject({{"canvas", "c1"}, {"revision", binding.revision}, {"x", 10}, {"y", 20}, {"width", 30}, {"height", 40},
                              {"padding", 64}, {"scale", 2.5}}));
        QCOMPARE(nodes.value("encode").toObject().value("inputs").toObject().value("prompt"), QJsonValue("a red umbrella"));
        const QJsonObject sample = nodes.value("sample").toObject().value("inputs").toObject();
        QCOMPARE(sample.value("seed"), QJsonValue(1234));
        QCOMPARE(sample.value("denoise"), QJsonValue(1.0)); // held to the block's max
        QCOMPARE(sample.value("steps"), QJsonValue(4));     // no value: the workflow's own
        // The file is never changed: the document given is as it was.
        QCOMPARE(document.graph.nodes.value("encode").inputs.value("prompt").literal(), QJsonValue("old words"));
        QCOMPARE(bound->sourceHash() == document.sourceHash(), false);
    }

    void aStoredSelectionGoesInByHashAndARectangleDropsAnOldOne() {
        QJsonObject workflow = fillWorkflow();
        QJsonObject withOld = withNode(workflow, "region", QJsonObject{{"type", "fugue/canvas-region"}, {"version", "1.2.0"},
                                                                     {"inputs", QJsonObject{{"canvas", ""}, {"selection", "sha256:" + QString(64, 'b')}}}});
        const Document document = load(withOld);
        const Action action = **actionOf(document);
        const ActionGraph graph = checkAction(action, document, m_registry).value();
        ActionBinding binding{"c1", "sha256:" + QString(64, 'a'), RouteRect{1, 2, 3, 4}, {}, {}, {}, {{"prompt", "x"}}};
        // A rectangle never reads the selection the file held.
        QVERIFY(!bindAction(document, action, graph, binding)->graph.nodes.value("region").inputs.contains("selection"));
        binding.selection = "sha256:" + QString(64, 'c');
        const Node region = bindAction(document, action, graph, binding)->graph.nodes.value("region");
        QCOMPARE(region.inputs.value("selection").literal(), QJsonValue(binding.selection));
        QCOMPARE(region.inputs.value("width").literal(), QJsonValue(3)); // its bounds, as Use selection writes them
    }

    void anotherBlankRegionReadsTheSamePictureAndANamedOneIsLeftAlone() {
        QJsonObject workflow = withNode(fillWorkflow(), "ref", QJsonObject{{"type", "fugue/canvas-region"}, {"version", "1.2.0"},
                                                                         {"inputs", QJsonObject{{"canvas", ""}, {"x", 5}}}});
        workflow = withNode(workflow, "other", QJsonObject{{"type", "fugue/canvas-region"}, {"version", "1.2.0"}, {"inputs", QJsonObject{{"canvas", "else"}}}});
        const Document document = load(workflow);
        const Action action = **actionOf(document);
        const ActionGraph graph = checkAction(action, document, m_registry).value();
        const ActionBinding binding{"c1", "sha256:" + QString(64, 'a'), RouteRect{1, 2, 3, 4}, {}, {}, {}, {}};
        const Document bound = bindAction(document, action, graph, binding).value();
        QCOMPARE(bound.graph.nodes.value("ref").inputs.value("canvas").literal(), QJsonValue("c1"));
        QCOMPARE(bound.graph.nodes.value("ref").inputs.value("revision").literal(), QJsonValue(binding.revision));
        QCOMPARE(bound.graph.nodes.value("ref").inputs.value("x").literal(), QJsonValue(5)); // its rectangle as written
        QCOMPARE(bound.graph.nodes.value("other").inputs.value("canvas").literal(), QJsonValue("else"));
        QVERIFY(!bound.graph.nodes.value("other").inputs.contains("revision"));
    }

    void anEmptyPromptTakesTheFallbackElseIsRefused() {
        const Document document = load(fillWorkflow());
        const Action action = **actionOf(document);
        const ActionGraph graph = checkAction(action, document, m_registry).value();
        const ActionBinding empty{"c1", "sha256:" + QString(64, 'a'), RouteRect{1, 2, 3, 4}, {}, {}, {}, {{"prompt", "  "}}};
        const Result<Document> refused = bindAction(document, action, graph, empty);
        QVERIFY(!refused);
        QVERIFY(refused.error().message.contains("Type what to put"));
        const Document withFallback = load(withAction(fillWorkflow(), [](QJsonObject &a) {
            a.insert("inputs", QJsonArray{QJsonObject{{"role", "prompt"}, {"targets", QJsonArray{"encode.prompt"}}, {"fallback", "Continue the scene."}}});
        }));
        const Action fallback = **actionOf(withFallback);
        QVERIFY(!fallback.needsPrompt());
        const Document bound = bindAction(withFallback, fallback, graph, empty).value();
        QCOMPARE(bound.graph.nodes.value("encode").inputs.value("prompt").literal(), QJsonValue("Continue the scene."));
        // No selection where one is required: said in words, nothing bound.
        const ActionBinding none{"c1", "sha256:" + QString(64, 'a'), {}, {}, {}, {}, {{"prompt", "x"}}};
        QVERIFY(bindAction(document, action, graph, none).error().message.contains("Select an area first"));
    }

    // The bundled Generative fill (research 5.1): Klein 4B, the crop as Text Encode's reference, 4 steps, guidance 1,
    // cap 1 MP, multiple 16, feather 0, Write blend 16; its block fits its graph, and the bound copy compiles.
    void theBundledGenerativeFillFitsItsGraphAndItsBoundCopyCompiles() {
        const Result<Document> document = Document::loadFile(kBundled);
        QVERIFY2(document, qPrintable(firstMessage(document.errors())));
        const Result<std::optional<Action>> action = actionOf(*document);
        QVERIFY(action && action->has_value());
        QCOMPARE((*action)->name, QStringLiteral("Generative fill"));
        QVERIFY((*action)->needsPrompt()); // an empty prompt changes nothing on Klein (research G8)
        const Result<ActionGraph> graph = checkAction(**action, *document, m_registry);
        QVERIFY2(graph, qPrintable(firstMessage(graph.errors())));
        const auto &nodes = document->graph.nodes;
        QCOMPARE(nodes.value("models").inputs.value("diffusion_model").handle().value("name").toString(), QStringLiteral("flux-2-klein-4b-Q8_0.gguf"));
        QCOMPARE(nodes.value("region").version.toString(), QStringLiteral("1.2.0")); // takes a stored selection
        QCOMPARE(nodes.value("region").inputs.value("max_megapixels").literal().toDouble(), 1.0);
        QCOMPARE(nodes.value("region").inputs.value("multiple").literal().toInt(), 16);
        QCOMPARE(nodes.value("region").inputs.value("feather").literal().toInt(), 0);
        QCOMPARE(nodes.value("encode").inputs.value("references").items().first().from().toString(), QStringLiteral("region.image"));
        QCOMPARE(nodes.value("sample").inputs.value("steps").literal().toInt(), 4);
        QCOMPARE(nodes.value("sample").inputs.value("guidance").literal().toDouble(), 1.0);
        QCOMPARE(nodes.value("write").inputs.value("blend").literal().toInt(), 16);
        QVERIFY(nodes.value("region").inputs.value("canvas").literal().toString().isEmpty()); // no canvas id in the file
        ActionBinding binding{"c1", "sha256:" + QString(64, 'a'), RouteRect{100, 100, 200, 150}, {}, 100, 1.5, {{"prompt", "a red umbrella"}, {"seed", 7}}};
        const Result<Document> bound = bindAction(*document, **action, *graph, binding);
        QVERIFY(bound);
        QVERIFY(!hasErrors(compile(*bound, m_registry).problems()));
    }
};

QTEST_GUILESS_MAIN(TestCanvasAction)
#include "test_canvas_action.moc"
