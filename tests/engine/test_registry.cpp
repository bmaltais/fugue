// Versions (spec 3.6), the node manifest's inputs and outputs (6.1, 6.2), literal
// checks, and the in-memory registry (6.12) with fake node types.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "engine/fake_provider.h"
#include "engine/node_type.h"
#include "engine/registry.h"
#include "engine/version.h"

using namespace fugue;

namespace {

QJsonObject object(const char *json) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (!document.isObject())
        qFatal("bad test JSON: %s", qPrintable(error.errorString()));
    return document.object();
}

// The example of spec 6.1.
const char *sharpen = "{"
                      "\"manifest\": 1,"
                      "\"id\": \"io.github.alice.tools/sharpen\","
                      "\"version\": \"1.2.0\","
                      "\"name\": \"Sharpen\","
                      "\"description\": \"Unsharp mask.\","
                      "\"category\": \"Image/Filter\","
                      "\"runtime\": {\"kind\": \"command\", \"entry\": \"./sharpen.sh\"},"
                      "\"inputs\": ["
                      "  {\"name\": \"image\", \"type\": \"image\"},"
                      "  {\"name\": \"amount\", \"type\": \"number\", \"default\": 1.5, \"min\": 0, \"max\": 5,"
                      "   \"step\": 0.1, \"widget\": \"slider\"}"
                      "],"
                      "\"outputs\": [{\"name\": \"image\", \"type\": \"image\"}],"
                      "\"needs\": {\"memory_mb\": 512}"
                      "}";

QStringList codes(const Errors &errors) {
    QStringList list;
    for (const Error &error : errors)
        list << error.code;
    return list;
}

QString dump(const Errors &errors) {
    return QString::fromUtf8(QJsonDocument(errorEnvelope(errors)).toJson(QJsonDocument::Compact));
}

bool mentions(const Errors &errors, const QString &input, const QString &word) {
    for (const Error &error : errors)
        if (error.input == input && error.message.contains(word, Qt::CaseInsensitive))
            return true;
    return false;
}

InputSpec numberInput() {
    InputSpec input;
    input.name = "steps";
    input.type = PortSpec{*PortType::parse("number"), false};
    input.min = 1;
    input.max = 50;
    input.integer = true;
    return input;
}

NodeType fakeType(const char *id, const char *version = "1.0.0") {
    NodeType type;
    type.id = QString::fromLatin1(id);
    type.version = *Version::parse(QString::fromLatin1(version));
    type.name = "Fake";
    type.runtime = QJsonObject{{"kind", "builtin"}};
    return type;
}

} // namespace

class TestRegistry : public QObject {
    Q_OBJECT

private slots:
    void versionsAreSemver() {
        const Version v = *Version::parse("1.12.3");
        QCOMPARE(v.major, 1);
        QCOMPARE(v.minor, 12);
        QCOMPARE(v.patch, 3);
        QCOMPARE(v.toString(), QStringLiteral("1.12.3"));
        QVERIFY(*Version::parse("1.2.0") < *Version::parse("1.10.0"));
        QVERIFY(*Version::parse("2.0.0") > *Version::parse("1.99.99"));
        QCOMPARE(*Version::parse("0.1.0"), *Version::parse("0.1.0"));
        for (const char *bad : {"1.2", "1.2.3.4", "01.2.3", "1.2.x", "v1.2.3", "", "1.2.3-beta"})
            QVERIFY2(!Version::parse(QString::fromLatin1(bad)), bad);
    }

    void aChoiceInputsListCanBeReplacedAfterTheTypeIsRegistered() {
        Registry registry;
        QVERIFY(registry.addManifest(object("{\"manifest\": 1, \"id\": \"io.github.alice.tools/pick\", \"version\": \"1.0.0\", \"name\": \"Pick\","
                                            "\"description\": \"Pick.\", \"category\": \"Text\", \"runtime\": {\"kind\": \"command\", \"entry\": \"./pick.sh\"},"
                                            "\"inputs\": [{\"name\": \"style\", \"type\": \"text\", \"default\": \"a\","
                                            "\"choices\": [\"a\"], \"description\": \"old\"},"
                                            "{\"name\": \"free\", \"type\": \"text\", \"default\": \"\"}],"
                                            "\"outputs\": [{\"name\": \"out\", \"type\": \"text\"}]}")).isEmpty());
        registry.setInputChoices("io.github.alice.tools/pick", "style", {"a", "b"}, "new");
        registry.setInputChoices("io.github.alice.tools/pick", "style", {}, "ignored");            // empty list: no change
        registry.setInputChoices("io.github.alice.tools/pick", "free", {"x"}, "not a choice");     // not a choice input
        registry.setInputChoices("io.github.alice.tools/none", "style", {"x"});                    // not a type
        const NodeType *type = registry.nodeType("io.github.alice.tools/pick");
        QCOMPARE(*type->inputs.at(0).choices, (QStringList{"a", "b"}));
        QCOMPARE(type->inputs.at(0).description, QString("new"));
        QVERIFY(!type->inputs.at(1).choices);
        registry.setInputChoices("io.github.alice.tools/pick", "style", {"c"}); // no description given: it stays
        QCOMPARE(type->inputs.at(0).description, QString("new"));
    }

    void manifestParses() {
        const Result<NodeType> type = NodeType::fromJson(object(sharpen));
        QVERIFY2(type.ok(), qPrintable(dump(type.errors())));
        QVERIFY2(type.warnings().isEmpty(), qPrintable(dump(type.warnings())));
        QCOMPARE(type->id, QStringLiteral("io.github.alice.tools/sharpen"));
        QCOMPARE(type->version, *Version::parse("1.2.0"));
        QCOMPARE(type->inputs.size(), 2);
        QCOMPARE(type->outputs.size(), 1);
        QVERIFY(!type->sink);
        QVERIFY(type->deterministic);

        const InputSpec *amount = type->input("amount");
        QVERIFY(amount);
        QCOMPARE(amount->type.toString(), QStringLiteral("number"));
        QCOMPARE(amount->defaultValue, QJsonValue(1.5));
        QCOMPARE(amount->max, std::optional<double>(5));
        QCOMPARE(amount->widget(), QStringLiteral("slider"));
        QVERIFY(!amount->required());

        const InputSpec *image = type->input("image");
        QVERIFY(image->required());
        QCOMPARE(image->widget(), QString()); // media inputs are wire-only
        QVERIFY(!type->input("missing"));
        QCOMPARE(type->output("image")->type.type.toString(), QStringLiteral("image"));
    }

    void manifestWritesBackAsData() {
        const QJsonObject manifest = object(sharpen);
        QCOMPARE(NodeType::fromJson(manifest)->toJson(), manifest);
    }

    void defaultWidgetsFollowTheType() {
        InputSpec input;
        input.type = PortSpec{*PortType::parse("number"), false};
        QCOMPARE(input.widget(), QStringLiteral("spin"));
        input.type = PortSpec{*PortType::parse("text"), false};
        QCOMPARE(input.widget(), QStringLiteral("line"));
        input.choices = QStringList{"a", "b"};
        QCOMPARE(input.widget(), QStringLiteral("choice"));
        input.type = PortSpec{*PortType::parse("boolean"), false};
        input.choices.reset();
        QCOMPARE(input.widget(), QStringLiteral("toggle"));
        input.optional = true;
        QVERIFY(!input.required());
    }

    void manifestErrorsAreAllReported() {
        const QJsonObject manifest = object(
            "{\"manifest\": 1, \"id\": \"io.github.alice.tools/broken\", \"version\": \"1.0\","
            " \"name\": \"Broken\", \"ui\": {},"
            " \"inputs\": ["
            "  {\"name\": \"Bad-Name\", \"type\": \"text\"},"
            "  {\"name\": \"dup\", \"type\": \"text\"},"
            "  {\"name\": \"dup\", \"type\": \"text\"},"
            "  {\"name\": \"kind\", \"type\": \"mask\"},"
            "  {\"name\": \"count\", \"type\": \"number\", \"choices\": [\"a\"]},"
            "  {\"name\": \"flag\", \"type\": \"boolean\", \"widget\": \"slider\"},"
            "  {\"name\": \"range\", \"type\": \"number\", \"min\": 5, \"max\": 1},"
            "  {\"name\": \"steps\", \"type\": \"number\", \"default\": 60, \"max\": 50},"
            "  {\"name\": \"pic\", \"type\": \"image\", \"default\": \"x.png\"},"
            "  {\"name\": \"typo\", \"type\": \"text\", \"defualt\": \"x\"}"
            " ],"
            " \"outputs\": [{\"name\": \"out\", \"type\": \"any\"}, {\"type\": \"text\"}]"
            "}");
        Errors errors;
        errors += NodeType::fromJson(manifest).errors();
        QVERIFY(hasErrors(errors));

        const QStringList found = codes(errors);
        QVERIFY2(found.count("invalid_document") >= 10, qPrintable(dump(errors)));
        QVERIFY2(found.contains("value_out_of_range"), qPrintable(dump(errors)));
        for (const char *word : {"version", "runtime", "ui"})
            QVERIFY2(dump(errors).contains(QString::fromLatin1(word)), word);
        QVERIFY(mentions(errors, "Bad-Name", "name"));
        QVERIFY(mentions(errors, "dup", "more than once"));
        QVERIFY(mentions(errors, "kind", "mask"));
        QVERIFY(mentions(errors, "count", "choices"));
        QVERIFY(mentions(errors, "flag", "slider"));
        QVERIFY(mentions(errors, "range", "min"));
        QVERIFY(mentions(errors, "pic", "default"));

        // An unknown field is a warning, not a failure on its own.
        bool warned = false;
        for (const Error &error : errors)
            warned |= error.input == "typo" && error.severity == Error::Severity::Warning;
        QVERIFY2(warned, qPrintable(dump(errors)));
        for (const Error &error : errors)
            QVERIFY2(error.message.contains("io.github.alice.tools/broken"), qPrintable(error.message));
    }

    void anUnknownFieldOnlyWarns() {
        QJsonObject manifest = object(sharpen);
        QJsonArray inputs = manifest["inputs"].toArray();
        QJsonObject amount = inputs[1].toObject();
        amount["defualt"] = 2;
        inputs[1] = amount;
        manifest["inputs"] = inputs;
        manifest["homepage"] = "https://example.org";

        const Result<NodeType> type = NodeType::fromJson(manifest);
        QVERIFY2(type.ok(), qPrintable(dump(type.errors())));
        const Errors errors = type.warnings(); // a manifest that parses carries its warnings
        QCOMPARE(errors.size(), 2);
        QVERIFY(!hasErrors(errors));
        QVERIFY(type->input("amount"));
    }

    void outputsCannotBeAny() {
        Errors errors;
        QJsonObject manifest = object(sharpen);
        manifest["outputs"] = QJsonArray{QJsonObject{{"name", "out"}, {"type", "any"}}};
        errors += NodeType::fromJson(manifest).errors();
        QVERIFY(hasErrors(errors));
    }

    void newerManifestFormatIsRefused() {
        QJsonObject manifest = object(sharpen);
        manifest["manifest"] = 2;
        Errors errors;
        errors += NodeType::fromJson(manifest).errors();
        QVERIFY(hasErrors(errors));
        QCOMPARE(codes(errors), QStringList{"unsupported_format_version"});

        manifest["manifest"] = 1.5;
        errors.clear();
        errors += NodeType::fromJson(manifest).errors();
        QVERIFY(hasErrors(errors));
        QCOMPARE(codes(errors), QStringList{"invalid_document"});
    }

    void literalNumbers() {
        const InputSpec steps = numberInput();
        QVERIFY(checkLiteral(steps, 20, PortKind::Builtin).isEmpty());
        QCOMPARE(codes(checkLiteral(steps, 0, PortKind::Builtin)), QStringList{"value_out_of_range"});
        QCOMPARE(codes(checkLiteral(steps, 51, PortKind::Builtin)), QStringList{"value_out_of_range"});
        QCOMPARE(codes(checkLiteral(steps, 2.5, PortKind::Builtin)), QStringList{"value_out_of_range"});
        QCOMPARE(codes(checkLiteral(steps, "20", PortKind::Builtin)), QStringList{"type_mismatch"});
        QCOMPARE(checkLiteral(steps, 0, PortKind::Builtin).first().input, QStringLiteral("steps"));

        InputSpec seed;
        seed.name = "seed";
        seed.type = PortSpec{*PortType::parse("number"), false};
        seed.integer = true;
        QVERIFY(checkLiteral(seed, qint64(9007199254740991), PortKind::Builtin).isEmpty());
        QCOMPARE(codes(checkLiteral(seed, 9007199254740992.0 * 2, PortKind::Builtin)),
                 QStringList{"value_out_of_range"});
    }

    void literalChoicesListEveryOption() {
        InputSpec method;
        method.name = "method";
        method.type = PortSpec{*PortType::parse("text"), false};
        method.choices = QStringList{"nearest", "bilinear", "bicubic", "lanczos", "area"};
        QVERIFY(checkLiteral(method, "lanczos", PortKind::Builtin).isEmpty());

        const Errors errors = checkLiteral(method, "lancos", PortKind::Builtin);
        QCOMPARE(codes(errors), QStringList{"value_not_in_choices"});
        QCOMPARE(errors.first().valid,
                 (QJsonArray{"nearest", "bilinear", "bicubic", "lanczos", "area"}));
        QCOMPARE(errors.first().hint, QStringLiteral("Did you mean \"lanczos\"?"));
        QCOMPARE(checkLiteral(method, "zzzzzzzz", PortKind::Builtin).first().hint, QString());
    }

    void literalColorsAreHexWithOptionalAlpha() {
        // 6.2: a color widget's value is #rrggbb or #rrggbbaa.
        InputSpec tint;
        tint.name = "tint";
        tint.type = PortSpec{*PortType::parse("text"), false};
        tint.widgetName = "color";
        QVERIFY(checkLiteral(tint, "#1a2B3c", PortKind::Builtin).isEmpty());
        QVERIFY(checkLiteral(tint, "#1a2b3c80", PortKind::Builtin).isEmpty());
        for (const char *bad : {"red", "#fff", "1a2b3c", "#1a2b3g", "#1a2b3c8", " #1a2b3c"})
            QCOMPARE(codes(checkLiteral(tint, bad, PortKind::Builtin)), QStringList{"type_mismatch"});
        // A manifest whose color default is not one fails to load.
        QJsonObject manifest = object(sharpen);
        QJsonArray inputs = manifest["inputs"].toArray();
        inputs.append(QJsonObject{{"name", "tint"}, {"type", "text"}, {"widget", "color"}, {"default", "red"}});
        manifest["inputs"] = inputs;
        QVERIFY(!NodeType::fromJson(manifest).ok());
        // A plain text input takes any text.
        tint.widgetName.clear();
        QVERIFY(checkLiteral(tint, "red", PortKind::Builtin).isEmpty());
    }

    void literalOtherForms() {
        InputSpec flag;
        flag.name = "flag";
        flag.type = PortSpec{*PortType::parse("boolean"), false};
        QVERIFY(checkLiteral(flag, true, PortKind::Builtin).isEmpty());
        QCOMPARE(codes(checkLiteral(flag, 1, PortKind::Builtin)), QStringList{"type_mismatch"});

        // {"auto": "random"} only for an input with the seed widget.
        const QJsonObject random{{"auto", "random"}};
        InputSpec seed = numberInput();
        QCOMPARE(codes(checkLiteral(seed, random, PortKind::Builtin)), QStringList{"type_mismatch"});
        seed.widgetName = "seed";
        QVERIFY(checkLiteral(seed, random, PortKind::Builtin).isEmpty());

        // Media and data inputs take a link only.
        InputSpec image;
        image.name = "image";
        image.type = PortSpec{*PortType::parse("image"), false};
        QCOMPARE(codes(checkLiteral(image, "x.png", PortKind::Builtin)), QStringList{"type_mismatch"});
        InputSpec latent;
        latent.name = "latent";
        latent.type = PortSpec{*PortType::parse("fugue/latent"), false};
        QCOMPARE(codes(checkLiteral(latent, QJsonObject{{"resource", "fugue/latent"}}, PortKind::Data)),
                 QStringList{"type_mismatch"});

        // A resource input takes a handle of its own type.
        InputSpec lora;
        lora.name = "lora";
        lora.type = PortSpec{*PortType::parse("fugue/lora"), false};
        const QJsonObject handle{{"resource", "fugue/lora"}, {"name", "detail.safetensors"}};
        QVERIFY(checkLiteral(lora, handle, PortKind::Resource).isEmpty());
        QCOMPARE(codes(checkLiteral(lora, QJsonObject{{"resource", "fugue/vae"}}, PortKind::Resource)),
                 QStringList{"type_mismatch"});
        QCOMPARE(codes(checkLiteral(lora, "detail.safetensors", PortKind::Resource)),
                 QStringList{"type_mismatch"});

        // An "any" input takes any literal.
        InputSpec any;
        any.name = "value";
        any.type = PortSpec{*PortType::parse("any"), false};
        QVERIFY(checkLiteral(any, "text", PortKind::Any).isEmpty());
    }

    void registryRegistersFakeNodeTypes() {
        Registry registry;
        QVERIFY(registry.addManifest(object(sharpen)).isEmpty());
        const NodeType *type = registry.nodeType("io.github.alice.tools/sharpen");
        QVERIFY(type);
        QCOMPARE(type->name, QStringLiteral("Sharpen"));
        QVERIFY(!registry.nodeType("io.github.alice.tools/blur"));

        QVERIFY(registry.add(fakeType("fugue/text")).isEmpty());
        QStringList ids;
        for (const NodeType *each : registry.nodeTypes())
            ids << each->id;
        QCOMPARE(ids, (QStringList{"fugue/text", "io.github.alice.tools/sharpen"}));
    }

    void registryRefusesADuplicateId() {
        Registry registry;
        QVERIFY(registry.add(fakeType("fugue/text")).isEmpty());
        const Errors errors = registry.add(fakeType("fugue/text", "1.4.0"));
        QVERIFY(hasErrors(errors));
        QCOMPARE(registry.nodeType("fugue/text")->version, *Version::parse("1.0.0"));
    }

    // 3.6: several majors of a type are installed at once; the newest answers by default, a
    // node recorded at an installed major gets that one, and one at a major that is not
    // installed gets the newest (it needs review).
    void severalMajorsOfATypeAreInstalledSideBySide() {
        Registry registry;
        QVERIFY(registry.add(fakeType("fugue/text", "2.1.0")).isEmpty());
        QVERIFY(registry.add(fakeType("fugue/text", "1.3.0")).isEmpty());
        QVERIFY(registry.add(fakeType("fugue/text", "3.0.0")).isEmpty());
        QCOMPARE(registry.majors("fugue/text"), (QList<int>{1, 2, 3}));
        QCOMPARE(registry.nodeType("fugue/text")->version, *Version::parse("3.0.0"));
        QCOMPARE(registry.nodeTypeAt("fugue/text", 1)->version, *Version::parse("1.3.0"));
        QVERIFY(!registry.nodeTypeAt("fugue/text", 4));
        QCOMPARE(registry.nodeTypeFor("fugue/text", 2)->version, *Version::parse("2.1.0"));
        QCOMPARE(registry.nodeTypeFor("fugue/text", 9)->version, *Version::parse("3.0.0"));
        QVERIFY(!registry.nodeTypeFor("fugue/none", 1));
        // Search and the library list each type once, at its newest major.
        QCOMPARE(registry.nodeTypes().size(), 1);
        QCOMPARE(registry.allNodeTypes().size(), 3);
        // The same major twice is still refused.
        QVERIFY(hasErrors(registry.add(fakeType("fugue/text", "2.2.0"))));
    }

    void anAliasReachesEveryMajorOfItsType() {
        Registry registry;
        NodeType v1 = fakeType("io.github.alice.tools/new", "1.0.0");
        v1.aliases = QStringList{"io.github.alice.tools/old"};
        NodeType v2 = fakeType("io.github.alice.tools/new", "2.0.0");
        v2.aliases = v1.aliases;
        QVERIFY(registry.add(v1).isEmpty());
        QVERIFY(registry.add(v2).isEmpty()); // its own alias again is no conflict
        QCOMPARE(registry.nodeTypeAt("io.github.alice.tools/old", 1)->version, *Version::parse("1.0.0"));
        QCOMPARE(registry.nodeType("io.github.alice.tools/old")->version, *Version::parse("2.0.0"));
    }

    void packVersionsAreKeptPerMajor() {
        Registry registry;
        registry.setPackVersion("io.github.alice.tools", "2.0.0");
        registry.setPackVersion("io.github.alice.tools", "1.4.0");
        QCOMPARE(registry.packVersions("io.github.alice.tools"), (QStringList{"1.4.0", "2.0.0"}));
        QCOMPARE(registry.packVersion("io.github.alice.tools"), QStringLiteral("2.0.0"));
        QVERIFY(registry.hasPackMajor("io.github.alice.tools", 1));
        QVERIFY(!registry.hasPackMajor("io.github.alice.tools", 3));
        registry.setPackSource("io.github.alice.tools", "1.4.0", "https://example.org/a", "v1.4.0");
        QCOMPARE(registry.packSource("io.github.alice.tools", "1.4.0"), QStringLiteral("https://example.org/a"));
        QVERIFY(registry.packSource("io.github.alice.tools").isEmpty()); // the newest has none
    }

    void registryRefusesABadManifest() {
        Registry registry;
        QJsonObject manifest = object(sharpen);
        manifest.remove("runtime");
        QVERIFY(hasErrors(registry.addManifest(manifest)));
        QVERIFY(!registry.nodeType("io.github.alice.tools/sharpen"));
    }

    void declaredPortTypesMustBeRegistered() {
        Registry registry;
        NodeType sampler = fakeType("io.github.alice.tools/sampler");
        InputSpec latent;
        latent.name = "latent";
        latent.type = PortSpec{*PortType::parse("io.github.alice.tools/latent@2"), false};
        sampler.inputs << latent;

        const Errors unknown = registry.add(sampler);
        QVERIFY(hasErrors(unknown));
        QVERIFY2(dump(unknown).contains("io.github.alice.tools/latent@2"), qPrintable(dump(unknown)));

        QVERIFY(registry.add(DeclaredPortType{*PortType::parse("io.github.alice.tools/latent@2"),
                                              PortKind::Data, "Latent", {}, {}, {}})
                    .isEmpty());
        QVERIFY(registry.add(sampler).isEmpty());
        QCOMPARE(registry.portKind(*PortType::parse("io.github.alice.tools/latent@2")), PortKind::Data);
        QCOMPARE(registry.portKind(*PortType::parse("io.github.alice.tools/latent")), PortKind::Unknown);
        QCOMPARE(registry.portKind(*PortType::parse("image")), PortKind::Builtin);
        QCOMPARE(registry.portKind(*PortType::parse("any")), PortKind::Any);
        QVERIFY(registry.add(DeclaredPortType{*PortType::parse("io.github.alice.tools/lora"), PortKind::Resource,
                                              "LoRA", {}, {}, {}})
                    .isEmpty());
        // Every declared type, sorted by id (what `fugue model list` goes through).
        QStringList declared;
        for (const DeclaredPortType *each : registry.portTypes())
            declared << each->type.toString();
        QCOMPARE(declared, (QStringList{"io.github.alice.tools/latent@2", "io.github.alice.tools/lora"}));

        // The same declared type twice is refused; the built-ins cannot be declared.
        QVERIFY(hasErrors(registry.add(DeclaredPortType{
            *PortType::parse("io.github.alice.tools/latent@2"), PortKind::Data, "Latent", {}, {}, {}})));
        QVERIFY(hasErrors(registry.add(DeclaredPortType{*PortType::parse("image"), PortKind::Data, "", {}, {}, {}})));
        QVERIFY(hasErrors(registry.add(DeclaredPortType{*PortType::parse("fugue/x"), PortKind::Any, "", {}, {}, {}})));
    }

    void onlyBuiltInInputsMayBeAny() {
        InputSpec value;
        value.name = "value";
        value.type = PortSpec{*PortType::parse("any"), false};

        NodeType preview = fakeType("fugue/preview");
        preview.sink = true;
        preview.inputs << value;
        NodeType packPreview = fakeType("io.github.alice.tools/preview");
        packPreview.inputs << value;

        Registry registry;
        QVERIFY(registry.add(preview).isEmpty());
        QVERIFY(hasErrors(registry.add(packPreview)));
    }

    // `takes_list` (a Switch side takes a list as one value) is for a single-value "any" input of a built-in node.
    void takesListIsOnlyForBuiltInAnyInputs() {
        const auto manifest = [](const char *id, const char *input) {
            return object(QByteArray("{\"manifest\": 1, \"id\": \"") + id + "\", \"version\": \"1.0.0\", \"name\": \"N\","
                          "\"description\": \"N.\", \"category\": \"Text\", \"runtime\": {\"kind\": \"command\", \"entry\": \"./n.sh\"},"
                          "\"inputs\": [" + input + "], \"outputs\": [{\"name\": \"out\", \"type\": \"text\"}]}");
        };
        const char *id = "io.github.alice.tools/n";
        const Result<NodeType> notAny = NodeType::fromJson(manifest(id, "{\"name\": \"a\", \"type\": \"text\", \"takes_list\": true}"));
        QVERIFY(!notAny.ok());
        QVERIFY2(dump(notAny.errors()).contains("takes_list"), qPrintable(dump(notAny.errors())));
        const Result<NodeType> list = NodeType::fromJson(manifest(id, "{\"name\": \"a\", \"type\": \"any\", \"list\": true, \"takes_list\": true}"));
        QVERIFY(!list.ok());
        QVERIFY2(dump(list.errors()).contains("takes_list"), qPrintable(dump(list.errors())));

        InputSpec side;
        side.name = "side";
        side.type = PortSpec{*PortType::parse("any"), false, true};
        NodeType builtin = fakeType("fugue/sidey");
        builtin.inputs << side;
        NodeType pack = fakeType("io.github.alice.tools/sidey");
        pack.inputs << side;
        Registry registry;
        QVERIFY(registry.add(builtin).isEmpty());
        const Errors refused = registry.add(pack);
        QVERIFY(hasErrors(refused));
        QVERIFY2(dump(refused).contains("takes_list"), qPrintable(dump(refused)));
    }

    // 6.12: one registry of node types, port types and providers.
    void theRegistryHoldsTheConfiguredProvidersInTheUsersOrder() {
        Registry registry;
        QVERIFY(registry.providers().isEmpty());
        ProviderInstance studio;
        studio.name = "Studio";
        studio.type = "fake";
        ProviderInstance local;
        local.name = "This machine";
        local.type = "fake";
        local.enabled = false;
        const auto provider = std::make_shared<FakeProvider>(Description{});
        QVERIFY(registry.setProviders({ProviderEntry{studio, provider}, ProviderEntry{local, nullptr}}).isEmpty());
        QCOMPARE(registry.providerNames(), (QStringList{"Studio", "This machine"}));
        QCOMPARE(registry.providers().at(0).provider.get(), provider.get());
        QVERIFY(!registry.providers().at(1).instance.enabled);

        // Setting them again replaces the list (providers.json was edited).
        QVERIFY(registry.setProviders({ProviderEntry{local, nullptr}}).isEmpty());
        QCOMPARE(registry.providerNames(), (QStringList{"This machine"}));
    }

    void providerNamesMustBeUniqueAndSet() {
        Registry registry;
        ProviderInstance one;
        one.name = "Studio";
        one.type = "fake";
        QVERIFY(registry.setProviders({ProviderEntry{one, nullptr}}).isEmpty());
        const Errors twice = registry.setProviders({ProviderEntry{one, nullptr}, ProviderEntry{one, nullptr}});
        QCOMPARE(twice.size(), 1);
        QCOMPARE(twice.first().code, QString("invalid_document"));
        ProviderInstance nameless;
        nameless.type = "fake";
        QVERIFY(hasErrors(registry.setProviders({ProviderEntry{nameless, nullptr}})));
        QCOMPARE(registry.providerNames(), (QStringList{"Studio"})); // unchanged by a refused list
    }

    void aliasesAnswerOnlyWhenNoTypeHasTheId() {
        Registry registry;
        NodeType renamed = fakeType("io.github.alice.tools/sharpen-pro");
        renamed.aliases = QStringList{"io.github.alice.tools/sharpen-old", "fugue/text"};
        QVERIFY(registry.add(renamed).isEmpty());
        QVERIFY(registry.add(fakeType("fugue/text")).isEmpty());

        QCOMPARE(registry.nodeType("io.github.alice.tools/sharpen-old")->id,
                 QStringLiteral("io.github.alice.tools/sharpen-pro"));
        QCOMPARE(registry.nodeType("fugue/text")->id, QStringLiteral("fugue/text"));

        // A second claim on the same alias is a warning; the first claim keeps it.
        NodeType other = fakeType("io.github.bob.tools/sharpen");
        other.aliases = QStringList{"io.github.alice.tools/sharpen-old"};
        const Errors errors = registry.add(other);
        QVERIFY(!errors.isEmpty());
        QVERIFY(!hasErrors(errors));
        QVERIFY(registry.nodeType("io.github.bob.tools/sharpen"));
        QCOMPARE(registry.nodeType("io.github.alice.tools/sharpen-old")->id,
                 QStringLiteral("io.github.alice.tools/sharpen-pro"));
    }

    // A reload of the packs (6.5): the types, port types and pack versions are the new ones,
    // the providers stay, and sameTypes() tells whether anything changed at all.
    void aReloadTakesTheNewTypesAndKeepsTheProviders() {
        Registry registry;
        QVERIFY(registry.add(fakeType("io.github.alice.tools/old")).isEmpty());
        registry.setPackVersion("io.github.alice.tools", "1.0.0");
        ProviderInstance studio;
        studio.name = "Studio";
        studio.type = "fake";
        QVERIFY(registry.setProviders({ProviderEntry{studio, nullptr}}).isEmpty());

        auto fresh = [] {
            Registry next;
            DeclaredPortType mask;
            mask.type = *PortType::parse("io.github.alice.tools/mask");
            mask.kind = PortKind::Data;
            mask.name = "Mask";
            next.add(mask);
            NodeType renamed = fakeType("io.github.alice.tools/new", "1.1.0");
            renamed.aliases = QStringList{"io.github.alice.tools/old"};
            next.add(renamed);
            next.setNodeDir("io.github.alice.tools/new", "/packs/alice/new", "seal-1");
            next.setPackVersion("io.github.alice.tools", "1.1.0");
            return next;
        };
        QVERIFY(!registry.sameTypes(fresh()));
        registry.replaceTypes(fresh());
        QVERIFY(registry.sameTypes(fresh()));
        QCOMPARE(registry.nodeType("io.github.alice.tools/old")->id, QStringLiteral("io.github.alice.tools/new"));
        QCOMPARE(registry.nodeTypes().size(), 1);
        QVERIFY(registry.portType(*PortType::parse("io.github.alice.tools/mask")));
        QCOMPARE(registry.packVersion("io.github.alice.tools"), QStringLiteral("1.1.0"));
        QCOMPARE(registry.providerNames(), QStringList{"Studio"});

        // A node's files changing on disk (its seal) is a change; so is a pack version alone.
        Registry resealed = fresh();
        resealed.setNodeDir("io.github.alice.tools/new", "/packs/alice/new", "seal-2");
        QVERIFY(!registry.sameTypes(resealed));
        Registry bumped = fresh();
        bumped.setPackVersion("io.github.alice.tools", "1.1.1");
        QVERIFY(!registry.sameTypes(bumped));
    }
};

QTEST_GUILESS_MAIN(TestRegistry)
#include "test_registry.moc"
