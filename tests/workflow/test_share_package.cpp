// A workflow packed to share (spec 11.5, #91): one .fuguepack (a zip), a template inside, opened by another
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
        registry.setPackSource("io.github.alice.tools", "https://github.com/alice/fugue-tools", "0123abcd");
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
