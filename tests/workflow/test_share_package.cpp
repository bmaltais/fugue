// A workflow packed to share (spec 11.5, #91): one .zip, a template inside, opened by another
// Fugue as a template opens.
//
// Seams: writeSharePackage / unpackSharePackage (engine/share_package.h) over real files with
// the built-in node types, zips made with libarchive for the ones Fugue must refuse, and
// Workflows::exportShare / openPath for the window.

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
#include "engine/document.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "engine/share_package.h"
#include "menu/menu_commands.h"
#include "nodes/builtin.h"
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
            {"models", node("fugue/load-models", {{"diffusion_model", model}})}}}}},
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

        // Without the prompts, when asked.
        request.options.withoutPrompts = true;
        QVERIFY(!hasErrors(writeSharePackage(workflowLoading(dir.filePath("pictures/cat.png")), m_registry, request,
                                             dir.filePath("quiet.zip"))));
        QTemporaryDir quiet;
        const QString quietFolder = unpackSharePackage(dir.filePath("quiet.zip"), quiet.path()).value();
        QCOMPARE(readJson(quietFolder + "/workflow.fugue").value("graph").toObject().value("nodes").toObject()
                     .value("prompt").toObject().value("inputs").toObject().value("text").toString(), QString());
    }

    void unpackingRefusesWhatWouldLeaveItsFolder() {
        QTemporaryDir dir;
        const QByteArray flow = R"({"fugue": 1, "graph": {"nodes": {}}})";
        for (const auto &[name, entries, link] :
             {std::tuple{"dots", QList<std::pair<QByteArray, QByteArray>>{{"x/workflow.fugue", flow}, {"x/../../evil", "!"}}, QByteArray()},
              std::tuple{"absolute", QList<std::pair<QByteArray, QByteArray>>{{"x/workflow.fugue", flow}, {"/tmp/evil", "!"}}, QByteArray()},
              std::tuple{"link", QList<std::pair<QByteArray, QByteArray>>{{"x/workflow.fugue", flow}, {"x/link", {}}}, QByteArray("/etc/passwd")},
              std::tuple{"empty", QList<std::pair<QByteArray, QByteArray>>{{"x/readme.txt", "hi"}}, QByteArray()}}) {
            const QString zip = dir.filePath(QString::fromLatin1(name) + ".zip");
            makeZip(zip, entries, link);
            QTemporaryDir out;
            const Result<QString> folder = unpackSharePackage(zip, out.path());
            QVERIFY2(!folder, name);
            QCOMPARE(folder.error().code, QString("invalid_document"));
            QVERIFY(!QFileInfo::exists(dir.filePath("evil")));
        }

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
        registry.setPackSource("io.github.alice.tools", "https://github.com/alice/fugue-tools");
        Document document = Document::fromJson(QJsonObject{
            {"fugue", 1},
            {"graph", QJsonObject{{"nodes", QJsonObject{{"s", QJsonObject{{"type", "io.github.alice.tools/sharpen"},
                                                                           {"version", "1.2.0"}}}}}}}}).value();
        document.refreshRequirements("0.1.0", &registry);
        QCOMPARE(document.requirements.value("packs").toObject(),
                 (QJsonObject{{"io.github.alice.tools", QJsonObject{{"version", "1.2.0"},
                                                                    {"source", "https://github.com/alice/fugue-tools"}}}}));
    }

    // File > Export for sharing writes the shown workflow; opening the .zip (File > Open, a drop)
    // opens it as a new workflow with its images.
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
        QVERIFY(!workflows.exportCompiled(dir.filePath("broken"))); // its Load Models lacks two files
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
        QVERIFY(QFileInfo::exists(dir.filePath("shared.zip"))); // ".zip" added
        QVERIFY(workflows.notice().contains(dir.filePath("shared.zip")));
        QVERIFY(workflows.notice().contains("lost.png")); // what it left out

        const int before = workflows.count();
        QVERIFY(workflows.openPath(dir.filePath("shared.zip")));
        QCOMPARE(workflows.count(), before + 1);
        QVERIFY(workflows.pathOf(workflows.current()).isEmpty()); // a new workflow, saved where asked
        const QString file = canvas.document().graph.nodes.value("load").inputs.value("file").literal().toString();
        QVERIFY2(file.startsWith(inputsDir() + "/"), qPrintable(file));
        QCOMPARE(readFile(file), png());
        QVERIFY(workflows.notice().contains("m.gguf")); // its model is missing here: the banner says so

        // Not a package: says why.
        writeFile(dir.filePath("other.zip"), "PK\x03\x04 not really");
        QVERIFY(!workflows.openPath(dir.filePath("other.zip")));
        QVERIFY(workflows.notice().contains("other.zip"));
    }
};

QTEST_MAIN(TestSharePackage)
#include "test_share_package.moc"
