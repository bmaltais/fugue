// An app shared as a `.fugueapp` bundle (#394, epic #397, spec 11.5): the package of #91 that also carries the packs the app uses and
// every sample file an app input names, keeps the models as references, and is refused (naming each item) when whoever opens it
// could not get something. Opening one is a template's opening: the one Get ready list (#379) then the App view.
//
// Seams: writeSharePackage / checkAppShare / unpackSharePackage over real folders, then the readiness plan of #379 run once with a
// loopback HTTP server for the model (no network); Workflows::openPath for the window. The launcher's "Keep this app" is
// test_app_own_window's.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "../engine/fake_http.h"
#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/app_binding.h"
#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/pack_loader.h"
#include "engine/paths.h"
#include "engine/readiness.h"
#include "engine/registry.h"
#include "engine/share_package.h"
#include "engine/templates.h"
#include "engine/trust_store.h"
#include "menu/menu_commands.h"
#include "nodes/builtin.h"
#include "workflow/image_inputs.h"
#include "workflow/workflows.h"

using namespace fugue;
using fugue::test::FakeHttpServer;
using fugue::test::HttpResponse;

namespace {

void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(path));
    file.write(bytes);
}

QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void writeJson(const QString &path, const QJsonObject &object) { write(path, QJsonDocument(object).toJson()); }

QByteArray png() {
    return QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=");
}

// An installed pack `root/<id>` with one command node, a setup, and what an install leaves around (an environment, git's folder).
QString makePack(const QString &root, const QString &id) {
    const QString dir = root + "/" + id;
    writeJson(dir + "/pack.json", QJsonObject{{"manifest", 1}, {"id", id}, {"version", "1.0.0"}, {"name", "Segment"}, {"nodes", QJsonArray{"tool"}},
                                              {"setup", QJsonObject{{"command", QJsonArray{"sh", "-c", "touch environment.ok"}}, {"description", "Makes the environment"}}}});
    writeJson(dir + "/tool/node.json",
              QJsonObject{{"manifest", 1}, {"id", id + "/tool"}, {"version", "1.0.0"}, {"name", "Tool"},
                          {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                          {"inputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}},
                          {"outputs", QJsonArray{QJsonObject{{"name", "image"}, {"type", "image"}}}}});
    write(dir + "/tool/run.sh", "#!/bin/sh\n");
    write(dir + "/.venv/bin/python", "an environment: never carried");
    write(dir + "/venv/lib/x.py", "another one");
    write(dir + "/.fugue-source.json", R"({"kind": "git", "location": "https://example.com/segment.git", "ref": "v1", "commit": "abc123"})");
    return dir;
}

QStringList filesUnder(const QString &folder) {
    QStringList names;
    QDirIterator it(folder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        names << QDir(folder).relativeFilePath(it.next());
    names.sort();
    return names;
}

} // namespace

class TestAppBundle : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    Registry m_sender; // the sender's: the built-ins and the Segment pack
    std::unique_ptr<FakeHttpServer> m_server;
    QByteArray m_weights = QByteArray(4000, 'w');

    QString path(const QString &name) const { return m_dir.filePath(name); }

    // The app: a picture in, another file in (any type), the Segment node and a model; one sink.
    Document app(const QString &sendersFolder, const QString &modelSource) const {
        const QJsonObject model{{"resource", "fugue/diffusion-model"}, {"hash", sha256(m_weights)}, {"name", "m.safetensors"}};
        QJsonObject requires_;
        if (!modelSource.isEmpty())
            requires_["models"] = QJsonArray{QJsonObject{{"resource", "fugue/diffusion-model"}, {"hash", sha256(m_weights)}, {"name", "m.safetensors"},
                                                         {"source", modelSource}, {"size", m_weights.size()}}};
        const auto node = [](const QString &type, const QString &version, const QJsonObject &inputs) {
            return QJsonObject{{"type", type}, {"version", version}, {"inputs", inputs}};
        };
        const QJsonObject json{
            {"fugue", 1},
            {"requires", requires_},
            {"app", QJsonObject{{"format", 1}, {"title", "Detailer"},
                                {"inputs", QJsonArray{QJsonObject{{"role", "image"}, {"targets", QJsonArray{"load.file"}}},
                                                      QJsonObject{{"name", "clip"}, {"targets", QJsonArray{"clip.file"}}}}},
                                {"outputs", QJsonArray{QJsonObject{{"node", "save"}}}}}},
            {"graph", QJsonObject{{"nodes", QJsonObject{
                {"load", node("fugue/load-image", "1.1.0", {{"file", sendersFolder + "/pic.png"}})},
                {"clip", node("fugue/load-image", "1.1.0", {{"file", sendersFolder + "/sample.clip"}})},
                {"tool", node("io.test.segment/tool", "1.0.0", {{"image", QJsonObject{{"from", "load.image"}}}})},
                {"models", node("fugue/load-models", "1.0.0", {{"diffusion_model", model}})},
                {"save", node("fugue/save-image", "1.0.0", {{"image", QJsonObject{{"from", "tool.image"}}}})}}}}}};
        return Document::fromJson(json).value();
    }

    ShareRequest request(const QStringList &packPaths) const {
        ShareRequest r;
        r.title = "Detailer";
        r.app = true;
        r.packPaths = packPaths;
        r.appVersion = "1.0.0";
        return r;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        qputenv("FUGUE_HOME", path("receiver-home").toUtf8());
        nodes::addBuiltinNodeTypes(m_sender);
        makePack(path("sender/packs"), "io.test.segment");
        QVERIFY(loadPack(m_sender, path("sender/packs/io.test.segment")).enabled);
        write(path("sender/pic.png"), png());
        write(path("sender/sample.clip"), "not an image: a clip's bytes");
        m_server = std::make_unique<FakeHttpServer>();
        QVERIFY(m_server->listen());
        m_server->on("GET", "/m.safetensors", [this](const auto &) { return HttpResponse{200, m_weights, "application/octet-stream"}; });
    }

    // The epic's test (#394): made on one home, opened on a fresh home with no packs and no models, ready after exactly one approval.
    void aBundleMadeHereIsRunnableOnAFreshHomeAfterOneApproval() {
        const QString zip = path("detailer.fugueapp");
        const Document sender = app(path("sender"), QString::fromLatin1(m_server->origin()) + "/m.safetensors");
        QStringList included, packs;
        const Errors written = writeSharePackage(sender, m_sender, request({path("sender/packs")}), zip, &included, &packs);
        QVERIFY2(!hasErrors(written), written.isEmpty() ? "" : qPrintable(written.first().message));
        QCOMPARE(packs, QStringList{"io.test.segment"});
        QVERIFY(included.contains("pic.png") && included.contains("sample.clip")); // a picture, and a file that is no image

        // What is inside: the pack as installed less its environment, the samples; no model.
        const QString unpacked = path("unpacked");
        QDir().mkpath(unpacked);
        const Result<QString> folder = unpackSharePackage(zip, unpacked);
        QVERIFY(folder);
        const QStringList files = filesUnder(folder.value());
        QCOMPARE(files, (QStringList{"inputs/pic.png", "inputs/sample.clip", "packs/io.test.segment/pack.json", "packs/io.test.segment/tool/node.json",
                                     "packs/io.test.segment/tool/run.sh", "template.json", "workflow.fugue"}));
        const QJsonObject recorded = QJsonDocument::fromJson(read(folder.value() + "/workflow.fugue")).object().value("requires").toObject();
        QCOMPARE(recorded.value("packs").toObject().value("io.test.segment").toObject().value("commit").toString(), QString("abc123"));
        QCOMPARE(recorded.value("models").toArray().at(0).toObject().value("source").toString(), QString::fromLatin1(m_server->origin()) + "/m.safetensors");

        // The receiver: a fresh registry (only the built-ins), no pack, no model.
        Registry receiver;
        nodes::addBuiltinNodeTypes(receiver);
        const TemplateLibrary library({{QFileInfo(folder.value()).absolutePath(), false}}, &receiver);
        const std::optional<Template> opened = library.find(QFileInfo(folder.value()).fileName());
        QVERIFY(opened);
        QVERIFY(opened->app); // an app: its card and its window
        QCOMPARE(opened->carriedPacks.size(), 1);
        Result<Document> document = Document::loadFile(opened->workflow);
        QVERIFY(document);
        makeInputsAbsolute(document.value(), receiver, opened->workflow); // as the window opens a template
        QVERIFY(takeInputsFrom(document.value(), receiver, opened->folder, true).isEmpty());
        for (const QString &node : {QStringLiteral("load"), QStringLiteral("clip")}) { // both samples are in the receiver's inputs folder, byte for byte
            const QString file = document->graph.nodes.value(node).inputs.value("file").literal().toString();
            QVERIFY2(file.startsWith(inputsDir() + "/"), qPrintable(file));
            QCOMPARE(read(file), node == "load" ? png() : QByteArray("not an image: a clip's bytes"));
        }

        FileHashes hashes(path("receiver/hashes.json"));
        TrustStore trust(path("receiver/trust.json"));
        const QString packsDir = path("receiver/packs");
        ReadinessEnv env{ModelLibrary({path("receiver/models")}, &receiver)};
        env.registry = &receiver;
        env.hashes = &hashes;
        env.trust = &trust;
        env.packsDir = packsDir;
        env.freeSpace = [](const QString &) { return qint64(4) << 30; };
        const ReadinessPlan plan = buildReadinessPlan(requestFor(*document, receiver, opened->carriedPacks, opened->folder), env);
        QVERIFY(plan.problems.isEmpty());
        QCOMPARE(plan.packs.size(), 1); // the carried pack: installed and trusted, its environment made
        QVERIFY(plan.packs.at(0).installs() && plan.packs.at(0).hasEnvironment());
        QCOMPARE(plan.models.size(), 1); // the model: a download, to a folder the sheet names
        QVERIFY(!plan.destination.isEmpty());

        // The one approval: one run of the list, no question on the way, nothing fetched but the model.
        int questions = 0;
        ReadinessRun run{packsDir, &trust, &hashes};
        run.allowMore = [&questions](const PackSummary &, const QStringList &) { ++questions; return true; };
        run.fetch = [&questions](const PackSource &, const QString &) { ++questions; return Result<QString>(QString()); };
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        QVERIFY2(outcome.ok, outcome.errors.isEmpty() ? "" : qPrintable(outcome.errors.first().message));
        QCOMPARE(questions, 0);
        QVERIFY(outcome.packsChanged);
        QVERIFY(QFileInfo(packsDir + "/io.test.segment/environment.ok").isFile());
        QVERIFY(!QFileInfo::exists(packsDir + "/io.test.segment/.venv")); // the sender's environment never travelled
        QVERIFY(trust.isTrusted("io.test.segment", "1.0.0"));
        QDirIterator weights(path("receiver/models"), {"m.safetensors"}, QDir::Files, QDirIterator::Subdirectories);
        QVERIFY(weights.hasNext());
        QCOMPARE(QFileInfo(weights.next()).size(), qint64(m_weights.size()));
        QCOMPARE(m_server->requests().size(), 1);

        // Ready to run: the node types are there, the app block fits the graph, and nothing is left to get.
        QVERIFY(loadPack(receiver, packsDir + "/io.test.segment").enabled);
        const Result<std::optional<app::App>> block = app::appOf(*document);
        QVERIFY(block && *block);
        QVERIFY(!hasErrors(app::checkApp(**block, *document, receiver)));
        const ReadinessPlan again = buildReadinessPlan(requestFor(*document, receiver, opened->carriedPacks, opened->folder), env);
        QVERIFY(again.models.isEmpty() && again.problems.isEmpty());
        QCOMPARE(again.installCount(), 0);
    }

    // The export checks first and names every item the receiver could not get (#394).
    void anExportRefusesWhatTheReceiverCouldNotGetAndNamesEachItem() {
        const QString empty = path("no-packs");
        QDir().mkpath(empty);
        const QString zip = path("refused.fugueapp");

        // The pack's folder is not found here and nothing says where to get it, and the model has no source: two items, two errors.
        const Document stuck = app(path("sender"), QString());
        const Errors errors = writeSharePackage(stuck, m_sender, request({empty}), zip);
        QVERIFY(hasErrors(errors));
        QCOMPARE(errors.size(), 2);
        QVERIFY(errors.at(0).message.contains("Segment 1.0.0"));
        QVERIFY(errors.at(1).message.contains("m.safetensors"));
        for (const Error &error : errors)
            QCOMPARE(error.code, QString("app_not_shareable"));
        QVERIFY(!QFileInfo::exists(zip)); // nothing was written
        QCOMPARE(checkAppShare(stuck, m_sender, request({empty})).size(), 2);

        // A pack that is fetched, not carried, is a warning: the bundle is written and the person is told. The sample files that are
        // gone are warnings too (the input stays empty).
        Document sourced = app(path("gone"), QString::fromLatin1(m_server->origin()) + "/m.safetensors");
        QJsonObject requires_ = sourced.requirements;
        requires_["packs"] = QJsonObject{{"io.test.segment", QJsonObject{{"version", "1.0.0"}, {"source", "https://example.com/segment.git"}, {"ref", "v1"}}}};
        sourced.requirements = requires_;
        const Errors warnings = writeSharePackage(sourced, m_sender, request({empty}), zip);
        QVERIFY(!hasErrors(warnings));
        QVERIFY(QFileInfo::exists(zip));
        QStringList texts;
        for (const Error &warning : warnings)
            texts << warning.message;
        QVERIFY2(texts.join('\n').contains("https://example.com/segment.git"), qPrintable(texts.join('\n')));
        QVERIFY2(texts.join('\n').contains("pic.png is not there"), qPrintable(texts.join('\n')));
    }

    // The reader goes by content: the bundle under any name opens, with its pack offered; an old package carries none (#394).
    void aRenamedBundleOpensAndAPlainPackageStillOpensAsAWorkflow() {
        const Document sender = app(path("sender"), QString::fromLatin1(m_server->origin()) + "/m.safetensors");
        const QString bundle = path("renamed.fuguepack"); // an app bundle under the package's name
        QVERIFY(!hasErrors(writeSharePackage(sender, m_sender, request({path("sender/packs")}), bundle)));
        ShareRequest plainRequest = request({});
        plainRequest.app = false; // the package of before: no pack, only images
        const QString plain = path("plain.fuguepack");
        QVERIFY(!hasErrors(writeSharePackage(sender, m_sender, plainRequest, plain)));
        QDir().mkpath(path("plain-unpacked"));
        const Result<QString> plainFolder = unpackSharePackage(plain, path("plain-unpacked"));
        QVERIFY(plainFolder);
        QVERIFY(!QFileInfo::exists(plainFolder.value() + "/packs")); // a package of before carries no pack

        QTemporaryDir state;
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        CanvasModel canvas(&registry);
        CommandRegistry commands;
        registerMenuCommands(commands);
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {};
        Workflows workflows(&canvas, &registry, &commands, locations);
        registerFileCommands(commands, workflows);

        QVERIFY2(workflows.openPath(bundle), qPrintable(workflows.notice()));
        QCOMPARE(workflows.tabs().last().toMap().value("package").toString(), bundle);
        QVERIFY(workflows.readiness()->isOpen());                 // the one Get ready sheet, by itself
        QCOMPARE(workflows.readiness()->view().value("packs").toList().size(), 1); // with the carried pack in it
        QCOMPARE(workflows.packOffer().value("id").toString(), QString("io.test.segment"));
        QVERIFY(QFileInfo(workflows.packOffer().value("source").toString() + "/pack.json").isFile()); // from the carried folder, which lives with the tab
        workflows.readiness()->cancel();

        const int before = workflows.count();
        QVERIFY2(workflows.openPath(plain), qPrintable(workflows.notice()));
        QCOMPARE(workflows.count(), before + 1);
        QVERIFY(!workflows.packOffer().value("installable").toBool()); // an old package carries no pack to install
        QVERIFY(canvas.document().graph.nodes.contains("load"));
        workflows.readiness()->cancel();

        // The suffix File > Export for sharing writes: .fugueapp for an app (the tab shown is one), .fuguepack for the rest.
        QCOMPARE(workflows.shareSuffix(), QString("fugueapp"));
        QVERIFY(workflows.openText(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QCOMPARE(workflows.shareSuffix(), QString("fuguepack"));
        QVERIFY(workflows.exportShare(path("exported"), {}));
        QVERIFY(QFileInfo::exists(path("exported.fuguepack")));
    }
};

QTEST_MAIN(TestAppBundle)
#include "test_app_bundle.moc"
