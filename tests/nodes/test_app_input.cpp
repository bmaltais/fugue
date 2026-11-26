// One description of an app input (#583, engine/app_input.h): its kind, default, whether a run needs it, its range and choices, and
// how a typed or text value becomes the JSON value bound. Pure: no AppView, no window, no process. The CLI's `app info` shape and the
// form's fields are those adapters' own tests (tests/cli/test_cli_app.cpp, tests/apps/test_app_view.cpp).

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryFile>
#include <QtTest>

#include "engine/app_binding.h"
#include "engine/app_input.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;
using namespace fugue::app;

namespace {

// The inputs of one app, each of a kind: a picture, an optional picture, a prompt, a prompt with nothing in it, a seed left to the
// engine and one with a number, a number with a range, a choice of the target's own, a toggle, a model, and a free text.
const char *kBlock = R"({
  "fugue": 1,
  "app": {"format": 1, "title": "Kinds",
          "inputs": [{"role": "image", "targets": ["load.file"]},
                     {"role": "image", "name": "region", "targets": ["region.file"], "optional": true},
                     {"role": "mask", "name": "hole", "targets": ["region.file"], "optional": true, "default": "hole.png"},
                     {"role": "prompt", "targets": ["encode.prompt"]},
                     {"role": "negative_prompt", "targets": ["blank.text"]},
                     {"role": "prompt", "name": "told", "targets": ["blank.text"], "fallback": "a cat"},
                     {"role": "seed", "targets": ["sample.seed"]},
                     {"role": "seed", "name": "fixed", "targets": ["sample2.seed"]},
                     {"name": "steps", "targets": ["sample.steps"], "min": 1, "max": 8, "section": "advanced"},
                     {"role": "strength", "targets": ["sample.denoise"]},
                     {"name": "sampler", "targets": ["sample.sampler"]},
                     {"name": "words", "targets": ["sample.scheduler"], "choices": ["karras", "simple"], "default": "simple"},
                     {"name": "soft", "targets": ["sample.soft_mask"]},
                     {"role": "model", "targets": ["sample.model"]},
                     {"name": "note", "targets": ["blank.text"], "default": "hello"},
                     {"name": "bare", "targets": ["sample.flow_shift"]}],
          "outputs": [{"node": "save"}]},
  "graph": {"nodes": {
    "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "a.png"}},
    "region": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "old.png"}},
    "encode": {"type": "fugue/text-encode", "version": "1.0.0", "inputs": {"prompt": "old words"}},
    "blank": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": ""}},
    "sample": {"type": "fugue/sampler", "version": "1.5.0", "inputs": {"seed": {"auto": "random"}, "steps": 4}},
    "sample2": {"type": "fugue/sampler", "version": "1.5.0", "inputs": {"seed": 7}},
    "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"image": {"from": "load.image"}, "name": "out"}}}}
})";

// A group instance whose exposed `opacity` (a Blend's number 0..1, default 0.5) and `mode` (its choice) the app binds.
const char *kGroup = R"({
  "fugue": 1,
  "app": {"format": 1, "title": "Shade",
          "inputs": [{"name": "opacity", "targets": ["shade.opacity"]}, {"name": "mode", "targets": ["shade.mode"]}],
          "outputs": [{"node": "save"}]},
  "graph": {"nodes": {
    "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "a.png"}},
    "shade": {"type": "local/shade", "version": "1.0.0", "inputs": {"image": {"from": "load.image"}}},
    "save": {"type": "fugue/save-image", "version": "1.1.0", "inputs": {"image": {"from": "shade.image"}, "name": "out"}}}},
  "groups": {"local/shade": {"version": "1.0.0", "name": "Shade",
    "inputs": [{"name": "image", "targets": ["mix.base", "mix.top"]},
               {"name": "opacity", "targets": ["mix.opacity"]},
               {"name": "mode", "targets": ["mix.mode"]}],
    "outputs": [{"name": "image", "from": "mix.image"}],
    "nodes": {"mix": {"type": "fugue/blend", "version": "1.0.0", "inputs": {}}}}}
})";

Document load(const char *json) {
    const Result<Document> loaded = Document::fromJson(QJsonDocument::fromJson(json).object());
    if (!loaded)
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

} // namespace

class TestAppInput : public QObject {
    Q_OBJECT
    Registry m_registry;
    Document m_kinds;
    Document m_group;
    App m_kindsApp;
    App m_groupApp;

    AppInput of(const QString &key) const {
        const InputBinding *input = m_kindsApp.input(key);
        if (!input)
            qFatal("no input %s", qPrintable(key));
        return describeInput(*input, m_kinds, &m_registry);
    }

private slots:
    void initTestCase() {
        nodes::addBuiltinNodeTypes(m_registry);
        m_kinds = load(kBlock);
        m_group = load(kGroup);
        m_kindsApp = **appOf(m_kinds);
        m_groupApp = **appOf(m_group);
        // The fixture is a well formed app, so the module describes what the graph really has.
        QVERIFY(!hasErrors(checkApp(m_kindsApp, m_kinds, m_registry)));
    }

    void everyRoleAndPortTypeHasItsKindAndItsCliType() {
        const QList<std::tuple<QString, QString, QString>> expected = {
            {"image", "image", "image"},   {"region", "image", "image"}, {"hole", "mask", "mask"},     {"prompt", "text", "text"},
            {"negative_prompt", "text", "text"}, {"seed", "seed", "number"}, {"steps", "number", "number"}, {"strength", "number", "number"},
            {"sampler", "choice", "choice"}, {"words", "choice", "choice"}, {"soft", "toggle", "boolean"}, {"model", "model", "model"},
            {"note", "text", "text"},      {"bare", "number", "number"}};
        for (const auto &[key, kind, cli] : expected) {
            const AppInput input = of(key);
            QVERIFY2(input.kind == kind, qPrintable(key + ": " + input.kind));
            QCOMPARE(cliTypeOf(input), cli);
        }
        // No target spec and no role to go by: any, as `fugue app info` always said.
        InputBinding loose;
        loose.name = "loose";
        QCOMPARE(describeInput(loose, m_kinds, &m_registry).kind, QStringLiteral("text"));
        QCOMPARE(cliTypeOf(describeInput(loose, m_kinds, &m_registry)), QStringLiteral("any"));
        // A resource port is a model with no role to say so; the role needs no registry.
        InputBinding byPort;
        byPort.name = "weights";
        byPort.targets = {PortRef{"sample", "model"}};
        QCOMPARE(describeInput(byPort, m_kinds, &m_registry).kind, QStringLiteral("model"));
        QVERIFY(describeInput(byPort, m_kinds, &m_registry).resource.has_value());
        QCOMPARE(describeInput(of("model").binding, m_kinds, nullptr).kind, QStringLiteral("model"));
    }

    void aDefaultIsTheBlocksThenTheWorkflowsThenTheTargetsOwn() {
        QCOMPARE(of("words").defaultValue, QJsonValue("simple"));      // the block's
        QCOMPARE(of("steps").defaultValue, QJsonValue(4));             // the node's literal
        QCOMPARE(of("fixed").defaultValue, QJsonValue(7));
        QCOMPARE(of("strength").defaultValue, QJsonValue(1));          // the target's own: the workflow holds none
        QVERIFY(of("sampler").defaultValue.isUndefined()); // the manifest has none
        QCOMPARE(of("note").defaultValue, QJsonValue("hello"));
        // A seed left to the engine is its own value, and says so.
        QCOMPARE(of("seed").defaultValue, QJsonValue(QJsonObject{{"auto", "random"}}));
        QVERIFY(of("seed").randomSeed);
        QVERIFY(!of("fixed").randomSeed);
        // An optional picture is left out: the file the node holds is no default (#484), a block's own is.
        QVERIFY(of("region").defaultValue.isUndefined());
        QCOMPARE(of("hole").defaultValue, QJsonValue("hole.png"));
        QCOMPARE(of("image").defaultValue, QJsonValue("a.png"));
    }

    void aFormStartsOnTheKindsOwnValue() {
        QCOMPARE(of("soft").initial, QJsonValue(false));                // a toggle is a bool, the spec's default
        QCOMPARE(of("sampler").initial, QJsonValue("euler"));           // a choice has one in force: the first of its words
        QCOMPARE(of("words").initial, QJsonValue("simple"));
        QCOMPARE(of("steps").initial, QJsonValue(4));
        QVERIFY(of("seed").initial.isUndefined());                      // {"auto": "random"} is no number
        QCOMPARE(of("fixed").initial, QJsonValue(7));
        QVERIFY(of("region").initial.isUndefined());
        QVERIFY(of("model").initial.isUndefined());
    }

    void aRunCannotDoWithoutAPictureOrAPromptThatHasNothing() {
        QVERIFY(!of("image").required);    // the workflow holds a picture
        QVERIFY(!of("region").required);   // optional: left out
        QVERIFY(!of("prompt").required);   // the workflow holds words
        QVERIFY(!of("steps").required);
        QVERIFY(!of("negative_prompt").required); // a negative prompt may be empty
        QVERIFY(!of("told").required);     // it has a fallback
        InputBinding empty;
        empty.role = "image";
        empty.targets = {PortRef{"load", "file"}};
        Document blank = m_kinds;
        blank.graph.nodes["load"].inputs.insert("file", InputValue::literal(QString()));
        QVERIFY(describeInput(empty, blank, &m_registry).required);        // a picture with none
        empty.optional = true;
        QVERIFY(!describeInput(empty, blank, &m_registry).required);
        InputBinding prompt;
        prompt.role = "prompt";
        prompt.targets = {PortRef{"blank", "text"}};
        QVERIFY(describeInput(prompt, m_kinds, &m_registry).required);      // a prompt with nothing in it and no fallback
        prompt.optional = true;
        QVERIFY(!describeInput(prompt, m_kinds, &m_registry).required);
    }

    void advancedRangeAndChoicesComeFromTheBlockOrTheTarget() {
        QVERIFY(of("steps").advanced);   // any case of the section's name
        QVERIFY(!of("bare").advanced);
        QCOMPARE(of("steps").binding.min, std::optional<double>(1));
        QCOMPARE(of("steps").binding.max, std::optional<double>(8));
        QCOMPARE(of("sampler").binding.choices->first(), QStringLiteral("euler")); // the target's own words
        QCOMPARE(*of("words").binding.choices, (QStringList{"karras", "simple"})); // the block's subset
    }

    void aGroupInstanceInputTakesTheInnerInputs() {
        const AppInput opacity = describeInput(*m_groupApp.input("opacity"), m_group, &m_registry);
        QCOMPARE(opacity.kind, QStringLiteral("number"));
        QCOMPARE(opacity.binding.min, std::optional<double>(0));
        QCOMPARE(opacity.binding.max, std::optional<double>(1));
        QCOMPARE(opacity.defaultValue, QJsonValue(0.5)); // the instance holds nothing: the inner default
        QCOMPARE(opacity.initial, QJsonValue(0.5));
        const AppInput mode = describeInput(*m_groupApp.input("mode"), m_group, &m_registry);
        QCOMPARE(mode.kind, QStringLiteral("choice"));
        QVERIFY(mode.binding.choices->contains("multiply"));
        QCOMPARE(mode.initial, QJsonValue("normal"));
    }

    void textTurnsIntoTheValueTheKindBinds() {
        QCOMPARE(*of("steps").fromText(" 6.5 "), QJsonValue(6.5));
        QCOMPARE(of("steps").fromText("lots").errors().first().code, QStringLiteral("usage"));
        QCOMPARE(*of("fixed").fromText("42"), QJsonValue(42));
        QCOMPARE(*of("seed").fromText("Random"), QJsonValue(QJsonObject{{"auto", "random"}})); // the engine's own
        QCOMPARE(*of("seed").fromText("auto"), QJsonValue(QJsonObject{{"auto", "random"}}));
        QVERIFY(!of("steps").fromText("random"));
        QCOMPARE(*of("soft").fromText(" TRUE "), QJsonValue(true));
        QVERIFY(!of("soft").fromText("yes"));
        QCOMPARE(*of("words").fromText("karras"), QJsonValue("karras"));
        QCOMPARE(*of("note").fromText("two words"), QJsonValue("two words"));
        // A model: a whole handle as JSON, else a name the caller resolves for the resource type the port takes.
        QCOMPARE(*of("model").fromText(R"({"id": "m"})"), QJsonValue(QJsonObject{{"id", "m"}}));
        QVERIFY(!of("model").fromText("flux.safetensors"));
        PortType asked;
        QString name;
        const auto lookup = [&](const PortType &type, const QString &word) -> Result<QJsonObject> {
            asked = type;
            name = word;
            return QJsonObject{{"path", "/m/flux"}};
        };
        QCOMPARE(*of("model").fromText("flux.safetensors", lookup), QJsonValue(QJsonObject{{"path", "/m/flux"}}));
        QCOMPARE(asked.toString(), QStringLiteral("fugue/model"));
        QCOMPARE(name, QStringLiteral("flux.safetensors"));
        // A picture is a file that exists, as an absolute path.
        QTemporaryFile file;
        QVERIFY(file.open());
        QCOMPARE(*of("image").fromText(file.fileName()), QJsonValue(file.fileName()));
        const Result<QJsonValue> missing = of("region").fromText("no/such.png");
        QVERIFY(!missing);
        QCOMPARE(missing.errors().first().code, QStringLiteral("file_not_found"));
        QVERIFY(missing.errors().first().message.contains("\"region\""));
    }

    void aTypedValueIsANumberOrNone() {
        QCOMPARE(of("steps").fromTyped(5), QJsonValue(5));
        QCOMPARE(of("steps").fromTyped("5.5"), QJsonValue(5.5));
        QVERIFY(of("steps").fromTyped("").isUndefined());
        QVERIFY(of("steps").fromTyped("abc").isUndefined());
        QVERIFY(of("seed").fromTyped(QVariant()).isUndefined());
        QCOMPARE(of("fixed").fromTyped(12), QJsonValue(12));
        QCOMPARE(of("soft").fromTyped(true), QJsonValue(true));
        QCOMPARE(of("note").fromTyped("words"), QJsonValue("words"));
    }

    void whatARunBindsIsTheGivenValueElseTheDefault() {
        QCOMPARE(of("steps").valueIn({{"steps", 9}}), QJsonValue(9));
        QCOMPARE(of("steps").valueIn({}), QJsonValue(4));
        QVERIFY(of("region").valueIn({}).isUndefined()); // an optional picture left out: the answer bindApp acts on
    }
};

QTEST_GUILESS_MAIN(TestAppInput)
#include "test_app_input.moc"
