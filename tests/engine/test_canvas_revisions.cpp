// Pinning a canvas revision at compile time (#209, spec 14.5): `head` becomes the revision's full hash,
// once, in the compiled graph (so in its cache key and in compiled.json), through compileForRun().
// Seam: compileForRun with a fake resolver, and CompiledGraph::toJson().

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "engine/compile_for_run.h"
#include "engine/document.h"
#include "engine/registry.h"

using namespace fugue;

namespace {

const QString hashA = "sha256:" + QString(64, 'a');
const QString hashB = "sha256:" + QString(64, 'b');

QJsonObject json(const char *text) {
    QByteArray bytes(text);
    bytes.replace('\'', '"');
    return QJsonDocument::fromJson(bytes).object();
}

Registry makeRegistry() {
    Registry r;
    const char *manifests[] = {
        "{'manifest': 1, 'id': 'fugue/canvas-region', 'version': '1.0.0', 'name': 'Canvas Region', 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'canvas', 'type': 'text'}, {'name': 'revision', 'type': 'text', 'default': 'head'}],"
        " 'outputs': [{'name': 'width', 'type': 'number'}]}",
        "{'manifest': 1, 'id': 'fugue/text', 'version': '1.0.0', 'name': 'Text', 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'text', 'type': 'text', 'default': 'x'}],"
        " 'outputs': [{'name': 'text', 'type': 'text'}]}",
        "{'manifest': 1, 'id': 'fugue/show', 'version': '1.0.0', 'name': 'Show', 'sink': true, 'runtime': {'kind': 'builtin'},"
        " 'inputs': [{'name': 'value', 'type': 'number'}], 'outputs': []}"};
    for (const char *manifest : manifests)
        if (hasErrors(r.addManifest(json(manifest))))
            qFatal("bad fixture manifest");
    return r;
}

Document document(const char *region) {
    QByteArray text = QByteArray("{'fugue': 1, 'graph': {'nodes': {'t': {'type': 'fugue/text', 'version': '1.0.0'},") +
                      "'r': {'type': 'fugue/canvas-region', 'version': '1.0.0', 'inputs': {" + region + "}},"
                      "'s': {'type': 'fugue/show', 'version': '1.0.0', 'inputs': {'value': {'from': 'r.width'}}}}}}";
    text.replace('\'', '"');
    return Document::load(text).value();
}

// A store of two canvases: canvas-aaaaaaaaaaaa at hashA, canvas-bbbbbbbbbbbb at hashB; the head asked is counted.
struct Fake {
    int asked = 0;
    CanvasRevisionResolver resolver() {
        return [this](const QString &canvas, const QString &ref) -> Result<QString> {
            ++asked;
            if (canvas != "canvas-aaaaaaaaaaaa")
                return makeError(QStringLiteral("canvas_not_found"), QStringLiteral("No canvas %1.").arg(canvas));
            if (ref == "head")
                return hashA;
            if (hashA.startsWith(ref) || ref == hashA.left(15))
                return hashA;
            return makeError(QStringLiteral("canvas_not_found"), QStringLiteral("No revision %1.").arg(ref));
        };
    }
};

QString regionRevision(const Compilation &c) { return c->nodes.value("r").inputs.value("revision").literal().toString(); }

} // namespace

class TestCanvasRevisions : public QObject {
    Q_OBJECT

private slots:
    void headIsNamedInFullOnceAndLandsInCompiledJson() {
        Fake fake;
        RunCompileSetup setup;
        setup.resolveCanvas = fake.resolver();
        Registry registry = makeRegistry();
        const Compilation c = compileForRun(document("'canvas': 'canvas-aaaaaaaaaaaa', 'revision': 'head'"), registry, {}, setup);
        QVERIFY(c.ok());
        QCOMPARE(regionRevision(c), hashA);
        QCOMPARE(fake.asked, 1);
        // compiled.json is CompiledGraph::toJson(): the hash is in it, "head" is not.
        const QByteArray compiled = QJsonDocument(c->toJson()).toJson();
        QVERIFY(compiled.contains(hashA.toUtf8()));
        QVERIFY(!compiled.contains("\"head\""));
    }

    void anInputLeftOutIsHeadAndIsPinnedToo() {
        Fake fake;
        RunCompileSetup setup;
        setup.resolveCanvas = fake.resolver();
        Registry registry = makeRegistry();
        const Compilation c = compileForRun(document("'canvas': 'canvas-aaaaaaaaaaaa'"), registry, {}, setup);
        QVERIFY(c.ok());
        QCOMPARE(regionRevision(c), hashA);
    }

    void aPrefixBecomesTheFullHash() {
        Fake fake;
        RunCompileSetup setup;
        setup.resolveCanvas = fake.resolver();
        Registry registry = makeRegistry();
        const Compilation c = compileForRun(document("'canvas': 'canvas-aaaaaaaaaaaa', 'revision': 'sha256:aaaaaaaa'"), registry, {}, setup);
        QVERIFY(c.ok());
        QCOMPARE(regionRevision(c), hashA);
    }

    void theSameCompileTwiceIsTheSameGraph() {
        Fake fake;
        RunCompileSetup setup;
        setup.resolveCanvas = fake.resolver();
        Registry first = makeRegistry(), second = makeRegistry();
        const Document doc = document("'canvas': 'canvas-aaaaaaaaaaaa'");
        QCOMPARE(compileForRun(doc, first, {}, setup)->toJson(), compileForRun(doc, second, {}, setup)->toJson());
    }

    void aMissingCanvasIsAnErrorOnTheNode() {
        Fake fake;
        RunCompileSetup setup;
        setup.resolveCanvas = fake.resolver();
        Registry registry = makeRegistry();
        const Compilation c = compileForRun(document("'canvas': 'canvas-cccccccccccc'"), registry, {}, setup);
        QVERIFY(!c.ok());
        Error error; // the warnings come with the errors: take the error
        for (const Error &each : c.errors())
            if (each.severity == Error::Severity::Error)
                error = each;
        QCOMPARE(error.code, QString("canvas_not_found"));
        QCOMPARE(error.node, QString("r"));
        QCOMPARE(error.input, QString("revision"));
    }

    void aLinkedCanvasIsLeftForTheKernelToRefuse() {
        Fake fake;
        RunCompileSetup setup;
        setup.resolveCanvas = fake.resolver();
        Registry registry = makeRegistry();
        const Compilation c = compileForRun(document("'canvas': {'from': 't.text'}"), registry, {}, setup);
        QVERIFY(c.ok());
        QCOMPARE(fake.asked, 0);
        QVERIFY(c->nodes.value("r").inputs.value("revision").isEmpty());
    }

    void withoutAResolverNothingChanges() {
        Registry registry = makeRegistry();
        const Compilation c = compileForRun(document("'canvas': 'canvas-aaaaaaaaaaaa', 'revision': 'head'"), registry, {}, RunCompileSetup());
        QVERIFY(c.ok());
        QCOMPARE(regionRevision(c), QString("head"));
    }
};

QTEST_GUILESS_MAIN(TestCanvasRevisions)
#include "test_canvas_revisions.moc"
