// Fugue Apps (#384): the `app` block parsed, written, kept by the document and the share copy, pruned when a node it
// names is deleted, checked against the graph and bound. The input list is the one the `action` block has
// (engine/input_binding.h); test_canvas_action covers it through actions, this covers it through apps. Pure: no window.

#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

#include "engine/app_binding.h"
#include "engine/document.h"
#include "engine/node_type.h"
#include "engine/registry.h"
#include "engine/share_copy.h"
#include "nodes/builtin.h"

using namespace fugue;
using namespace fugue::app;

namespace {

QJsonObject workflow() {
    return QJsonDocument::fromJson(R"({
      "fugue": 1,
      "app": {"format": 1, "title": "Restyle", "description": "Paints a picture in a style.", "defaultView": "graph",
              "inputs": [{"role": "image", "name": "subject", "targets": ["load.file"], "label": "Picture"},
                         {"role": "prompt", "targets": ["encode.prompt"], "label": "Prompt"},
                         {"role": "seed", "targets": ["sample.seed", "sample2.seed"]},
                         {"name": "steps", "targets": ["sample.steps"], "min": 1, "max": 8, "section": "Advanced"},
                         {"name": "quality", "targets": ["sample.denoise"], "choices": ["Light", "Strong"],
                          "choice_values": [0.4, 0.9], "default": "Light"},
                         {"name": "type", "targets": ["save.format"], "choices": ["png"], "default": "png"}],
              "outputs": [{"node": "save", "label": "Result"}],
              "compare": {"input": "subject", "top": "steps"}},
      "graph": {"nodes": {
        "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "a.png"}},
        "encode": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "old words"}},
        "sample": {"type": "fugue/sampler", "version": "1.2.0", "inputs": {"conditioning": {"from": "encode.conditioning"}, "seed": {"auto": "random"}, "steps": 4}},
        "sample2": {"type": "fugue/sampler", "version": "1.2.0", "inputs": {"seed": 1}},
        "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"image": {"from": "load.image"}, "name": "out"}}
      }}
    })").object();
}

Document load(const QJsonObject &json) {
    const Result<Document> loaded = Document::fromJson(json);
    if (!loaded)
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

App appOfFixture(const Document &document) {
    const Result<std::optional<App>> parsed = appOf(document);
    if (!parsed || !parsed->has_value())
        qFatal("fixture has no app");
    return **parsed;
}

// A picture through a library group instance (spec 5.4) whose exposed `opacity` and `mode` are a Blend's, and a block that binds them.
QJsonObject groupWorkflow() {
    return QJsonDocument::fromJson(R"({
      "fugue": 1,
      "app": {"format": 1, "title": "Shade",
              "inputs": [{"name": "opacity", "targets": ["shade.opacity"]},
                         {"name": "mode", "targets": ["shade.mode"]}],
              "outputs": [{"node": "save"}]},
      "graph": {"nodes": {
        "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "a.png"}},
        "shade": {"type": "local/shade", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}, "opacity": 0.25}},
        "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"image": {"from": "shade.image"}, "name": "out"}}}},
      "groups": {"local/shade": {"version": "1.0.0", "name": "Shade",
        "inputs": [{"name": "image", "targets": ["mix.base", "mix.top"]},
                   {"name": "opacity", "targets": ["mix.opacity"]},
                   {"name": "mode", "targets": ["mix.mode"]}],
        "outputs": [{"name": "image", "from": "mix.image"}],
        "nodes": {"mix": {"type": "fugue/blend", "version": "1.0.0", "inputs": {}}}}}
    })").object();
}

QJsonObject withBlock(const std::function<void(QJsonObject &)> &change) {
    QJsonObject json = workflow();
    QJsonObject block = json.value("app").toObject();
    change(block);
    json.insert("app", block);
    return json;
}

QJsonObject withoutNode(QJsonObject json, const QString &id) {
    QJsonObject graph = json.value("graph").toObject();
    QJsonObject nodes = graph.value("nodes").toObject();
    nodes.remove(id);
    graph.insert("nodes", nodes);
    json.insert("graph", graph);
    return json;
}

QString messages(const Errors &errors) {
    QStringList all;
    for (const Error &error : errors)
        all << error.message;
    return all.join(QLatin1Char('\n'));
}

} // namespace

class TestAppBinding : public QObject {
    Q_OBJECT
    Registry m_registry;

private slots:
    void initTestCase() { nodes::addBuiltinNodeTypes(m_registry); }

    void theBlockParsesAndWritesBackTheSame() {
        const App app = appOfFixture(load(workflow()));
        QCOMPARE(app.title, QStringLiteral("Restyle"));
        QCOMPARE(app.defaultView, QStringLiteral("graph"));
        QCOMPARE(app.inputs.size(), 6);
        // A role with a name is keyed by the name: two pictures can both have the image role.
        QCOMPARE(app.inputs.at(0).role, QStringLiteral("image"));
        QCOMPARE(app.inputs.at(0).key(), QStringLiteral("subject"));
        QCOMPARE(app.input("seed")->targets.size(), 2);
        QCOMPARE(app.outputNodes(), QStringList{"save"});
        QVERIFY(app.compare && app.compare->input == "subject" && app.compare->top == "steps" && app.compare->left.isEmpty());
        QCOMPARE(toJson(app), workflow().value("app").toObject());
    }

    void aWorkflowWithoutABlockIsNoApp() {
        QJsonObject plain = workflow();
        plain.remove("app");
        const Result<std::optional<App>> parsed = appOf(load(plain));
        QVERIFY(parsed);
        QVERIFY(!parsed->has_value());
    }

    // Kept on save/load and by the share copy (an older Fugue keeps unknown top-level keys the same way), and outside the hash.
    void theBlockSurvivesSaveLoadAndShareAndIsOutsideTheSourceHash() {
        const Document with = load(workflow());
        QJsonObject bare = workflow();
        bare.remove("app");
        QCOMPARE(with.sourceHash(), load(bare).sourceHash());
        QCOMPARE(Document::load(with.save()).value().extra.value("app"), workflow().value("app"));
        ShareOptions options;
        options.withoutLayout = true;
        QCOMPARE(shareCopy(with.toJson(), m_registry, options).value("app"), workflow().value("app"));
        Document edited = with;
        App app = appOfFixture(edited);
        app.title = "Changed";
        setApp(edited, app);
        QCOMPARE(appOfFixture(Document::load(edited.save()).value()).title, QStringLiteral("Changed"));
        clearApp(edited);
        QVERIFY(!edited.extra.contains("app"));
    }

    void aMalformedBlockSaysWhatIsWrong_data() {
        QTest::addColumn<QJsonObject>("json");
        QTest::addColumn<QString>("words");
        const auto row = [&](const char *name, const std::function<void(QJsonObject &)> &change, const char *words) {
            QTest::newRow(name) << withBlock(change) << QString::fromLatin1(words);
        };
        row("no title", [](QJsonObject &b) { b.remove("title"); }, "no title");
        row("view", [](QJsonObject &b) { b.insert("defaultView", "wall"); }, "app or graph");
        row("compare unknown picture", [](QJsonObject &b) { b.insert("compare", QJsonObject{{"input", "nope"}}); },
            "names the input nope, which the app does not have");
        row("compare no picture", [](QJsonObject &b) { b.insert("compare", QJsonObject{{"left", "steps"}}); }, "names no `input`");
        row("compare unknown side", [](QJsonObject &b) {
            b.insert("compare", QJsonObject{{"input", "subject"}, {"top", "nope"}});
        }, "names the input nope");
        row("compare not a picture", [](QJsonObject &b) { b.insert("compare", QJsonObject{{"input", "prompt"}}); }, "not a picture");
        row("action role", [](QJsonObject &b) { b.insert("inputs", QJsonArray{QJsonObject{{"role", "colour"}, {"targets", QJsonArray{"a.b"}}}}); },
            "does not know");
        row("no targets", [](QJsonObject &b) { b.insert("inputs", QJsonArray{QJsonObject{{"role", "seed"}}}); }, "names no targets");
        row("name twice", [](QJsonObject &b) {
            const QJsonObject one{{"role", "image"}, {"name", "a"}, {"targets", QJsonArray{"load.file"}}};
            b.insert("inputs", QJsonArray{one, one});
        }, "twice");
        row("output no node", [](QJsonObject &b) { b.insert("outputs", QJsonArray{QJsonObject{{"label", "x"}}}); }, "names no node");
        row("output twice", [](QJsonObject &b) { b.insert("outputs", QJsonArray{QJsonObject{{"node", "s"}}, QJsonObject{{"node", "s"}}}); }, "twice");
    }
    void aMalformedBlockSaysWhatIsWrong() {
        QFETCH(QJsonObject, json);
        QFETCH(QString, words);
        const Result<App> parsed = parseApp(json.value("app"));
        QVERIFY(!parsed);
        QCOMPARE(parsed.error().code, QStringLiteral("app_invalid"));
        QVERIFY2(messages(parsed.errors()).contains(words), qPrintable(messages(parsed.errors())));
    }

    void aLaterFormatNeedsANewerFugue() {
        const Result<App> parsed = parseApp(withBlock([](QJsonObject &b) { b.insert("format", 2); }).value("app"));
        QVERIFY(!parsed);
        QCOMPARE(parsed.error().code, QStringLiteral("unsupported_format_version"));
    }

    void aBlockThatFitsItsGraphChecksClean() {
        const Document document = load(workflow());
        const Errors errors = checkApp(appOfFixture(document), document, m_registry);
        QVERIFY2(errors.isEmpty(), qPrintable(messages(errors)));
    }

    // An exposed input of a group instance keeps its inner input's spec (spec 5.4): type, range, choices, default.
    void aGroupInstanceTargetHasTheInnerInputsSpec() {
        const Document document = load(groupWorkflow());
        const InputSpec *opacity = targetInputSpec(document, m_registry, PortRef{"shade", "opacity"});
        QVERIFY(opacity);
        QCOMPARE(opacity->type.type.id, QStringLiteral("number"));
        QCOMPARE(opacity->min, std::optional<double>(0));
        QCOMPARE(opacity->max, std::optional<double>(1));
        QCOMPARE(opacity->defaultValue.toDouble(), 0.5);
        const InputSpec *mode = targetInputSpec(document, m_registry, PortRef{"shade", "mode"});
        QVERIFY(mode && mode->choices && mode->choices->contains("multiply"));
        QVERIFY(!targetInputSpec(document, m_registry, PortRef{"shade", "nope"}));
        QVERIFY(!targetInputSpec(document, m_registry, PortRef{"gone", "opacity"}));
        const App app = appOfFixture(document);
        const InputBinding completed = completedBy(*app.input("opacity"), opacity);
        QCOMPARE(completed.max, std::optional<double>(1));
        QVERIFY(!completed.choices);
        QCOMPARE(completedBy(*app.input("mode"), mode).choices, mode->choices);
        QVERIFY2(checkApp(app, document, m_registry).isEmpty(), qPrintable(messages(checkApp(app, document, m_registry))));
    }

    void aWrongTypedGroupInstanceTargetIsCaught() {
        QJsonObject json = groupWorkflow();
        QJsonObject block = json.value("app").toObject();
        block.insert("inputs", QJsonArray{QJsonObject{{"role", "prompt"}, {"targets", QJsonArray{"shade.opacity"}}},
                                          QJsonObject{{"name", "mode"}, {"targets", QJsonArray{"shade.mode"}}, {"choices", QJsonArray{"multiply", "wobble"}}}});
        json.insert("app", block);
        const Document document = load(json);
        const QString said = messages(checkApp(appOfFixture(document), document, m_registry));
        QVERIFY2(said.contains("takes number, not text"), qPrintable(said));
        QVERIFY2(said.contains("\"wobble\""), qPrintable(said));
    }

    void aGroupInstanceInputThatIsGoneIsStale() {
        QJsonObject json = groupWorkflow();
        QJsonObject block = json.value("app").toObject();
        block.insert("inputs", QJsonArray{QJsonObject{{"name", "opacity"}, {"targets", QJsonArray{"shade.gone"}}}});
        json.insert("app", block);
        const Document document = load(json);
        App app = appOfFixture(document);
        QVERIFY2(messages(pruneApp(app, document, m_registry)).contains("shade.gone"), "stale");
        QVERIFY(app.inputs.isEmpty());
    }

    void aDeletedNodeIsDroppedAndReported() {
        const Document document = load(withoutNode(workflow(), "encode"));
        App app = appOfFixture(document);
        const Errors report = pruneApp(app, document, m_registry);
        QCOMPARE(report.size(), 2); // the target, then the input left with none
        QCOMPARE(report.first().code, QStringLiteral("app_stale"));
        QCOMPARE(report.first().severity, Error::Severity::Warning);
        QVERIFY2(messages(report).contains("encode.prompt"), qPrintable(messages(report)));
        QVERIFY(!app.input("prompt"));
        QCOMPARE(app.inputs.size(), 5);
        QVERIFY(checkApp(app, document, m_registry).isEmpty());
    }

    void aDeletedTargetOfSeveralLeavesTheOthersAndADeletedInputIsStaleToo() {
        QJsonObject json = withoutNode(workflow(), "sample2");
        // sample's `steps` input is still there; point one target at an input the node type does not have.
        QJsonObject block = json.value("app").toObject();
        QJsonArray inputs = block.value("inputs").toArray();
        QJsonObject steps = inputs.at(3).toObject();
        steps.insert("targets", QJsonArray{"sample.steps", "sample.gone"});
        inputs.replace(3, steps);
        block.insert("inputs", inputs);
        json.insert("app", block);
        const Document document = load(json);
        App app = appOfFixture(document);
        const Errors report = pruneApp(app, document, m_registry);
        QCOMPARE(report.size(), 2);
        QCOMPARE(app.input("seed")->targets.size(), 1);
        QCOMPARE(app.input("steps")->targets.size(), 1);
        QCOMPARE(app.input("steps")->targets.first().toString(), QStringLiteral("sample.steps"));
    }

    void aDeletedOutputIsDroppedAndAnAppWithNoOutputIsInvalid() {
        const Document document = load(withoutNode(workflow(), "save"));
        App app = appOfFixture(document);
        const Errors report = pruneApp(app, document, m_registry);
        QVERIFY2(messages(report).contains("output save"), qPrintable(messages(report)));
        QVERIFY(app.outputs.isEmpty());
        QVERIFY(messages(checkApp(app, document, m_registry)).contains("no output"));
    }

    // A node of a pack that is not installed is not a deleted node: its targets stay.
    void aNodeOfAMissingPackIsNotStale() {
        QJsonObject json = workflow();
        QJsonObject graph = json.value("graph").toObject();
        QJsonObject nodes = graph.value("nodes").toObject();
        nodes.insert("encode", QJsonObject{{"type", "acme.packs/encoder"}, {"version", "1.0.0"}, {"inputs", QJsonObject{{"prompt", "x"}}}});
        graph.insert("nodes", nodes);
        json.insert("graph", graph);
        const Document document = load(json);
        App app = appOfFixture(document);
        QVERIFY(pruneApp(app, document, m_registry).isEmpty());
        QVERIFY(app.input("prompt"));
        QVERIFY(messages(checkApp(app, document, m_registry)).contains("not installed"));
    }

    void whatIsStillWrongIsReportedInWords_data() {
        QTest::addColumn<QJsonObject>("json");
        QTest::addColumn<QString>("words");
        const auto input = [&](const QJsonObject &fields) {
            return withBlock([&](QJsonObject &b) { b.remove("compare"); b.insert("inputs", QJsonArray{fields}); });
        };
        QTest::newRow("wrong type for the role") << input({{"role", "seed"}, {"targets", QJsonArray{"encode.prompt"}}}) << "not number";
        QTest::newRow("two types") << input({{"name", "x"}, {"targets", QJsonArray{"sample.seed", "encode.prompt"}}}) << "different types";
        QTest::newRow("linked") << input({{"name", "x"}, {"targets", QJsonArray{"sample.conditioning"}}}) << "linked to another node";
        QTest::newRow("choice not offered by the target")
            << input({{"name", "type"}, {"targets", QJsonArray{"save.format"}}, {"choices", QJsonArray{"png", "bmp"}}}) << "\"bmp\"";
        QTest::newRow("choice values onto text")
            << input({{"name", "type"}, {"targets", QJsonArray{"save.format"}}, {"choices", QJsonArray{"a"}}, {"choice_values", QJsonArray{1}}, {"default", "a"}})
            << "not a number";
        QTest::newRow("output not a sink") << withBlock([](QJsonObject &b) { b.insert("outputs", QJsonArray{QJsonObject{{"node", "sample"}}}); })
                                           << "not a Save or Preview sink";
        QTest::newRow("output not a node") << withBlock([](QJsonObject &b) { b.insert("outputs", QJsonArray{QJsonObject{{"node", "nope"}}}); })
                                           << "not a node";
    }
    void whatIsStillWrongIsReportedInWords() {
        QFETCH(QJsonObject, json);
        QFETCH(QString, words);
        const Document document = load(json);
        const Errors errors = checkApp(appOfFixture(document), document, m_registry);
        QVERIFY(hasErrors(errors));
        QCOMPARE(errors.first().code, QStringLiteral("app_invalid"));
        QVERIFY2(messages(errors).contains(words), qPrintable(messages(errors)));
    }

    // Value in: the form's values (and the block's defaults) go into a copy; the file's document is untouched.
    void bindingPutsTheValuesInACopy() {
        const Document document = load(workflow());
        const App app = appOfFixture(document);
        const Result<Document> bound = bindApp(document, app,
                                               {{"subject", "b.png"}, {"prompt", "a fox"}, {"seed", 7}, {"steps", 99}, {"quality", "Strong"}});
        QVERIFY(bound);
        const auto literal = [&](const Document &d, const char *node, const char *input) { return d.graph.nodes.value(node).inputs.value(input).literal(); };
        QCOMPARE(literal(*bound, "load", "file"), QJsonValue("b.png"));
        QCOMPARE(literal(*bound, "encode", "prompt"), QJsonValue("a fox"));
        QCOMPARE(literal(*bound, "sample", "seed"), QJsonValue(7));
        QCOMPARE(literal(*bound, "sample2", "seed"), QJsonValue(7)); // one input, two targets
        QCOMPARE(literal(*bound, "sample", "steps"), QJsonValue(8)); // held in max
        QCOMPARE(literal(*bound, "sample", "denoise"), QJsonValue(0.9)); // the word's number
        QCOMPARE(literal(*bound, "save", "format"), QJsonValue("png")); // the block's default
        QCOMPARE(literal(document, "encode", "prompt"), QJsonValue("old words"));
        QCOMPARE(literal(*bound, "save", "name"), QJsonValue("out")); // no value, no default: the workflow's own stays
        const Result<Document> refused = bindApp(document, app, {{"quality", "Weird"}});
        QVERIFY(!refused);
        QCOMPARE(refused.error().code, QStringLiteral("app_invalid"));
    }

    // A target whose node was deleted is skipped by the binding, never brought back as an empty node.
    void bindingSkipsATargetWhoseNodeIsGone() {
        const Document document = load(withoutNode(workflow(), "encode"));
        const Result<Document> bound = bindApp(document, appOfFixture(document), {{"prompt", "a fox"}});
        QVERIFY(bound);
        QVERIFY(!bound->graph.nodes.contains("encode"));
        QCOMPARE(bound->graph.nodes.size(), document.graph.nodes.size());
    }

    // #484: an optional picture left empty takes its branch out of the run's copy; filled, it puts it back; a required one is never bypassed.
    void anOptionalPictureLeftEmptyIsBypassedAndFilledIsNot() {
        QJsonObject json = workflow();
        QJsonObject block = json.value("app").toObject();
        QJsonArray inputs = block.value("inputs").toArray();
        inputs.append(QJsonObject{{"role", "image"}, {"name", "extra"}, {"targets", QJsonArray{"load2.file"}}, {"optional", true}});
        inputs.append(QJsonObject{{"role", "image"}, {"name", "wide"}, {"targets", QJsonArray{"load3.file"}}, {"optional", true},
                                  {"bypass", QJsonArray{"load3", "blur"}}});
        block.insert("inputs", inputs);
        json.insert("app", block);
        QJsonObject nodes = json.value("graph").toObject().value("nodes").toObject();
        nodes.insert("load2", QJsonObject{{"type", "fugue/load-image"}, {"version", "1.1.0"}, {"mode", "bypass"}, {"inputs", QJsonObject{{"file", "teacup.jpg"}}}});
        nodes.insert("load3", QJsonObject{{"type", "fugue/load-image"}, {"version", "1.1.0"}, {"inputs", QJsonObject{{"file", "fox.jpg"}}}});
        nodes.insert("blur", QJsonObject{{"type", "fugue/load-image"}, {"version", "1.1.0"}, {"inputs", QJsonObject{{"file", "x.jpg"}}}});
        json.insert("graph", QJsonObject{{"nodes", nodes}});
        const Document document = load(json);
        const App app = appOfFixture(document);
        QVERIFY(checkApp(app, document, m_registry).isEmpty());
        const auto mode = [](const Document &d, const char *node) { return d.graph.nodes.value(node).mode; };

        const Result<Document> empty = bindApp(document, app, {{"subject", "b.png"}, {"extra", ""}});
        QVERIFY(empty);
        QCOMPARE(mode(*empty, "load2"), NodeMode::Bypass);
        QCOMPARE(mode(*empty, "load3"), NodeMode::Bypass); // left out altogether
        QCOMPARE(mode(*empty, "blur"), NodeMode::Bypass);  // the block's `bypass` list, not the target's node alone
        QCOMPARE(mode(*empty, "load"), NodeMode::Normal);  // a required picture is never bypassed
        QCOMPARE(empty->graph.nodes.value("load2").inputs.value("file").literal(), QJsonValue("teacup.jpg")); // nothing is written to an empty one

        const Result<Document> filled = bindApp(document, app, {{"subject", "b.png"}, {"extra", "c.png"}, {"wide", "d.png"}});
        QVERIFY(filled);
        QCOMPARE(mode(*filled, "load2"), NodeMode::Normal); // the file had it bypassed; the run's copy does not
        QCOMPARE(mode(*filled, "load3"), NodeMode::Normal);
        QCOMPARE(mode(*filled, "blur"), NodeMode::Normal);
        QCOMPARE(filled->graph.nodes.value("load2").inputs.value("file").literal(), QJsonValue("c.png"));
        QCOMPARE(mode(document, "load2"), NodeMode::Bypass); // the file's document is untouched
        QCOMPARE(mode(document, "load3"), NodeMode::Normal);

        // A required picture left empty is not switched off: the run fails on it as before.
        const Result<Document> required = bindApp(document, app, {{"subject", ""}});
        QVERIFY(required);
        QCOMPARE(mode(*required, "load"), NodeMode::Normal);
        QCOMPARE(required->graph.nodes.value("load").inputs.value("file").literal(), QJsonValue(""));
    }

    void aBypassListNamesNodesOfTheWorkflow() {
        QJsonObject json = workflow();
        QJsonObject block = json.value("app").toObject();
        QJsonArray inputs = block.value("inputs").toArray();
        QJsonObject first = inputs.at(0).toObject();
        first.insert("optional", true);
        first.insert("bypass", QJsonArray{"ghost"});
        inputs.replace(0, first);
        block.insert("inputs", inputs);
        json.insert("app", block);
        const Document document = load(json);
        const Errors errors = checkApp(appOfFixture(document), document, m_registry);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first().message.contains("ghost"));
    }
};

QTEST_GUILESS_MAIN(TestAppBinding)
#include "test_app_binding.moc"
