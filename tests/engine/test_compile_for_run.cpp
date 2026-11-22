// Compiling as a run does (engine/compile_for_run.h): the providers a setup registers, the trust
// it asks, the group embedding and the order of the problems, observed through compileForRun()
// with a fake setup and CompiledGraph::toJson().

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "engine/compile_for_run.h"
#include "engine/document.h"
#include "engine/registry.h"

using namespace fugue;

namespace {

QJsonObject json(const char *text) {
    QByteArray bytes(text);
    bytes.replace('\'', '"');
    return QJsonDocument::fromJson(bytes).object();
}

Registry makeRegistry() {
    Registry r;
    const char *manifests[] = {
        "{'manifest': 1, 'id': 'fugue/note', 'version': '1.0.0', 'name': 'Note', 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'text', 'type': 'text', 'default': 'x'}],"
        " 'outputs': [{'name': 'text', 'type': 'text'}]}",
        "{'manifest': 1, 'id': 'io.example.tools/shout', 'version': '2.0.0', 'name': 'Shout', 'sink': true, 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'text', 'type': 'text', 'default': 'x'}],"
        " 'outputs': [{'name': 'text', 'type': 'text'}]}"};
    for (const char *manifest : manifests) {
        const Errors added = r.addManifest(json(manifest));
        if (hasErrors(added))
            qFatal("bad fixture manifest");
    }
    r.setPackVersion("io.example.tools", "2.0.0");
    return r;
}

Document document(const char *nodes) {
    QByteArray text = QByteArray("{'fugue': 1, 'graph': {'nodes': {") + nodes + "}}}";
    text.replace('\'', '"');
    return Document::load(text).value();
}

// A setup whose providers are the named instances, made by nobody: a fake for what providers.json is.
RunCompileSetup fakeSetup(const QStringList &names, Errors warnings = {}, bool trusted = true) {
    RunCompileSetup setup;
    setup.registerProviders = [names, warnings](Registry &registry) {
        QList<ProviderEntry> entries;
        for (const QString &name : names) {
            ProviderEntry entry;
            entry.instance.name = name;
            entry.instance.type = "fake";
            entries << entry;
        }
        const Errors refused = registry.setProviders(entries);
        return hasErrors(refused) ? refused : warnings;
    };
    setup.trusted = [trusted](const QString &, const QString &) { return trusted; };
    return setup;
}

// A node pinned to the instance Studio, and a node from a pack.
const char *pinned = "'a': {'type': 'fugue/note', 'version': '1.0.0', 'provider': 'Studio'},"
                     "'b': {'type': 'io.example.tools/shout', 'version': '2.0.0', 'inputs': {'text': {'from': 'a.text'}}}";

QString codes(const Errors &errors) {
    QStringList out;
    for (const Error &error : errors)
        out << error.code;
    return out.join(',');
}

} // namespace

class TestCompileForRun : public QObject {
    Q_OBJECT

private slots:
    // The same document compiles to the same graph whoever asks (the CLI, a run, the export).
    void aNodeNeedingAProviderCompilesTheSameForEveryCaller() {
        const Document doc = document(pinned);
        Registry cli = makeRegistry(), exported = makeRegistry();
        const Compilation first = compileForRun(doc, cli, {}, fakeSetup({"Studio"}));
        const Compilation second = compileForRun(doc, exported, {}, fakeSetup({"Studio"}));
        QVERIFY2(first.ok(), qPrintable(codes(first.problems())));
        QCOMPARE(first->toJson(), second->toJson());
        QCOMPARE(first->nodes.value("a").provider, QString("Studio"));
        // Without the instance the pin is unknown, as in a run.
        Registry bare = makeRegistry();
        const Compilation missing = compileForRun(doc, bare, {}, fakeSetup({"Laptop"}));
        QVERIFY(!missing.ok());
        QCOMPARE(missing.problems().first().code, QString("unknown_provider"));
    }

    void anUntrustedPacksNodeIsRefused() {
        const Document doc = document(pinned);
        Registry r = makeRegistry();
        const Compilation refused = compileForRun(doc, r, {}, fakeSetup({"Studio"}, {}, false));
        QVERIFY(!refused.ok());
        QCOMPARE(codes(refused.problems()), QString("pack_untrusted"));
        QCOMPARE(refused.problems().first().node, QString("b"));
        QVERIFY(compileForRun(doc, r, {}, fakeSetup({"Studio"})).ok());
    }

    void theProviderProblemsComeBeforeTheCompilersAndAnErrorStopsEarly() {
        // `a` is pinned to an instance that is not there, which the compiler says after the setup's warning.
        const Document doc = document(pinned);
        Error cloud = makeError("unknown_provider", "Cloud could not be made.");
        cloud.severity = Error::Severity::Warning;
        const Errors warning{cloud};
        Registry r = makeRegistry();
        const Compilation result = compileForRun(doc, r, {}, fakeSetup({"Laptop"}, warning));
        QVERIFY(!result.ok());
        QCOMPARE(result.problems().size(), 2);
        QCOMPARE(result.problems().at(0).message, QString("Cloud could not be made."));
        QCOMPARE(result.problems().at(1).severity, Error::Severity::Error);

        RunCompileSetup broken;
        broken.registerProviders = [](Registry &) { return Errors{makeError("invalid_document", "providers.json is broken.")}; };
        const Compilation stopped = compileForRun(doc, r, {}, broken);
        QVERIFY(!stopped.ok());
        QCOMPARE(codes(stopped.problems()), QString("invalid_document"));
    }

    void theRequestsTargetAndProviderReachTheCompiler() {
        const Document doc = document(pinned);
        Registry r = makeRegistry();
        RunCompileRequest request;
        request.target = "a";
        const Compilation toHere = compileForRun(doc, r, request, fakeSetup({"Studio"}));
        QVERIFY(toHere.ok());
        QCOMPARE(toHere->nodes.keys(), QStringList{"a"});
        request = {};
        request.provider = "Laptop";
        const Compilation chosen = compileForRun(document("'a': {'type': 'fugue/note', 'version': '1.0.0'},"
                                                          "'b': {'type': 'io.example.tools/shout', 'version': '2.0.0',"
                                                          " 'inputs': {'text': {'from': 'a.text'}}}"), r, request,
                                                 fakeSetup({"Studio", "Laptop"}));
        QVERIFY2(chosen.ok(), qPrintable(codes(chosen.problems())));
        QCOMPARE(chosen->nodes.value("a").provider, QString("Laptop"));
    }
};

QTEST_GUILESS_MAIN(TestCompileForRun)
#include "test_compile_for_run.moc"
