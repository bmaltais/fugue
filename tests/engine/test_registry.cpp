// Versions (spec 3.6), the node manifest's inputs and outputs (6.1, 6.2), literal
// checks, and the in-memory registry (6.12) with fake node types.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

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

    void manifestParses() {
        Errors errors;
        const std::optional<NodeType> type = NodeType::fromJson(object(sharpen), errors);
        QVERIFY2(type, qPrintable(dump(errors)));
        QVERIFY2(errors.isEmpty(), qPrintable(dump(errors)));
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
        Errors errors;
        const QJsonObject manifest = object(sharpen);
        QCOMPARE(NodeType::fromJson(manifest, errors)->toJson(), manifest);
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
        QVERIFY(!NodeType::fromJson(manifest, errors));

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

        Errors errors;
        const std::optional<NodeType> type = NodeType::fromJson(manifest, errors);
        QVERIFY2(type, qPrintable(dump(errors)));
        QCOMPARE(errors.size(), 2);
        QVERIFY(!hasErrors(errors));
        QVERIFY(type->input("amount"));
    }

    void outputsCannotBeAny() {
        Errors errors;
        QJsonObject manifest = object(sharpen);
        manifest["outputs"] = QJsonArray{QJsonObject{{"name", "out"}, {"type", "any"}}};
        QVERIFY(!NodeType::fromJson(manifest, errors));
    }

    void newerManifestFormatIsRefused() {
        QJsonObject manifest = object(sharpen);
        manifest["manifest"] = 2;
        Errors errors;
        QVERIFY(!NodeType::fromJson(manifest, errors));
        QCOMPARE(codes(errors), QStringList{"unsupported_format_version"});
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
        const Errors errors = registry.add(fakeType("fugue/text", "2.0.0"));
        QVERIFY(hasErrors(errors));
        QCOMPARE(registry.nodeType("fugue/text")->version, *Version::parse("1.0.0"));
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
                                              PortKind::Data, "Latent", {}})
                    .isEmpty());
        QVERIFY(registry.add(sampler).isEmpty());
        QCOMPARE(registry.portKind(*PortType::parse("io.github.alice.tools/latent@2")), PortKind::Data);
        QCOMPARE(registry.portKind(*PortType::parse("io.github.alice.tools/latent")), PortKind::Unknown);
        QCOMPARE(registry.portKind(*PortType::parse("image")), PortKind::Builtin);
        QCOMPARE(registry.portKind(*PortType::parse("any")), PortKind::Any);

        // The same declared type twice is refused; the built-ins cannot be declared.
        QVERIFY(hasErrors(registry.add(DeclaredPortType{
            *PortType::parse("io.github.alice.tools/latent@2"), PortKind::Data, "Latent", {}})));
        QVERIFY(hasErrors(registry.add(DeclaredPortType{*PortType::parse("image"), PortKind::Data, "", {}})));
        QVERIFY(hasErrors(registry.add(DeclaredPortType{*PortType::parse("fugue/x"), PortKind::Any, "", {}})));
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
};

QTEST_GUILESS_MAIN(TestRegistry)
#include "test_registry.moc"
