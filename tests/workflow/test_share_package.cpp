// A workflow packed to share (spec 11.5, #91): one .fuguepack (a zip), a template inside, opened by another
// Fugue as a template opens.
//
// Seams: writeSharePackage / unpackSharePackage (engine/share_package.h) over real files with
// the built-in node types, zips made with libarchive for the ones Fugue must refuse, and
// Workflows::exportShare / openPath for the window.

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <archive.h>
#include <archive_entry.h>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/canvas_package.h"
#include "engine/document.h"
#include "engine/paint_document.h"
#include "engine/paint_file.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "engine/share_package.h"
#include "menu/menu_commands.h"
#include "nodes/builtin.h"
#include "run/run_service.h"
#include "workflow/workflows.h"

using namespace fugue;

namespace {

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QJsonObject readJson(const QString &path) { return QJsonDocument::fromJson(readFile(path)).object(); }

// The smallest PNG (one transparent pixel).
QByteArray png() {
    return QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=");
}

QJsonObject node(const QString &type, const QJsonObject &inputs) {
    return {{"type", type}, {"version", "1.0.0"}, {"inputs", inputs}};
}

// A workflow loading `image`, another Load Image naming a file that is gone, a prompt, and a model.
Document workflowLoading(const QString &image) {
    const QString hash = QStringLiteral("sha256:") + QString(64, u'9');
    const QJsonObject model{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", "m.gguf"}};
    const QJsonObject json{
        {"fugue", 1},
        {"graph", QJsonObject{{"nodes", QJsonObject{
            {"load", node("fugue/load-image", {{"file", image}})},
            {"gone", node("fugue/load-image", {{"file", "/nowhere/lost.png"}})},
            {"prompt", node("fugue/text", {{"text", "a fox in the snow"}})},
            {"models", node("fugue/load-models", {{"diffusion_model", model}})},
            {"empty", node("fugue/preview", {})}}}}}, // a required input with nothing in it: the graph does not compile
        {"requires", QJsonObject{{"models", QJsonArray{QJsonObject{{"resource", "fugue/diffusion-model"},
                                                                   {"hash", hash},
                                                                   {"name", "m.gguf"},
                                                                   {"source", "https://h.example/m.gguf"},
                                                                   {"size", 42}}}}}}};
    return Document::fromJson(json).value();
}

// A zip with one entry of each (name, bytes); a `link` names a symbolic link's target instead.
void makeZip(const QString &path, const QList<std::pair<QByteArray, QByteArray>> &files, const QByteArray &link = {}) {
    archive *out = archive_write_new();
    archive_write_set_format_zip(out);
    QVERIFY(archive_write_open_filename(out, QFile::encodeName(path).constData()) == ARCHIVE_OK);
    for (const auto &[name, bytes] : files) {
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.constData());
        if (!link.isEmpty() && name.endsWith("link")) {
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_symlink(entry, link.constData());
            archive_write_header(out, entry);
        } else {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_entry_set_size(entry, bytes.size());
            archive_write_header(out, entry);
            archive_write_data(out, bytes.constData(), size_t(bytes.size()));
        }
        archive_entry_free(entry);
    }
    archive_write_close(out);
    archive_write_free(out);
}

} // namespace

class TestSharePackage : public QObject {
    Q_OBJECT

    Registry m_registry;

private slots:
    void initTestCase() { nodes::addBuiltinNodeTypes(m_registry); }

    void aPackageIsATemplateWithItsImagesAndNoModel() {
        QTemporaryDir dir;
        writeFile(dir.filePath("pictures/cat.png"), png());
        writeFile(dir.filePath("made.png"), png() + "made");
        ShareRequest request;
        request.title = "Cat edit";
        request.preview = dir.filePath("made.png");
        request.appVersion = "0.1.0";
        const QString zip = dir.filePath("cat.zip");
        const Errors errors = writeSharePackage(workflowLoading(dir.filePath("pictures/cat.png")), m_registry, request, zip);
        QVERIFY(!hasErrors(errors));
        QCOMPARE(errors.size(), 1); // the file that is gone, named
        QCOMPARE(errors.first().node, QString("gone"));
        QVERIFY(isSharePackage(zip));
        QVERIFY(!isSharePackage(dir.filePath("made.png")));

        // Never over a file there.
        QVERIFY(hasErrors(writeSharePackage(workflowLoading({}), m_registry, request, zip)));

        QTemporaryDir out;
        const Result<QString> folder = unpackSharePackage(zip, out.path());
        QVERIFY2(folder, qPrintable(folder ? QString() : folder.error().message));
        QCOMPARE(QFileInfo(folder.value()).fileName(), QString("cat-edit"));
        QCOMPARE(readJson(folder.value() + "/template.json").value("title").toString(), QString("Cat edit"));
        QCOMPARE(readFile(folder.value() + "/inputs/cat.png"), png());
        QCOMPARE(readFile(folder.value() + "/preview.png"), png() + "made");
        const QJsonObject flow = readJson(folder.value() + "/workflow.fugue");
        const QJsonObject nodes = flow.value("graph").toObject().value("nodes").toObject();
        QCOMPARE(nodes.value("load").toObject().value("inputs").toObject().value("file").toString(), QString("inputs/cat.png"));
        QCOMPARE(nodes.value("gone").toObject().value("inputs").toObject().value("file").toString(), QString("lost.png"));
        QCOMPARE(nodes.value("prompt").toObject().value("inputs").toObject().value("text").toString(), QString("a fox in the snow"));
        const QJsonObject model = flow.value("requires").toObject().value("models").toArray().at(0).toObject();
        QCOMPARE(model.value("source").toString(), QString("https://h.example/m.gguf")); // a reference, not a file
        QCOMPARE(model.value("size").toInt(), 42);
        QCOMPARE(QDir(folder.value()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot),
                 (QStringList{"inputs", "preview.png", "template.json", "workflow.fugue"}));

        // Only images Fugue takes in go in; a name every system unpacks.
        writeFile(dir.filePath("secret/id_rsa"), "-----BEGIN OPENSSH PRIVATE KEY-----");
        writeFile(dir.filePath("pictures/12:30 .shot.png"), png());
        Document odd = workflowLoading(dir.filePath("pictures/12:30 .shot.png"));
        odd.graph.nodes["gone"].inputs.insert("file", InputValue::literal(dir.filePath("secret/id_rsa")));
        QStringList included;
        const Errors oddErrors = writeSharePackage(odd, m_registry, request, dir.filePath("odd.zip"), &included);
        QVERIFY(!hasErrors(oddErrors));
        QCOMPARE(included, QStringList{"12:30 .shot.png"});
        QVERIFY(oddErrors.first().message.contains("not an image"));
        QTemporaryDir oddOut;
        const Result<QString> oddFolder = unpackSharePackage(dir.filePath("odd.zip"), oddOut.path());
        QVERIFY2(oddFolder, qPrintable(oddFolder ? QString() : oddFolder.error().message));
        QCOMPARE(QDir(oddFolder.value() + "/inputs").entryList(QDir::Files | QDir::Hidden), QStringList{"12_30_.shot.png"});
        const QJsonObject oddNodes = readJson(oddFolder.value() + "/workflow.fugue").value("graph").toObject().value("nodes").toObject();
        QCOMPARE(oddNodes.value("load").toObject().value("inputs").toObject().value("file").toString(), QString("inputs/12_30_.shot.png"));
        QCOMPARE(oddNodes.value("gone").toObject().value("inputs").toObject().value("file").toString(), QString("id_rsa"));

        // Without the prompts, when asked.
        request.options.withoutPrompts = true;
        QVERIFY(!hasErrors(writeSharePackage(workflowLoading(dir.filePath("pictures/cat.png")), m_registry, request,
                                             dir.filePath("quiet.zip"))));
        QTemporaryDir quiet;
        const QString quietFolder = unpackSharePackage(dir.filePath("quiet.zip"), quiet.path()).value();
        QCOMPARE(readJson(quietFolder + "/workflow.fugue").value("graph").toObject().value("nodes").toObject()
                     .value("prompt").toObject().value("inputs").toObject().value("text").toString(), QString());
    }

    // A painted mask is an input file (#174): the package carries it beside the picture, the workflow in
    // it names both under `inputs/`, and opening the package takes both into the inputs folder.
    void aPaintedMaskTravelsInThePackage() {
        QTemporaryDir dir, state;
        // Two pictures that differ, so the picture and the mask are two files.
        QImage gray(6, 4, QImage::Format_Grayscale8);
        gray.fill(200);
        QByteArray maskBytes;
        {
            QBuffer buffer(&maskBytes);
            buffer.open(QIODevice::WriteOnly);
            gray.save(&buffer, "PNG");
        }
        writeFile(dir.filePath("cat.png"), png());
        writeFile(dir.filePath("cat-mask.png"), maskBytes);
        const QString flowText = QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
            "load": {"type": "fugue/load-image", "version": "1.1.0", "inputs": {"file": "%1", "mask": "%2"}},
            "paint": {"type": "fugue/paint-mask", "version": "1.0.0",
                      "inputs": {"image": {"from": "load.image"}, "mask": "%2"}}}}})")
                                     .arg(dir.filePath("cat.png"), dir.filePath("cat-mask.png"));
        const Document document = Document::load(flowText.toUtf8()).value();

        ShareRequest request;
        request.title = "Painted";
        request.appVersion = "0.1.0";
        QStringList included;
        const QString zip = dir.filePath("painted.zip");
        QVERIFY(!hasErrors(writeSharePackage(document, m_registry, request, zip, &included)));
        QCOMPARE(included.size(), 2);
        QVERIFY(included.contains("cat-mask.png"));
        QTemporaryDir out;
        const QString folder = unpackSharePackage(zip, out.path()).value();
        QCOMPARE(readFile(folder + "/inputs/cat-mask.png"), maskBytes);
        const QJsonObject nodes = readJson(folder + "/workflow.fugue").value("graph").toObject().value("nodes").toObject();
        QCOMPARE(nodes.value("load").toObject().value("inputs").toObject().value("mask").toString(), QString("inputs/cat-mask.png"));
        QCOMPARE(nodes.value("paint").toObject().value("inputs").toObject().value("mask").toString(), QString("inputs/cat-mask.png"));

        // Opened: the mask is in the inputs folder, named by the node, the same bytes.
        CanvasModel canvas(&m_registry);
        CommandRegistry commands;
        registerMenuCommands(commands);
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {};
        Workflows workflows(&canvas, &m_registry, &commands, locations);
        registerFileCommands(commands, workflows);
        QVERIFY2(workflows.openPath(zip), qPrintable(workflows.notice()));
        const QString mask = canvas.document().graph.nodes.value("load").inputs.value("mask").literal().toString();
        QVERIFY2(mask.startsWith(inputsDir() + "/"), qPrintable(mask));
        QCOMPARE(readFile(mask), maskBytes);
        QCOMPARE(canvas.document().graph.nodes.value("paint").inputs.value("mask").literal().toString(), mask);
    }

    // A paint canvas is an input file too (#181): the package carries its .ora, the workflow names it
    // under `inputs/`, and opening the package takes it into the inputs folder, the same bytes.
    void aPaintFileTravelsInThePackage() {
        QTemporaryDir dir, state;
        mask::PaintDocument canvasDocument(10, 6);
        canvasDocument.addLayer(mask::LayerKind::Picture, "Picture");
        canvasDocument.addLayer(mask::LayerKind::Paint, "Paint");
        canvasDocument.brush.color = mask::Rgba{255, 0, 0, 255};
        canvasDocument.fillRect(0, 0, 5, 3);
        const QByteArray ora = mask::paintToOra(canvasDocument).value();
        writeFile(dir.filePath("sketch.ora"), ora);
        const QString flowText = QStringLiteral(R"({"fugue": 1, "graph": {"nodes": {
            "p": {"type": "fugue/paint", "version": "1.0.0", "inputs": {"width": 10, "height": 6, "paint": "%1"}}}}})")
                                     .arg(dir.filePath("sketch.ora"));
        const Document document = Document::load(flowText.toUtf8()).value();

        ShareRequest request;
        request.title = "Sketched";
        request.appVersion = "0.1.0";
        QStringList included;
        const QString zip = dir.filePath("sketched.zip");
        const Errors written = writeSharePackage(document, m_registry, request, zip, &included);
        QVERIFY2(!hasErrors(written) && written.isEmpty(), qPrintable(written.isEmpty() ? QString() : written.first().message));
        QCOMPARE(included, QStringList{"sketch.ora"});
        QTemporaryDir out;
        const QString folder = unpackSharePackage(zip, out.path()).value();
        QCOMPARE(readFile(folder + "/inputs/sketch.ora"), ora);
        const QJsonObject nodes = readJson(folder + "/workflow.fugue").value("graph").toObject().value("nodes").toObject();
        QCOMPARE(nodes.value("p").toObject().value("inputs").toObject().value("paint").toString(), QString("inputs/sketch.ora"));

        CanvasModel canvas(&m_registry);
        CommandRegistry commands;
        registerMenuCommands(commands);
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {};
        Workflows workflows(&canvas, &m_registry, &commands, locations);
        registerFileCommands(commands, workflows);
        QVERIFY2(workflows.openPath(zip), qPrintable(workflows.notice()));
        const QString paint = canvas.document().graph.nodes.value("p").inputs.value("paint").literal().toString();
        QVERIFY2(paint.startsWith(inputsDir() + "/") && paint.endsWith(".ora"), qPrintable(paint));
        QCOMPARE(readFile(paint), ora);
    }

    void unpackingRefusesWhatWouldLeaveItsFolder() {
        QTemporaryDir dir;
        const QByteArray flow = R"({"fugue": 1, "graph": {"nodes": {}}})";
        for (const auto &[name, entries, link] :
             {std::tuple{"dots", QList<std::pair<QByteArray, QByteArray>>{{"x/workflow.fugue", flow}, {"x/../../evil", "!"}}, QByteArray()},
              std::tuple{"absolute", QList<std::pair<QByteArray, QByteArray>>{{"x/workflow.fugue", flow}, {"/tmp/evil", "!"}}, QByteArray()},
              std::tuple{"link", QList<std::pair<QByteArray, QByteArray>>{{"x/workflow.fugue", flow}, {"x/link", {}}}, QByteArray("/etc/passwd")},
              std::tuple{"empty", QList<std::pair<QByteArray, QByteArray>>{{"x/readme.txt", "hi"}}, QByteArray()},
              std::tuple{"backslash", QList<std::pair<QByteArray, QByteArray>>{{"x/workflow.fugue", flow}, {"x\\..\\evil", "!"}}, QByteArray()},
              std::tuple{"drive", QList<std::pair<QByteArray, QByteArray>>{{"x/workflow.fugue", flow}, {"C:/evil", "!"}}, QByteArray()},
              std::tuple{"twice", QList<std::pair<QByteArray, QByteArray>>{{"x/workflow.fugue", flow}, {"x/workflow.fugue", "{}"}}, QByteArray()}}) {
            const QString zip = dir.filePath(QString::fromLatin1(name) + ".zip");
            makeZip(zip, entries, link);
            QTemporaryDir out;
            const Result<QString> folder = unpackSharePackage(zip, out.path());
            QVERIFY2(!folder, name);
            QCOMPARE(folder.error().code, QString("invalid_document"));
            QVERIFY(!QFileInfo::exists(dir.filePath("evil")));
        }

        // A device is not a file. libarchive writes none into a zip, so this one is made by hand:
        // one empty stored entry whose Unix mode, in the central directory, is a character device.
        {
            const QString zip = dir.filePath("device.zip");
            QFile out(zip);
            QVERIFY(out.open(QIODevice::WriteOnly));
            const QByteArray name = "x/tty";
            const auto le = [](quint32 value, int bytes) {
                QByteArray b;
                for (int i = 0; i < bytes; ++i)
                    b.append(char((value >> (8 * i)) & 0xff));
                return b;
            };
            QByteArray local = le(0x04034b50, 4) + le(20, 2) + le(0, 2) + le(0, 2) + le(0, 4) + le(0, 4) + le(0, 4)
                               + le(0, 4) + le(quint32(name.size()), 2) + le(0, 2) + name;
            QByteArray central = le(0x02014b50, 4) + le((3 << 8) | 20, 2) + le(20, 2) + le(0, 2) + le(0, 2) + le(0, 4)
                                 + le(0, 4) + le(0, 4) + le(0, 4) + le(quint32(name.size()), 2) + le(0, 2) + le(0, 2)
                                 + le(0, 2) + le(0, 2) + le(quint32(020644) << 16, 4) + le(0, 4) + name;
            QByteArray end = le(0x06054b50, 4) + le(0, 2) + le(0, 2) + le(1, 2) + le(1, 2)
                             + le(quint32(central.size()), 4) + le(quint32(local.size()), 4) + le(0, 2);
            out.write(local + central + end);
            out.close();
            QTemporaryDir unpacked;
            const Result<QString> refused = unpackSharePackage(zip, unpacked.path());
            QVERIFY(!refused);
            QVERIFY2(refused.error().message.contains("not a file"), qPrintable(refused.error().message));
        }

        // A zip named like a folder it holds: its files go in a new folder, not that one.
        const QString clash = dir.filePath("inputs.zip");
        makeZip(clash, {{"workflow.fugue", flow}, {"inputs/a.png", png()}});
        QTemporaryDir clashOut;
        const Result<QString> clashFolder = unpackSharePackage(clash, clashOut.path());
        QVERIFY2(clashFolder, qPrintable(clashFolder ? QString() : clashFolder.error().message));
        QCOMPARE(QFileInfo(clashFolder.value()).fileName(), QString("inputs-2"));
        QVERIFY(QFileInfo::exists(clashFolder.value() + "/workflow.fugue"));
        QVERIFY(QFileInfo::exists(clashFolder.value() + "/inputs/a.png"));

        // Files at the zip's top (someone zipped the folder's contents), and macOS's extras.
        const QString loose = dir.filePath("My Flow.zip");
        makeZip(loose, {{"workflow.fugue", flow}, {"template.json", R"({"title": "Loose"})"}, {"__MACOSX/._x", "junk"},
                        {".DS_Store", "junk"}});
        QTemporaryDir out;
        const Result<QString> folder = unpackSharePackage(loose, out.path());
        QVERIFY2(folder, qPrintable(folder ? QString() : folder.error().message));
        QCOMPARE(QFileInfo(folder.value()).fileName(), QString("my-flow"));
        QVERIFY(QFileInfo::exists(folder.value() + "/workflow.fugue"));
        QVERIFY(!QFileInfo::exists(out.filePath("__MACOSX")));
    }

    // A saved workflow names the packs it uses as they are installed, with where to get them.
    void aSaveNamesTheInstalledPacksAndTheirSources() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        const Errors added = registry.addManifest(QJsonObject{
            {"manifest", 1}, {"id", "io.github.alice.tools/sharpen"}, {"version", "1.2.0"}, {"name", "Sharpen"},
            {"runtime", QJsonObject{{"kind", "builtin"}}}, {"inputs", QJsonArray()},
            {"outputs", QJsonArray{QJsonObject{{"name", "text"}, {"type", "text"}}}}});
        QVERIFY2(added.isEmpty(), qPrintable(added.isEmpty() ? QString() : added.first().message));
        registry.setPackVersion("io.github.alice.tools", "1.2.0");
        registry.setPackSource("io.github.alice.tools", "1.2.0", "https://github.com/alice/fugue-tools", "0123abcd");
        Document document = Document::fromJson(QJsonObject{
            {"fugue", 1},
            {"graph", QJsonObject{{"nodes", QJsonObject{{"s", QJsonObject{{"type", "io.github.alice.tools/sharpen"},
                                                                           {"version", "1.2.0"}}}}}}}}).value();
        document.refreshRequirements("0.1.0", &registry);
        QCOMPARE(document.requirements.value("packs").toObject(),
                 (QJsonObject{{"io.github.alice.tools", QJsonObject{{"version", "1.2.0"},
                                                                    {"source", "https://github.com/alice/fugue-tools"},
                                                                    {"ref", "0123abcd"}}}}));
    }

    // File > Export for sharing writes the shown workflow; opening the .fuguepack (File > Open, a drop)
    // opens it as a new workflow with its images.
    // File > Export compiled writes the graph a run of the same document compiles to: the providers
    // of the setup and its trust apply to both, and a pin to an instance the setup has is kept.
    void exportCompiledIsWhatARunCompiles() {
        QTemporaryDir dir, state;
        const auto studio = [](Registry &registry) {
            ProviderEntry entry;
            entry.instance.name = "Studio";
            entry.instance.type = "fake";
            return registry.setProviders({entry});
        };
        CanvasModel canvas(&m_registry);
        CommandRegistry commands;
        registerMenuCommands(commands);
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {};
        locations.registerProviders = studio;
        Workflows workflows(&canvas, &m_registry, &commands, locations);
        const QByteArray text = R"({"fugue": 1, "graph": {"nodes": {"t": {"type": "fugue/text", "version": "1.0.0",
            "provider": "Studio", "inputs": {"text": "hi"}}, "p": {"type": "fugue/preview", "version": "1.0.0",
            "inputs": {"value": {"from": "t.text"}}}}}})";
        QVERIFY(workflows.openText(QString::fromUtf8(text)));
        QVERIFY2(workflows.exportCompiled(dir.filePath("graph")), qPrintable(workflows.notice()));
        const QJsonObject exported = readJson(dir.filePath("graph.json"));
        QCOMPARE(exported.value("nodes").toObject().value("t").toObject().value("provider").toString(), QString("Studio"));

        RunSetup setup;
        setup.addNodeTypes = [](Registry &registry) { nodes::addBuiltinNodeTypes(registry); };
        setup.addKernels = nodes::addBuiltinKernels;
        setup.registerProviders = studio;
        setup.runsRoot = dir.filePath("runs");
        setup.queueDir = dir.filePath("queue");
        setup.lockPath = dir.filePath("engine.lock");
        setup.cacheRoot = dir.filePath("results");
        setup.hashesPath = dir.filePath("hashes.json");
        setup.thumbnailDir = dir.filePath("thumbnails");
        setup.trustPath = state.filePath("trust.json");
        setup.collectGarbage = false;
        RunService service(setup);
        QString run, status;
        connect(&service, &RunService::event, this, [&](const QString &id, const QJsonObject &event) {
            if (id == run && event.value("event").toString() == "run.done")
                status = event.value("status").toString();
        });
        RunRequest request;
        request.document = Document::load(text).value();
        run = service.start(request);
        QTRY_VERIFY_WITH_TIMEOUT(!status.isEmpty(), 20000);
        QCOMPARE(status, QString("ok"));
        QCOMPARE(readJson(dir.filePath("runs/" + run + "/compiled.json")), exported);

        // Without the instance, neither compiles the pin.
        locations.registerProviders = [](Registry &registry) { return registry.setProviders({}); };
        Workflows bare(&canvas, &m_registry, &commands, locations);
        QVERIFY(bare.openText(QString::fromUtf8(text)));
        QVERIFY(!bare.exportCompiled(dir.filePath("bare")));
        QVERIFY(bare.notice().contains("unknown_provider") || bare.notice().contains("Studio"));
        QVERIFY(!QFileInfo::exists(dir.filePath("bare.json")));
    }

    // #249: a package that carries a canvas is imported as a canvas of the project, never opened as a workflow and never
    // over a canvas there: its id is taken here, so the canvas gets a new one and the banner says so.
    void aCanvasPackageImportsAsANewCanvas() {
        QTemporaryDir dir, state;
        canvas::CanvasStore store = canvas::CanvasStore::forSession();
        mask::PaintDocument doc(64, 64);
        doc.addLayer(mask::LayerKind::Paint, "Base");
        doc.brush.color = mask::Rgba{20, 80, 200, 255};
        doc.fillRect(0, 0, 64, 64);
        const QString id = store.create("Shared sketch", doc, "new")->id;
        const size_t canvasesBefore = store.list().size(); // the session's store may hold others
        const QString zip = dir.filePath("canvas.fuguepack");
        QVERIFY(!hasErrors(canvas::writeCanvasPackage(store, id, canvas::SharedPart::History, zip)));

        CanvasModel canvas(&m_registry);
        CommandRegistry commands;
        registerMenuCommands(commands);
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {};
        Workflows workflows(&canvas, &m_registry, &commands, locations);
        const int before = workflows.count();
        QVERIFY2(workflows.openPath(zip), qPrintable(workflows.notice()));
        QCOMPARE(workflows.count(), before); // no workflow tab
        QVERIFY(workflows.notice().contains("Shared sketch"));
        QVERIFY(workflows.notice().contains("history"));
        QVERIFY(workflows.notice().contains("yours is untouched"));
        QCOMPARE(store.list().size(), canvasesBefore + 1);
        QCOMPARE(store.info(id)->name, QString("Shared sketch"));
        // A package that is not what it says is refused with its reason and adds nothing.
        QFile bad(dir.filePath("bad.fuguepack"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("PK\x03\x04 not really a zip");
        bad.close();
        QVERIFY(!workflows.openPath(dir.filePath("bad.fuguepack")));
        QCOMPARE(store.list().size(), canvasesBefore + 1);
    }

    void theWindowExportsAndOpensAPackage() {
        QTemporaryDir dir, state;
        writeFile(dir.filePath("cat.png"), png());
        CanvasModel canvas(&m_registry);
        CommandRegistry commands;
        registerMenuCommands(commands);
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {};
        Workflows workflows(&canvas, &m_registry, &commands, locations);
        registerFileCommands(commands, workflows);
        QVERIFY(workflows.openText(QString::fromUtf8(QJsonDocument(workflowLoading(dir.filePath("cat.png")).toJson()).toJson())));

        QSignalSpy asked(&workflows, &Workflows::exportShareRequested);
        QVERIFY(commands.run("file.export-share")); // the menu's item does something now
        QCOMPARE(asked.size(), 1);

        // The compiled graph, as `fugue compile` prints it (its other menu item did nothing too).
        QSignalSpy compiledAsked(&workflows, &Workflows::exportCompiledRequested);
        QVERIFY(commands.run("file.export-compiled"));
        QCOMPARE(compiledAsked.size(), 1);
        QVERIFY(!workflows.exportCompiled(dir.filePath("broken"))); // its Preview has nothing to show
        QVERIFY(workflows.notice().contains("does not compile"));
        QVERIFY(!QFileInfo::exists(dir.filePath("broken.json")));
        const int shared = workflows.current();
        QVERIFY(workflows.openText(R"({"fugue": 1, "graph": {"nodes": {"t": {"type": "fugue/text", "version": "1.0.0",
            "inputs": {"text": "hi"}}, "p": {"type": "fugue/preview", "version": "1.0.0",
            "inputs": {"value": {"from": "t.text"}}}}}})"));
        QVERIFY2(workflows.exportCompiled(dir.filePath("graph")), qPrintable(workflows.notice()));
        QVERIFY(readJson(dir.filePath("graph.json")).contains("nodes"));
        QVERIFY(!workflows.exportCompiled(dir.filePath("graph.json"))); // never over a file
        workflows.setCurrent(shared);

        QVERIFY(workflows.exportShare(QUrl::fromLocalFile(dir.filePath("shared")).toString(), {}));
        QVERIFY(QFileInfo::exists(dir.filePath("shared.fuguepack"))); // its extension added
        QVERIFY(workflows.notice().contains(dir.filePath("shared.fuguepack")));
        QVERIFY(workflows.notice().contains("lost.png")); // what it left out

        const int before = workflows.count();
        QVERIFY(workflows.openPath(dir.filePath("shared.fuguepack")));
        QCOMPARE(workflows.count(), before + 1);
        QVERIFY(workflows.pathOf(workflows.current()).isEmpty()); // a new workflow, saved where asked
        const QString file = canvas.document().graph.nodes.value("load").inputs.value("file").literal().toString();
        QVERIFY2(file.startsWith(inputsDir() + "/"), qPrintable(file));
        QCOMPARE(readFile(file), png());
        QVERIFY(workflows.notice().contains("m.gguf")); // its model is missing here: the banner says so
        // The file the sender lacked names no file in the unpacked folder, which is gone.
        QCOMPARE(canvas.document().graph.nodes.value("gone").inputs.value("file").literal().toString(), QString("lost.png"));

        // A pack it uses that is not here: the banner offers it, pinned to what the sender had.
        const QJsonObject withPack{
            {"fugue", 1},
            {"graph", QJsonObject{{"nodes", QJsonObject{{"s", QJsonObject{{"type", "io.github.alice.tools/sharpen"},
                                                                           {"version", "1.2.0"}}}}}}},
            {"requires", QJsonObject{{"packs", QJsonObject{{"io.github.alice.tools",
                QJsonObject{{"version", "1.2.0"}, {"source", "https://github.com/alice/fugue-tools"}, {"ref", "0123abcd"}}}}}}}};
        QVERIFY(workflows.openText(QString::fromUtf8(QJsonDocument(withPack).toJson())));
        // Its node's image comes with a package too, though the pack is not here to say it is a file.
        {
            const QString zip = dir.filePath("with-pack.zip");
            QJsonObject flow = withPack;
            QJsonObject nodes = flow.value("graph").toObject().value("nodes").toObject();
            QJsonObject sharpen = nodes.value("s").toObject();
            sharpen.insert("inputs", QJsonObject{{"picture", "inputs/cat.png"}});
            nodes.insert("s", sharpen);
            flow.insert("graph", QJsonObject{{"nodes", nodes}});
            makeZip(zip, {{"sharp/workflow.fugue", QJsonDocument(flow).toJson()},
                          {"sharp/template.json", R"({"title": "Sharp"})"}, {"sharp/inputs/cat.png", png()}});
            QVERIFY(workflows.openPath(zip));
            const QString picture = canvas.document().graph.nodes.value("s").inputs.value("picture").literal().toString();
            QVERIFY2(picture.startsWith(inputsDir() + "/"), qPrintable(picture));
            QCOMPARE(readFile(picture), png());
        }
        QCOMPARE(workflows.packOffer().value("id").toString(), QString("io.github.alice.tools"));
        QSignalSpy install(&workflows, &Workflows::packInstallRequested);
        workflows.installPack();
        QCOMPARE(install.size(), 1);
        QCOMPARE(install.first().at(0).toString(), QString("https://github.com/alice/fugue-tools"));
        QCOMPARE(install.first().at(1).toString(), QString("0123abcd"));
        workflows.dismissNotice();
        QVERIFY(workflows.packOffer().isEmpty()); // goes with the line

        // Not a package: says why.
        writeFile(dir.filePath("other.zip"), "PK\x03\x04 not really");
        QVERIFY(!workflows.openPath(dir.filePath("other.zip")));
        QVERIFY(workflows.notice().contains("other.zip"));
    }
};

QTEST_MAIN(TestSharePackage)
#include "test_share_package.moc"
