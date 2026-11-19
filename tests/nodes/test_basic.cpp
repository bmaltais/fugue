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
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtEndian>
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

// The text of the PNG's iTXt chunk keyed `fugue`, read by walking the chunks here; null when
// there is none. A tEXt chunk does not count.
QByteArray pngWorkflowText(const QString &file) {
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = in.readAll();
    qsizetype at = 8;
    while (at + 12 <= bytes.size()) {
        const quint32 length = qFromBigEndian<quint32>(bytes.constData() + at);
        if (bytes.mid(at + 4, 4) == "iTXt" && bytes.mid(at + 8, 6) == QByteArray("fugue\0", 6))
            return bytes.mid(at + 8 + 10, length - 10);
        at += 12 + length;
    }
    return {};
}

// The XMP packet's fugue:workflow text of a WebP file, unescaped; null when there is none.
QByteArray webpWorkflowText(const QString &file) {
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = in.readAll();
    qsizetype at = 12;
    while (at + 8 <= bytes.size()) {
        const quint32 length = qFromLittleEndian<quint32>(bytes.constData() + at + 4);
        if (bytes.mid(at, 4) == "XMP ") {
            const QRegularExpression tag("<fugue:workflow>(.*)</fugue:workflow>", QRegularExpression::DotMatchesEverythingOption);
            QString text = tag.match(QString::fromUtf8(bytes.mid(at + 8, length))).captured(1);
            text.replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&");
            return text.toUtf8();
        }
        at += 8 + length + (length & 1);
    }
    return {};
}

QJsonObject parsed(const QByteArray &text) {
    return QJsonDocument::fromJson(text).object();
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

    // Runs a document given as JSON text; a node without a version gets 1.0.0. Without
    // `fromDocument` the run gets only the compiled graph, as `fugue run compiled.json` does.
    Ran run(const QByteArray &nodes, const QJsonObject &layout = {}, bool fromDocument = true) {
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
        if (fromDocument)
            options.document = document->toJson();
        ran.outcome = runGraph(*compiled, *made, environment, options);
        ran.events = made->events();
        ran.state = made->state();
        for (const Error &error : std::as_const(ran.outcome.errors))
            if (error.severity == Error::Severity::Error) {
                ran.error = error.code + ": " + error.message;
                break;
            }
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

    // --- Save Image ---

    // load -> save, with the given inputs of the save node (JSON object text without braces).
    QByteArray saveFlow(const QString &source, const QString &saveInputs) {
        return QByteArray("{'l': {'type': 'fugue/load-image', 'inputs': {'file': '") + source.toUtf8() + "'}},"
               " 't': {'type': 'fugue/text', 'inputs': {'text': 'a secret prompt'}},"
               " 'p': {'type': 'fugue/preview', 'inputs': {'value': {'from': 't.text'}}},"
               " 's': {'type': 'fugue/save-image', 'inputs': {'image': {'from': 'l.image'}, " + saveInputs.toUtf8() + "}}}";
    }

    QString outDir() { return path("out"); }

    void saveImageWritesAPngWithTheWorkflowEmbedded() {
        const QString source = makePng("src-full", 8, 6);
        const QJsonObject layout = json("{'nodes': {'l': {'pos': [10, 20]}}}");
        const Ran ran = run(saveFlow(source, "'name': 'fox', 'folder': '" + outDir() + "'"), layout);
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        const QString file = outDir() + "/fox.png";
        QVERIFY(QFileInfo::exists(file));
        // The output is the written file, with its size.
        const QJsonObject written = ran.outcome.outputs.value("s").value("file").media().toJson();
        QCOMPARE(written.value("path").toString(), file);
        QCOMPARE(written.value("width").toInt(), 8);

        const QByteArray text = pngWorkflowText(file);
        QVERIFY2(!text.isEmpty(), "no iTXt chunk keyed fugue");
        const QJsonObject workflow = parsed(text);
        QCOMPARE(workflow.value("fugue").toInt(), 1);
        // The graph, as it was run, and the layout.
        const QJsonObject nodes = workflow.value("graph").toObject().value("nodes").toObject();
        QCOMPARE(nodes.value("t").toObject().value("inputs").toObject().value("text").toString(), QString("a secret prompt"));
        QCOMPARE(workflow.value("layout").toObject().value("nodes").toObject().value("l").toObject().value("pos").toArray().at(0).toInt(), 10);
        // Absolute local paths are gone: the loaded file is a bare name, and the folder too.
        QCOMPARE(nodes.value("l").toObject().value("inputs").toObject().value("file").toString(), QString("src-full.png"));
        QCOMPARE(nodes.value("s").toObject().value("inputs").toObject().value("folder").toString(), QString("out"));
        QVERIFY2(!text.contains(m_dir.path().toUtf8()), "a local path was embedded");
        // The run section exists (it holds what was used; nothing here was randomized).
        QVERIFY(workflow.contains("run"));
    }

    void embedNoPromptsBlanksTheTextButKeepsTheGraph() {
        const QString source = makePng("src-np", 4, 4);
        const Ran ran = run(saveFlow(source, "'name': 'np', 'embed': 'no-prompts', 'folder': '" + outDir() + "'"));
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        const QByteArray text = pngWorkflowText(outDir() + "/np.png");
        QVERIFY(!text.isEmpty());
        QVERIFY(!text.contains("a secret prompt"));
        const QJsonObject nodes = parsed(text).value("graph").toObject().value("nodes").toObject();
        QVERIFY(nodes.contains("t"));
        QCOMPARE(nodes.value("t").toObject().value("inputs").toObject().value("text").toString(), QString());
    }

    void embedNoneWritesAPlainImage() {
        const QString source = makePng("src-none", 4, 4);
        const Ran ran = run(saveFlow(source, "'name': 'plain', 'embed': 'none', 'folder': '" + outDir() + "'"));
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        QVERIFY(QFileInfo::exists(outDir() + "/plain.png"));
        QVERIFY(pngWorkflowText(outDir() + "/plain.png").isEmpty());
    }

    // A run from a compiled graph has no document to embed (11.7): the image is saved and the
    // run says why it holds no workflow, on the node, as a warning.
    void aRunFromACompiledGraphWarnsThatNothingWasEmbedded() {
        const QString source = makePng("src-compiled", 4, 4);
        const Ran ran = run(saveFlow(source, "'name': 'compiled', 'folder': '" + outDir() + "'"), {}, false);
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        QVERIFY(QFileInfo::exists(outDir() + "/compiled.png"));
        QVERIFY(pngWorkflowText(outDir() + "/compiled.png").isEmpty());
        QCOMPARE(ran.outcome.status, RunStatus::Ok);
        QCOMPARE(ran.outcome.errors.size(), 1);
        const Error warning = ran.outcome.errors.first();
        QCOMPARE(warning.code, QString("workflow_not_embedded"));
        QVERIFY(warning.severity == Error::Severity::Warning);
        QCOMPARE(warning.node, QString("s"));
        QVERIFY2(warning.message.contains("compiled graph"), qPrintable(warning.message));
        // run.done carries it, as it does every warning of the run.
        const QJsonArray done = ran.events.last().value("errors").toArray();
        QCOMPARE(done.size(), 1);
        QCOMPARE(done.at(0).toObject().value("code").toString(), QString("workflow_not_embedded"));
        QCOMPARE(done.at(0).toObject().value("severity").toString(), QString("warning"));
    }

    // Embedding none is the node's own setting: nothing to warn about.
    void embedNoneFromACompiledGraphDoesNotWarn() {
        const QString source = makePng("src-compiled-none", 4, 4);
        const Ran ran = run(saveFlow(source, "'name': 'compiled-none', 'embed': 'none', 'folder': '" + outDir() + "'"), {},
                            false);
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        QVERIFY(ran.outcome.errors.isEmpty());
    }

    void saveImageWritesAWebpWithTheWorkflowInXmp() {
        const QString source = makePng("src-webp", 9, 7);
        const Ran ran = run(saveFlow(source, "'name': 'wp', 'format': 'webp', 'folder': '" + outDir() + "'"));
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        const QString file = outDir() + "/wp.webp";
        QVERIFY(QFileInfo::exists(file));
        const vips::VImage decoded = vips::VImage::new_from_file(file.toUtf8().constData());
        QCOMPARE(decoded.width(), 9);
        QCOMPARE(decoded.height(), 7);
        const QJsonObject workflow = parsed(webpWorkflowText(file));
        QVERIFY(workflow.value("graph").toObject().value("nodes").toObject().contains("s"));
        QCOMPARE(ran.outcome.outputs.value("s").value("file").media().toJson().value("format").toString(), QString("webp"));
    }

    void saveImageFromAFileThatAlreadyHasAWorkflowKeepsOne() {
        const QString source = makePng("src-twice", 4, 4);
        run(saveFlow(source, "'name': 'again', 'folder': '" + outDir() + "'"));
        // Load the saved image and save it again: one chunk, from the second workflow.
        const Ran ran = run(saveFlow(outDir() + "/again.png", "'name': 'again2', 'folder': '" + outDir() + "'"));
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        QFile in(outDir() + "/again2.png");
        QVERIFY(in.open(QIODevice::ReadOnly));
        QCOMPARE(in.readAll().count("iTXt"), 1);
    }

    void aSecondSaveWithNewContentGetsANewNameAndNeverOverwrites() {
        const QString a = makePng("src-n1", 4, 4);
        const QString b = makePng("src-n2", 5, 5);
        run(saveFlow(a, "'name': 'shot', 'folder': '" + outDir() + "'"));
        const Ran second = run(saveFlow(b, "'name': 'shot', 'folder': '" + outDir() + "'"));
        QVERIFY2(second.error.isEmpty(), qPrintable(second.error));
        QVERIFY(QFileInfo::exists(outDir() + "/shot.png"));
        QVERIFY(QFileInfo::exists(outDir() + "/shot-2.png"));
        QCOMPARE(second.outcome.outputs.value("s").value("file").media().path(), outDir() + "/shot-2.png");
        QCOMPARE(vips::VImage::new_from_file((outDir() + "/shot.png").toUtf8().constData()).width(), 4);
    }

    void anUnchangedSaveDoesNotWriteADuplicateButARemovedFileIsWrittenAgain() {
        const QString source = makePng("src-dup", 4, 4);
        const QByteArray flow = saveFlow(source, "'name': 'once', 'folder': '" + outDir() + "'");
        QVERIFY(run(flow).outcome.ran.contains("s"));
        const Ran again = run(flow);
        QVERIFY(!again.outcome.ran.contains("s"));
        QVERIFY(!QFileInfo::exists(outDir() + "/once-2.png"));
        QCOMPARE(again.outcome.outputs.value("s").value("file").media().path(), outDir() + "/once.png");
        // The file is gone: the cached result is not valid any more, so the node writes it.
        QVERIFY(QFile::remove(outDir() + "/once.png"));
        QVERIFY(run(flow).outcome.ran.contains("s"));
        QVERIFY(QFileInfo::exists(outDir() + "/once.png"));
    }

    // Save nodes run in parallel: several with one name and folder each get a name of their
    // own, and none overwrites another's file.
    void parallelSavesWithOneNameNeverOverwriteEachOther_data() {
        QTest::addColumn<QString>("format");
        QTest::addRow("png") << "png";
        QTest::addRow("webp") << "webp";
    }
    void parallelSavesWithOneNameNeverOverwriteEachOther() {
        QFETCH(QString, format);
        const QString folder = path("race-" + format);
        QByteArray nodes = "{";
        const int saves = 8;
        for (int i = 0; i < saves; ++i) {
            const QString source = makePng(QString("src-race-%1-%2").arg(format).arg(i), 10 + i, 3);
            nodes += QString("'l%1': {'type': 'fugue/load-image', 'inputs': {'file': '%2'}},"
                             " 's%1': {'type': 'fugue/save-image', 'inputs': {'image': {'from': 'l%1.image'},"
                             " 'name': 'race', 'format': '%3', 'embed': 'none', 'folder': '%4'}}%5")
                         .arg(i)
                         .arg(source, format, folder, i + 1 < saves ? "," : "}")
                         .toUtf8();
        }
        const Ran ran = run(nodes);
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        QSet<QString> files;
        QSet<int> widths;
        for (int i = 0; i < saves; ++i) {
            const QString file = ran.outcome.outputs.value(QString("s%1").arg(i)).value("file").media().path();
            files.insert(file);
            widths.insert(vips::VImage::new_from_file(file.toUtf8().constData()).width());
        }
        QCOMPARE(files.size(), saves);
        QCOMPARE(widths.size(), saves); // every source's pixels are in a file of their own
        QCOMPARE(QDir(folder).entryList(QDir::Files | QDir::Hidden).size(), saves); // no partial file left
    }

    void saveImageRefusesANameThatIsAPath() {
        const QString source = makePng("src-bad", 4, 4);
        const Ran ran = run(saveFlow(source, "'name': '../escape', 'folder': '" + outDir() + "'"));
        QVERIFY(ran.error.startsWith("node_failed"));
        QCOMPARE(ran.outcome.errors.value(0).input, QString("name"));
        QVERIFY(!QFileInfo::exists(path("escape.png")));
    }

    void aRelativeFolderIsUnderTheOutputFolder() {
        const QString source = makePng("src-rel", 4, 4);
        qputenv("XDG_PICTURES_DIR", path("pictures").toUtf8());
        const Ran ran = run(saveFlow(source, "'name': 'rel', 'folder': 'sub'"));
        QVERIFY2(ran.error.isEmpty(), qPrintable(ran.error));
        qunsetenv("XDG_PICTURES_DIR");
        QCOMPARE(ran.outcome.outputs.value("s").value("file").media().path(), path("pictures/Fugue/sub/rel.png"));
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
