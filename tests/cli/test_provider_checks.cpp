// What `fugue validate` says about providers (spec 8.5): a model or capability that
// nothing serves is a warning, never an error, because validate executes nothing.
//
// Seam: providerWarnings(), a pure function of a compiled graph, the registry and
// provider descriptions already fetched (the CLI fetches them; no network here).

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include "cli/compile_step.h"
#include "engine/compiler.h"
#include "engine/providers.h"

using namespace fugue;
using namespace fugue::cli;

namespace {

QJsonObject handle(const QString &name) {
    return QJsonObject{{"resource", "fugue/image-model"}, {"name", name}};
}

Registry registry() {
    Registry r;
    DeclaredPortType model;
    model.type = *PortType::parse("fugue/image-model");
    model.kind = PortKind::Resource;
    model.name = "Image model";
    model.extensions = {"gguf"};
    if (hasErrors(r.add(model)))
        qFatal("port type");
    const QJsonObject manifest{
        {"manifest", 1},
        {"id", "fugue/gen"},
        {"version", "1.0.0"},
        {"name", "Generate"},
        {"runtime", QJsonObject{{"kind", "builtin"}}},
        {"capabilities", QJsonArray{"image.generate"}},
        {"inputs", QJsonArray{QJsonObject{{"name", "model"}, {"type", "fugue/image-model"}, {"optional", true}}}},
        {"outputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}},
        {"sink", true},
    };
    if (hasErrors(r.addManifest(manifest)))
        qFatal("manifest");
    return r;
}

CompiledGraph compiled(const Registry &r, const QString &model) {
    Nodes nodes;
    Node node;
    node.type = "fugue/gen";
    node.version = *Version::parse("1.0.0");
    if (!model.isEmpty())
        node.inputs["model"] = InputValue::resource(handle(model));
    nodes["g"] = node;
    Document document;
    document.graph.nodes = nodes;
    Compilation result = compile(document, r);
    if (!result.ok())
        qFatal("does not compile");
    return *result.graph;
}

Candidate offering(const QString &name, const QString &status) {
    OfferedModel offered;
    offered.handle = handle(name);
    offered.status = status;
    CapabilityOffer offer;
    offer.capability = "image.generate";
    offer.models.append(offered);
    Description description;
    description.engine = "sdcpp";
    description.capabilities.append(offer);
    Candidate candidate;
    candidate.instance.name = "box";
    candidate.instance.type = "omarchy";
    candidate.description = description;
    return candidate;
}

QStringList codes(const Errors &errors) {
    QStringList list;
    for (const Error &error : errors)
        list << error.code + ":" + error.node;
    return list;
}

} // namespace

class TestProviderChecks : public QObject {
    Q_OBJECT

private slots:
    void aServedModelGivesNoWarning() {
        const Registry r = registry();
        QVERIFY(providerWarnings(compiled(r, "flux"), r, {offering("flux", "ready")}).isEmpty());
    }

    void noModelRequestedNeedsOnlyTheCapability() {
        const Registry r = registry();
        QVERIFY(providerWarnings(compiled(r, {}), r, {offering("flux", "ready")}).isEmpty());
    }

    void aMissingModelIsAWarningNamingTheNode() {
        const Registry r = registry();
        const Errors warnings = providerWarnings(compiled(r, "flux"), r, {offering("other", "ready")});
        QCOMPARE(codes(warnings), (QStringList{"model_missing:g"}));
        QCOMPARE(warnings.at(0).severity, Error::Severity::Warning);
        QVERIFY(!hasErrors(warnings));
    }

    void aModelThatIsNotRunningIsAWarning() {
        const Registry r = registry();
        const Errors warnings = providerWarnings(compiled(r, "flux"), r, {offering("flux", "unavailable")});
        QCOMPARE(codes(warnings), (QStringList{"model_not_running:g"}));
        QCOMPARE(warnings.at(0).severity, Error::Severity::Warning);
    }

    void noProviderAtAllIsACapabilityWarning() {
        const Registry r = registry();
        const Errors warnings = providerWarnings(compiled(r, {}), r, {});
        QCOMPARE(codes(warnings), (QStringList{"capability_unavailable:g"}));
        QCOMPARE(warnings.at(0).severity, Error::Severity::Warning);
    }

    void nodesWithoutCapabilitiesAreNotChecked() {
        Registry r;
        const QJsonObject manifest{{"manifest", 1}, {"id", "fugue/x"}, {"version", "1.0.0"}, {"name", "X"},
                                   {"runtime", QJsonObject{{"kind", "builtin"}}}, {"sink", true},
                                   {"inputs", QJsonArray()}, {"outputs", QJsonArray()}};
        QVERIFY(!hasErrors(r.addManifest(manifest)));
        Document document;
        Node node;
        node.type = "fugue/x";
        node.version = *Version::parse("1.0.0");
        document.graph.nodes["x"] = node;
        const Compilation result = compile(document, r);
        QVERIFY(result.ok());
        QVERIFY(providerWarnings(*result.graph, r, {}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestProviderChecks)
#include "test_provider_checks.moc"
