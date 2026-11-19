// The basic nodes (spec 14.1): Text, Number, Load Image, Save Image, Preview.
//
// Seam: a workflow document run through the real scheduler (runGraph) with the
// built-in node types and kernels, as the CLI worker does; media files are read
// back from disk, and the embedded workflow is read out of the written file by a
// chunk parser of this test's own. The real `fugue` binary is driven in
// test_basic_cli.cpp.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <vips/vips8>

#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/media.h"
#include "engine/registry.h"
#include "engine/result_cache.h"
#include "engine/run_folder.h"
#include "engine/scheduler.h"
#include "nodes/builtin.h"

using namespace fugue;

namespace {

QJsonObject json(const QByteArray &text) {
    QByteArray bytes(text);
    bytes.replace('\'', '"');
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (!document.isObject())
        qFatal("bad test JSON: %s in %s", qPrintable(error.errorString()), text.constData());
    return document.object();
}

struct Ran {
    RunOutcome outcome;
    QList<QJsonObject> events;
    QJsonObject state;
    QString error; // the first error, for a failure message
};

} // namespace

class TestBasic : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    Registry m_registry;
    Kernels m_kernels;
    std::unique_ptr<FileHashes> m_hashes;
    std::unique_ptr<ResultCache> m_cache;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    // Runs a document given as JSON text; a node without a version gets 1.0.0.
    Ran run(const QByteArray &nodes, const QJsonObject &layout = {}) {
        QJsonObject nodeObject = json(nodes);
        for (auto it = nodeObject.begin(); it != nodeObject.end(); ++it) {
            QJsonObject node = it.value().toObject();
            node.insert("version", "1.0.0");
            it.value() = node;
        }
        QJsonObject documentJson{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodeObject}}}};
        if (!layout.isEmpty())
            documentJson.insert("layout", layout);
        const Result<Document> document = Document::fromJson(documentJson);
        if (!document)
            qFatal("bad test document");
        const Compilation compiled = compile(*document, m_registry);
        Ran ran;
        if (!compiled.ok()) {
            ran.error = compiled.errors().value(0).message;
            return ran;
        }
        const auto made = RunFolder::create(path("runs"));
        if (!made)
            qFatal("no run folder");
        RunEnvironment environment;
        environment.registry = &m_registry;
        environment.kernels = &m_kernels;
        environment.cache = m_cache.get();
        environment.hashes = m_hashes.get();
        RunOptions options;
        options.document = document->toJson();
        ran.outcome = runGraph(*compiled, *made, environment, options);
        ran.events = made->events();
        ran.state = made->state();
        if (!ran.outcome.errors.isEmpty())
            ran.error = ran.outcome.errors.value(0).code + ": " + ran.outcome.errors.value(0).message;
        return ran;
    }

    // A solid-colour PNG fixture, made by libvips; returns its path.
    QString makePng(const QString &name, int width, int height) {
        const vips::VImage image = vips::VImage::black(width, height, vips::VImage::option()->set("bands", 3)) + 100;
        const Result<MediaHandle> handle = MediaHandle::fromImage(image.cast(VIPS_FORMAT_UCHAR));
        if (!handle)
            qFatal("fixture image");
        const Result<MediaHandle> saved = handle->saved(path("fixtures"), name, *m_hashes);
        if (!saved)
            qFatal("fixture save");
        return saved->path();
    }

    static QList<QJsonObject> eventsOf(const Ran &ran, const QString &kind, const QString &node) {
        QList<QJsonObject> found;
        for (const QJsonObject &event : ran.events)
            if (event.value("event").toString() == kind && event.value("node").toString() == node)
                found.append(event);
        return found;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        nodes::addBuiltinNodeTypes(m_registry);
        nodes::addBuiltinKernels(m_kernels);
        m_hashes = std::make_unique<FileHashes>(path("hashes.json"));
        m_cache = std::make_unique<ResultCache>(path("results"));
    }

    void textGivesItsText() {
        const Ran ran = run("{'t': {'type': 'fugue/text', 'inputs': {'text': 'a red fox'}},"
                            " 'p': {'type': 'fugue/preview', 'inputs': {'value': {'from': 't.text'}}}}");
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        QCOMPARE(ran.outcome.outputs.value("t").value("text").text(), QString("a red fox"));
    }

    void numberGivesItsValue() {
        const Ran ran = run("{'n': {'type': 'fugue/number', 'inputs': {'value': 4.5}},"
                            " 'p': {'type': 'fugue/preview', 'inputs': {'value': {'from': 'n.value'}}}}");
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        QCOMPARE(ran.outcome.outputs.value("n").value("value").number(), 4.5);
    }

    void previewReportsItsValueInTheRunEvents() {
        const Ran ran = run("{'t': {'type': 'fugue/text', 'inputs': {'text': 'shown text'}},"
                            " 'p': {'type': 'fugue/preview', 'inputs': {'value': {'from': 't.text'}}}}");
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        const QList<QJsonObject> done = eventsOf(ran, "node.done", "p");
        QCOMPARE(done.size(), 1);
        QCOMPARE(done.first().value("shown").toObject().value("value").toString(), QString("shown text"));
        // run.json carries it too, so `runs status` needs no replay.
        QCOMPARE(ran.state.value("nodes").toObject().value("p").toObject().value("shown").toObject().value("value").toString(),
                 QString("shown text"));
    }

    void aCachedPreviewStillShowsItsValue() {
        const QByteArray nodes = "{'t': {'type': 'fugue/text', 'inputs': {'text': 'cached shown'}},"
                                 " 'p': {'type': 'fugue/preview', 'inputs': {'value': {'from': 't.text'}}}}";
        run(nodes);
        const Ran again = run(nodes);
        const QList<QJsonObject> cached = eventsOf(again, "node.cached", "p");
        QCOMPARE(cached.size(), 1);
        QCOMPARE(cached.first().value("shown").toObject().value("value").toString(), QString("cached shown"));
    }

    void previewShowsANumberAndAnImage() {
        const QString file = makePng("pv", 6, 4);
        const Ran ran = run(QByteArray("{'l': {'type': 'fugue/load-image', 'inputs': {'file': '") + file.toUtf8() + "'}},"
                            " 'w': {'type': 'fugue/preview', 'inputs': {'value': {'from': 'l.width'}}},"
                            " 'i': {'type': 'fugue/preview', 'inputs': {'value': {'from': 'l.image'}}}}");
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        QCOMPARE(eventsOf(ran, "node.done", "w").first().value("shown").toObject().value("value").toInt(), 6);
        const QJsonObject image = eventsOf(ran, "node.done", "i").first().value("shown").toObject().value("value").toObject();
        QCOMPARE(image.value("type").toString(), QString("image"));
        QCOMPARE(image.value("path").toString(), file);
    }

    void loadImageGivesTheImageAndItsSize() {
        const QString file = makePng("load", 7, 5);
        const Ran ran = run(QByteArray("{'l': {'type': 'fugue/load-image', 'inputs': {'file': '") + file.toUtf8() + "'}},"
                            " 'p': {'type': 'fugue/preview', 'inputs': {'value': {'from': 'l.image'}}}}");
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        const Values out = ran.outcome.outputs.value("l");
        QCOMPARE(out.value("width").number(), 7.0);
        QCOMPARE(out.value("height").number(), 5.0);
        QVERIFY(out.value("image").isMedia());
        QCOMPARE(out.value("image").media().hash(), m_hashes->hash(file));
    }

    void loadImageIsKeyedByContentNotPath() {
        const QString first = makePng("same-a", 3, 3);
        const QString second = path("fixtures/same-b.png");
        QVERIFY(QFile::copy(first, second));
        auto flow = [](const QString &file) {
            return QByteArray("{'l': {'type': 'fugue/load-image', 'inputs': {'file': '") + file.toUtf8() + "'}},"
                   " 'p': {'type': 'fugue/preview', 'inputs': {'value': {'from': 'l.width'}}}}";
        };
        QVERIFY(run(flow(first)).outcome.ran.contains("l"));
        // Another path, same bytes: a hit, so the kernel does not run.
        const Ran again = run(flow(second));
        QVERIFY2(again.error.isEmpty(), qPrintable(again.error));
        QVERIFY(!again.outcome.ran.contains("l"));
        // Same path, other bytes: it runs again.
        QVERIFY(QFile::remove(second));
        const QString changed = makePng("same-b", 4, 4);
        QCOMPARE(changed, second);
        QVERIFY(run(flow(second)).outcome.ran.contains("l"));
    }

    void loadImageNamesAMissingFile() {
        const Ran ran = run("{'l': {'type': 'fugue/load-image', 'inputs': {'file': '/nonexistent/none.png'}},"
                            " 'p': {'type': 'fugue/preview', 'inputs': {'value': {'from': 'l.width'}}}}");
        QVERIFY(ran.error.startsWith("file_not_found"));
        QVERIFY(ran.error.contains("/nonexistent/none.png"));
    }

    void loadImageRejectsAFileThatIsNotAnImage() {
        QFile file(path("notimage.png"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("this is text");
        file.close();
        const Ran ran = run(QByteArray("{'l': {'type': 'fugue/load-image', 'inputs': {'file': '") + path("notimage.png").toUtf8() +
                            "'}}, 'p': {'type': 'fugue/preview', 'inputs': {'value': {'from': 'l.width'}}}}");
        QVERIFY(ran.error.startsWith("media_unreadable"));
    }
};

QTEST_GUILESS_MAIN(TestBasic)
#include "test_basic.moc"
