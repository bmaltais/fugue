// Save, open, recovery, import, trust and tabs (spec 11, 12.10, 6.7), on the real Workflows
// with a canvas model and the built-in node types, and the window that shows them.

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/stat.h>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/compiler.h"
#include "engine/embed.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "qml_app.h"
#include "theme.h"
#include "translator.h"
#include "workflow/content_hash.h"
#include "workflow/workflow_io.h"
#include "workflow/workflows.h"

using namespace fugue;

namespace {

const char *textNode = R"({"fugue": 1, "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a fox"}}}},
  "layout": {"nodes": {"prompt": {"pos": [10, 20]}}}})";

// An unknown pack's command node with inputs and links, between two built-in nodes.
const char *withMissingNode = R"({"fugue": 1, "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "x"}},
    "odd": {"type": "io.example.tools/sharpen", "version": "2.0.0",
            "inputs": {"image": {"from": "prompt.text"}, "amount": 3}},
    "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "odd.image"}}}}},
  "requires": {"packs": {"io.example.tools": {"version": "2.0.0", "source": "https://example.com/tools"}}}})";

bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QVariantMap nodeRow(CanvasModel &canvas, const QString &id) {
    for (int i = 0; i < canvas.nodes()->rowCount(); ++i) {
        const QModelIndex index = canvas.nodes()->index(i);
        if (canvas.nodes()->data(index, NodeList::NodeId).toString() != id)
            continue;
        QVariantMap map;
        map["missing"] = canvas.nodes()->data(index, NodeList::Missing);
        map["title"] = canvas.nodes()->data(index, NodeList::Title);
        return map;
    }
    return {};
}

QQuickItem *item(QQuickItem *from, const QString &name) {
    for (auto *child : from->childItems()) {
        if (child->objectName() == name)
            return child;
        if (auto *found = item(child, name))
            return found;
    }
    return nullptr;
}

QString themeFolder(const QTemporaryDir &dir) {
    QFile shell(dir.filePath("shell.toml"));
    if (shell.open(QIODevice::WriteOnly))
        shell.write("[font]\nbase-size = 12\n");
    return dir.path();
}

// Everything Workflows needs, with its files in a temporary folder.
struct Fixture {
    QTemporaryDir dir;
    Registry registry;
    CanvasModel canvas;
    CommandRegistry commands;
    WorkflowLocations locations;
    std::unique_ptr<Workflows> workflows;

    Fixture() : canvas((nodes::addBuiltinNodeTypes(registry), &registry)) {
        registerDefaultCommands(commands);
        locations.recoveryDir = dir.filePath("state/recovery");
        locations.trustPath = dir.filePath("state/trust.json");
        locations.tabColorsPath = dir.filePath("state/tab-colors.json");
        locations.recoveryIntervalMs = 3600 * 1000; // the tests call writeRecoveryCopies()
        workflows = std::make_unique<Workflows>(&canvas, &registry, &commands, locations);
    }
    // A second start of the app on the same state folder.
    std::unique_ptr<Workflows> restart(CanvasModel &other) {
        return std::make_unique<Workflows>(&other, &registry, &commands, locations);
    }
    QString file(const QString &name) const { return dir.filePath(name); }
};

} // namespace

class TestWorkflows : public QObject {
    Q_OBJECT

private slots:
    void contentHashIgnoresViewRequirementsAndRunButNotLayout() {
        Document a = Document::load(textNode).value();
        Document b = a;
        b.layout.insert("view", QJsonObject{{"zoom", 2}});
        b.requirements = QJsonObject{{"fugue", "9"}};
        b.run = QJsonObject{{"seed", 1}};
        QCOMPARE(contentHash(a), contentHash(b));
        b.layout.insert("nodes", QJsonObject{{"prompt", QJsonObject{{"pos", QJsonArray{11, 20}}}}});
        QVERIFY(contentHash(a) != contentHash(b));
    }

    void detectsTheKindFromTheContentNotTheName() {
        QCOMPARE(detectKind(QByteArray("\x89PNG\r\n\x1a\n....", 12)), SourceKind::Image);
        QCOMPARE(detectKind(QByteArray("RIFF\0\0\0\0WEBPVP8 ", 16)), SourceKind::Image);
        QCOMPARE(detectKind("{\"fugue\": 1}"), SourceKind::Document);

        Fixture f;
        // A workflow saved with a wrong extension still opens.
        QVERIFY(write(f.file("workflow.txt"), textNode));
        QVERIFY(f.workflows->openPath(f.file("workflow.txt")));
        QCOMPARE(f.canvas.nodes()->rowCount(), 1);
    }

    // --- save ---

    void opensAFreshWorkflowThatIsNotUnsaved() {
        Fixture f;
        QCOMPARE(f.workflows->count(), 1);
        QVERIFY(!f.workflows->unsaved());
        // Ctrl+W on it has nothing to close, so the registry lets it quit.
        QVERIFY(!f.commands.workflowOpen());
    }

    void unsavedIsByContentNotByEdit() {
        Fixture f;
        QVERIFY(write(f.file("a.fugue"), textNode));
        QVERIFY(f.workflows->openPath(f.file("a.fugue")));
        QVERIFY(!f.workflows->unsaved());
        QVERIFY(f.commands.workflowOpen());

        f.canvas.beginMove("prompt");
        f.canvas.moveBy(40, 0);
        f.canvas.endMove();
        QVERIFY(f.workflows->unsaved());
        QVERIFY(f.commands.unsavedWork());
        QVERIFY(f.workflows->windowTitle().startsWith("• a.fugue"));

        // Moving it back leaves the content as it was saved.
        f.canvas.beginMove("prompt");
        f.canvas.moveBy(0, 0);
        f.canvas.endMove();
        f.canvas.beginMove("prompt");
        f.canvas.moveBy(-40, 0);
        f.canvas.endMove();
        QVERIFY(!f.workflows->unsaved());
        QVERIFY(!f.commands.unsavedWork());

        // Panning and zooming are not content.
        f.canvas.setView(500, 500, 2);
        f.canvas.beginMove("prompt");
        f.canvas.endMove();
        QVERIFY(!f.workflows->unsaved());
    }

    void savesAtomicallyToTheTabsFileAndClearsUnsaved() {
        Fixture f;
        QVERIFY(write(f.file("a.fugue"), textNode));
        QVERIFY(f.workflows->openPath(f.file("a.fugue")));
        f.canvas.addNode("fugue/text", 300, 0);
        QVERIFY(f.workflows->unsaved());

        QVERIFY(f.workflows->save());
        QVERIFY(!f.workflows->unsaved());
        const Result<Document> saved = Document::loadFile(f.file("a.fugue"));
        QVERIFY(saved.ok());
        QCOMPARE(saved->graph.nodes.size(), 2);
        // `requires` is written on save, and the hidden partial file is gone.
        QCOMPARE(saved->requirements.value("nodes").toObject().value("fugue/text").toString(), QString("1.0.0"));
        QDir dir(f.dir.path());
        for (const QString &name : dir.entryList(QDir::Files | QDir::Hidden))
            QVERIFY2(!name.endsWith(".part"), qPrintable(name));
    }

    void saveWithoutAFileAsksWhereAndAddsTheExtension() {
        Fixture f;
        f.canvas.addNode("fugue/text", 0, 0);
        QSignalSpy asked(f.workflows.get(), &Workflows::saveAsRequested);
        QVERIFY(!f.workflows->save());
        QCOMPARE(asked.size(), 1);

        QVERIFY(f.workflows->saveAs(QUrl::fromLocalFile(f.file("mine")).toString()));
        QVERIFY(QFileInfo::exists(f.file("mine.fugue")));
        QCOMPARE(f.workflows->tabs().first().toMap().value("title").toString(), QString("mine.fugue"));
        QVERIFY(!f.workflows->unsaved());
    }

    // Undo after a Save As to another folder names the same files as before it: the steps
    // kept from before hold the old folder's relative paths made absolute, so they do not
    // resolve in the new folder to another image of the same name.
    void undoAfterSaveAsElsewhereKeepsNamingTheSameFiles() {
        Fixture f;
        QImage red(4, 4, QImage::Format_RGB32), blue(4, 4, QImage::Format_RGB32);
        red.fill(Qt::red);
        blue.fill(Qt::blue);
        QVERIFY(QDir().mkpath(f.file("a/inputs")) && QDir().mkpath(f.file("b/inputs")));
        QVERIFY(red.save(f.file("a/inputs/cat.png")) && blue.save(f.file("b/inputs/cat.png")));
        QVERIFY(write(f.file("a/w.fugue"), R"({"fugue": 1, "graph": {"nodes": {
            "src": {"type": "fugue/load-image", "version": "1.0.0", "inputs": {"file": "inputs/cat.png"}}}}})"));
        QVERIFY(f.workflows->openPath(f.file("a/w.fugue")));
        const auto file = [&] {
            return f.canvas.document().graph.nodes.value("src").inputs.value("file").literal().toString();
        };
        f.canvas.addNode("fugue/text", 300, 0); // a step from before the save
        QVERIFY(f.workflows->saveAs(f.file("b/w2.fugue")));
        QCOMPARE(file(), QString("inputs/cat-2.png"));
        QCOMPARE(read(f.file("b/inputs/cat-2.png")), read(f.file("a/inputs/cat.png")));
        const QString original = QFileInfo(f.file("a/inputs/cat.png")).canonicalFilePath();
        f.canvas.undo(); // Move images
        QCOMPARE(file(), original);
        f.canvas.undo(); // the added node
        QCOMPARE(file(), original);
        QCOMPARE(f.canvas.document().graph.nodes.size(), 1);
        f.canvas.redo();
        f.canvas.redo();
        QCOMPARE(file(), QString("inputs/cat-2.png"));
    }

    // The dialog asked about "mine", not "mine.fugue": a file already there is never replaced.
    void anAddedExtensionNeverReplacesAnExistingFile() {
        Fixture f;
        QVERIFY(write(f.file("mine.fugue"), textNode));
        f.canvas.addNode("fugue/number", 0, 0);
        QVERIFY(!f.workflows->saveAs(QUrl::fromLocalFile(f.file("mine")).toString()));
        QCOMPARE(read(f.file("mine.fugue")), QByteArray(textNode));
        QVERIFY2(f.workflows->notice().contains("mine.fugue") && f.workflows->notice().contains("exists"),
                 qPrintable(f.workflows->notice()));
        QVERIFY(f.workflows->unsaved());
        QVERIFY(f.workflows->pathOf(0).isEmpty());
        // A name typed in full was asked about by the dialog: it replaces.
        QVERIFY(f.workflows->saveAs(f.file("mine.fugue")));
        QCOMPARE(Document::loadFile(f.file("mine.fugue"))->graph.nodes.size(), 1);
    }

    void aFailedSaveKeepsTheWorkAndSaysWhy() {
        Fixture f;
        f.canvas.addNode("fugue/text", 0, 0);
        QVERIFY(!f.workflows->saveAs(f.file("no-such-folder/x.fugue")));
        QVERIFY(f.workflows->unsaved());
        QVERIFY(f.workflows->notice().contains("Could not save"));
    }

    // A project's documents/ may not exist (made before it was created with the project): saving there
    // works, and the dialog never starts on a missing folder.
    void savingIntoAMissingDocumentsFolderMakesItAndTheDialogStartsOnAFolder() {
        Fixture f;
        const QString root = f.file("proj");
        QVERIFY(QDir().mkpath(root));
        const QByteArray saved = qgetenv("FUGUE_HOME");
        qputenv("FUGUE_HOME", root.toLocal8Bit());
        const auto restore = qScopeGuard([&] {
            if (saved.isNull())
                qunsetenv("FUGUE_HOME");
            else
                qputenv("FUGUE_HOME", saved);
        });
        QVERIFY(!QFileInfo(root + "/documents").exists());
        QCOMPARE(f.workflows->suggestedFolder().toLocalFile(), root);
        f.canvas.addNode("fugue/text", 0, 0);
        QVERIFY(f.workflows->saveAs(root + "/documents/w.fugue"));
        QVERIFY(QFileInfo(root + "/documents/w.fugue").isFile());
        QCOMPARE(f.workflows->suggestedFolder().toLocalFile(), root + "/documents");
    }

    // Open and drop read only regular files, and only what a workflow needs: a pipe is never
    // opened (it would wait for a writer forever), a huge file is refused before it is read,
    // and a picture's workflow is found without reading the picture whole.
    void opensOnlyRegularFilesAndReadsOnlyTheWorkflow() {
        Fixture f;
        QVERIFY(::mkfifo(QFile::encodeName(f.file("pipe")).constData(), 0600) == 0);
        QVERIFY(!f.workflows->openPath(f.file("pipe")));
        QVERIFY2(f.workflows->notice().contains("not a file"), qPrintable(f.workflows->notice()));
        f.workflows->dismissNotice();
        f.workflows->dropUrls({QUrl::fromLocalFile(f.file("pipe"))}, 0, 0);
        QVERIFY2(f.workflows->notice().contains("not a file"), qPrintable(f.workflows->notice()));

        QFile video(f.file("video.mp4"));
        QVERIFY(video.open(QIODevice::WriteOnly) && video.resize(qint64(3) << 30)); // sparse
        video.close();
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!f.workflows->openPath(f.file("video.mp4")));
        QVERIFY2(f.workflows->notice().contains("too big"), qPrintable(f.workflows->notice()));

        QImage image(4, 4, QImage::Format_RGB32);
        image.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(image.save(&buffer, "PNG"));
        const Result<QByteArray> withFlow = pngWithWorkflow(png, textNode);
        QVERIFY(withFlow.ok());
        QFile picture(f.file("out.png"));
        QVERIFY(picture.open(QIODevice::WriteOnly) && picture.write(*withFlow) == withFlow->size());
        QVERIFY(picture.resize(qint64(3) << 30)); // a long tail after IEND
        picture.close();
        QVERIFY(f.workflows->openPath(f.file("out.png")));
        QCOMPARE(f.canvas.nodes()->rowCount(), 1);
        f.workflows->dropUrls({QUrl::fromLocalFile(f.file("out.png"))}, 0, 0);
        QCOMPARE(f.canvas.nodes()->rowCount(), 1);
        QVERIFY2(timer.elapsed() < 1500, qPrintable(QString::number(timer.elapsed())));
    }

    void anUnreadableFileIsANoticeNotACrash() {
        Fixture f;
        QVERIFY(write(f.file("bad.fugue"), "not json"));
        QVERIFY(!f.workflows->openPath(f.file("bad.fugue")));
        QVERIFY(f.workflows->notice().contains("bad.fugue"));
        QCOMPARE(f.workflows->count(), 1);
        QVERIFY(!f.workflows->openPath(f.file("missing.fugue")));
    }

    // --- recovery ---

    void writesARecoveryCopyAndNeverTouchesTheUsersFile() {
        Fixture f;
        QVERIFY(write(f.file("a.fugue"), textNode));
        QVERIFY(f.workflows->openPath(f.file("a.fugue")));
        const QByteArray before = read(f.file("a.fugue"));

        // Nothing unsaved, nothing written.
        f.workflows->writeRecoveryCopies();
        QVERIFY(!QFileInfo::exists(f.locations.recoveryDir) || QDir(f.locations.recoveryDir).isEmpty());

        f.canvas.addNode("fugue/text", 300, 0);
        f.workflows->writeRecoveryCopies();
        const QStringList copies = QDir(f.locations.recoveryDir).entryList({"*.fugue"});
        QCOMPARE(copies.size(), 1);
        QCOMPARE(read(f.file("a.fugue")), before);
        QCOMPARE(Document::loadFile(f.locations.recoveryDir + "/" + copies.first())->graph.nodes.size(), 2);

        // Saving makes the copy unnecessary.
        QVERIFY(f.workflows->save());
        QVERIFY(QDir(f.locations.recoveryDir).entryList({"*.fugue"}).isEmpty());
    }

    void offersTheRecoveryCopiesOnTheNextStartAndRecoversThem() {
        Fixture f;
        QVERIFY(write(f.file("a.fugue"), textNode));
        QVERIFY(f.workflows->openPath(f.file("a.fugue")));
        f.canvas.addNode("fugue/text", 300, 0);
        f.workflows->newWorkflow();
        f.canvas.addNode("fugue/number", 0, 0);
        f.workflows->writeRecoveryCopies();
        const QByteArray before = read(f.file("a.fugue"));

        // The app dies; the next start finds two copies.
        f.workflows.reset();
        CanvasModel again(&f.registry);
        auto next = f.restart(again);
        QCOMPARE(next->recoverable().size(), 2);
        QVERIFY(!next->unsaved());

        next->recover();
        QVERIFY(next->recoverable().isEmpty());
        // The empty first tab gave way, and both workflows are back, unsaved.
        QCOMPARE(next->count(), 2);
        QVERIFY(next->unsaved(0) && next->unsaved(1));
        QStringList paths{next->pathOf(0), next->pathOf(1)};
        paths.sort();
        QCOMPARE(paths, (QStringList{QString(), f.file("a.fugue")}));
        QCOMPARE(read(f.file("a.fugue")), before);

        // Saving the recovered workflow writes the user's file, with the recovered content.
        const int withFile = next->pathOf(0).isEmpty() ? 1 : 0;
        next->setCurrent(withFile);
        QCOMPARE(again.nodes()->rowCount(), 2);
        QVERIFY(next->save());
        QCOMPARE(Document::loadFile(f.file("a.fugue"))->graph.nodes.size(), 2);
    }

    // A second Fugue never offers (so never discards or duplicates) the copies of one still
    // running; the copies of one that ended, or died and left its lock, are offered to one
    // start only.
    void aRunningFuguesCopiesAreNotAnothersToOffer() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("recovery");
        const Document document = Document::load(textNode).value();
        const auto ids = [](const QList<RecoveryEntry> &entries) {
            QStringList list;
            for (const RecoveryEntry &entry : entries)
                list << entry.id;
            list.sort();
            return list;
        };
        QString lockPath;
        QByteArray lock;
        {
            RecoveryStore first(folder);
            QVERIFY(first.write("one", document, {}, "One").isEmpty());
            RecoveryStore second(folder);
            QVERIFY(second.list().isEmpty());
            QVERIFY(second.write("two", document, {}, "Two").isEmpty());
            QVERIFY(RecoveryStore(folder).list().isEmpty());
            lockPath = folder + "/owner-" + first.owner() + ".lock";
            lock = read(lockPath);
            QVERIFY(!lock.isEmpty());
        }
        // The first one died: its lock is still there, naming a process that is gone.
        QProcess gone;
        gone.start("true");
        QVERIFY(gone.waitForStarted());
        const qint64 pid = gone.processId();
        QVERIFY(gone.waitForFinished());
        QList<QByteArray> lines = lock.split('\n');
        lines[0] = QByteArray::number(pid);
        QVERIFY(write(lockPath, lines.join('\n')));

        RecoveryStore third(folder);
        QCOMPARE(ids(third.list()), (QStringList{"one", "two"}));
        // They are the third one's now: a fourth start does not offer them too.
        RecoveryStore fourth(folder);
        QVERIFY(fourth.list().isEmpty());
    }

    // A copy that cannot be read is kept aside, never deleted: it may be the only one.
    void anUnreadableRecoveryCopyIsKeptAside() {
        Fixture f;
        QVERIFY(QDir().mkpath(f.locations.recoveryDir));
        QVERIFY(write(f.locations.recoveryDir + "/lost.fugue", "not json"));
        CanvasModel again(&f.registry);
        auto next = f.restart(again);
        QCOMPARE(next->recoverable().size(), 1);
        next->recover();
        QVERIFY2(next->notice().contains("could not be read"), qPrintable(next->notice()));
        QVERIFY(!QFileInfo::exists(f.locations.recoveryDir + "/lost.fugue"));
        QCOMPARE(read(f.locations.recoveryDir + "/unreadable/lost.fugue"), QByteArray("not json"));
        QVERIFY2(next->notice().contains(f.locations.recoveryDir + "/unreadable"), qPrintable(next->notice()));
        CanvasModel third(&f.registry);
        QVERIFY(f.restart(third)->recoverable().isEmpty());
    }

    // Recovery copies and trust.json are the owner's alone (0600), whatever the umask.
    void recoveryCopiesAndTrustAreTheOwnersAlone() {
        Fixture f;
        const QFileDevice::Permissions owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
        const auto ownerOnly = [&](const QString &path) {
            QFile file(path);
            return file.exists() && (file.permissions() & ~(owner | QFileDevice::ReadUser | QFileDevice::WriteUser)) == 0;
        };
        f.canvas.addNode("fugue/text", 0, 0);
        f.workflows->writeRecoveryCopies();
        const QStringList copies = QDir(f.locations.recoveryDir).entryList({"*.fugue", "*.json"}, QDir::Files);
        QCOMPARE(copies.size(), 2); // the copy and its note
        for (const QString &name : copies)
            QVERIFY2(ownerOnly(f.locations.recoveryDir + "/" + name), qPrintable(name));
        QVERIFY(write(f.file("odd.fugue"), withMissingNode));
        QVERIFY(f.workflows->openPath(f.file("odd.fugue")));
        f.workflows->allowPendingTrust();
        QVERIFY(ownerOnly(f.locations.trustPath));
    }

    void discardingTheOfferDeletesTheCopies() {
        Fixture f;
        f.canvas.addNode("fugue/text", 0, 0);
        f.workflows->writeRecoveryCopies();
        f.workflows.reset();
        CanvasModel again(&f.registry);
        auto next = f.restart(again);
        QCOMPARE(next->recoverable().size(), 1);
        next->discardRecovery();
        QVERIFY(QDir(f.locations.recoveryDir).entryList({"*.fugue"}).isEmpty());
        CanvasModel third(&f.registry);
        QVERIFY(f.restart(third)->recoverable().isEmpty());
    }

    void closingATabRemovesItsRecoveryCopy() {
        Fixture f;
        f.canvas.addNode("fugue/text", 0, 0);
        f.workflows->newWorkflow(); // a second tab, so closing the first leaves a workflow behind
        f.workflows->writeRecoveryCopies();
        QCOMPARE(QDir(f.locations.recoveryDir).entryList({"*.fugue"}).size(), 1);
        f.workflows->discardAndClose(0);
        QVERIFY(QDir(f.locations.recoveryDir).entryList({"*.fugue"}).isEmpty());
    }

    void theTimerWritesTheCopies() {
        Fixture f;
        f.locations.recoveryIntervalMs = 20;
        CanvasModel other(&f.registry);
        Workflows quick(&other, &f.registry, &f.commands, f.locations);
        other.addNode("fugue/text", 0, 0);
        QTRY_VERIFY(!QDir(f.locations.recoveryDir).entryList({"*.fugue"}).isEmpty());
    }

    // --- missing models ---

    // Spec 4.3: on open, Fugue checks the model folders and says what is missing, where each
    // file can be had and where it goes, without blocking.
    void openingSaysWhichModelsAreMissingWhereToGetThemAndWhereTheyGo() {
        Fixture f;
        const QString hash = QStringLiteral("sha256:") + QString(64, u'3');
        QJsonObject model{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", "big-model.gguf"}};
        QJsonObject sourced = model;
        sourced.insert("source", "https://huggingface.co/example/big-model.gguf");
        const QJsonObject json{
            {"fugue", 1},
            {"graph", QJsonObject{{"nodes", QJsonObject{{"models", QJsonObject{
                {"type", "fugue/load-models"}, {"version", "1.0.0"},
                {"inputs", QJsonObject{{"diffusion_model", model}}}}}}}}},
            {"requires", QJsonObject{{"models", QJsonArray{sourced}}}}};
        QVERIFY(write(f.file("needs.fugue"), QJsonDocument(json).toJson()));
        QVERIFY(f.workflows->openPath(f.file("needs.fugue")));
        const QString notice = f.workflows->notice();
        QVERIFY2(notice.contains("big-model.gguf"), qPrintable(notice));
        QVERIFY2(notice.contains("https://huggingface.co/example/big-model.gguf"), qPrintable(notice));
        QVERIFY2(notice.contains(QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                                                 "/fugue/models/diffusion-model")),
                 qPrintable(notice));
        QCOMPARE(f.canvas.nodes()->rowCount(), 1); // it opened all the same
    }

    // --- missing nodes and trust ---

    void missingNodesOpenAsPlaceholdersKeepingInputsAndSurviveASave() {
        Fixture f;
        QVERIFY(write(f.file("odd.fugue"), withMissingNode));
        QVERIFY(f.workflows->openPath(f.file("odd.fugue")));
        QVERIFY(nodeRow(f.canvas, "odd").value("missing").toBool());
        QVERIFY(!nodeRow(f.canvas, "prompt").value("missing").toBool());
        QCOMPARE(f.canvas.links()->rowCount(), 2);
        QVERIFY(f.workflows->notice().contains("not installed (Tools)"));
        QVERIFY(!f.workflows->unsaved()); // opening is not an edit

        // Saving loses nothing: inputs, links and the version it was made with.
        QVERIFY(f.workflows->saveAs(f.file("copy.fugue")));
        const Document copy = Document::loadFile(f.file("copy.fugue")).value();
        const Node odd = copy.graph.nodes.value("odd");
        QCOMPARE(odd.type, QString("io.example.tools/sharpen"));
        QCOMPARE(odd.version.toString(), QString("2.0.0"));
        QCOMPARE(odd.inputs.size(), 2);
        QCOMPARE(odd.inputs.value("amount").literal().toInt(), 3);
        QCOMPARE(odd.inputs.value("image").from().toString(), QString("prompt.text"));
        QCOMPARE(copy.graph.nodes.value("show").inputs.value("value").from().toString(), QString("odd.image"));
    }

    void pastingNodesOfAnUnknownPackAsksForTrustLikeOpening() {
        Fixture f;
        QVERIFY(f.workflows->pendingTrust().isEmpty());
        QVERIFY(!f.canvas.paste(QString::fromUtf8(withMissingNode)).isEmpty());
        const QVariantList asked = f.workflows->pendingTrust();
        QCOMPARE(asked.size(), 1);
        QCOMPARE(asked.first().toMap().value("id").toString(), QString("io.example.tools"));
    }

    void theVersionAskedAboutIsTheInstalledOne() {
        Fixture f;
        f.registry.setPackVersion("io.example.tools", "3.1.0"); // the workflow says 2.0.0
        QVERIFY(write(f.file("odd.fugue"), withMissingNode));
        QVERIFY(f.workflows->openPath(f.file("odd.fugue")));
        QCOMPARE(f.workflows->pendingTrust().first().toMap().value("version").toString(), QString("3.1.0"));
    }

    void anUnknownPacksNodesAskForTrustAndTheAnswerIsRemembered() {
        Fixture f;
        QVERIFY(write(f.file("odd.fugue"), withMissingNode));
        QVERIFY(f.workflows->openPath(f.file("odd.fugue")));
        QSignalSpy changed(f.workflows.get(), &Workflows::pendingTrustChanged);
        const QVariantList asked = f.workflows->pendingTrust();
        QCOMPARE(asked.size(), 1);
        QCOMPARE(asked.first().toMap().value("id").toString(), QString("io.example.tools"));
        QCOMPARE(asked.first().toMap().value("version").toString(), QString("2.0.0"));
        QCOMPARE(asked.first().toMap().value("source").toString(), QString("https://example.com/tools"));
        QVERIFY(!f.workflows->isTrusted("io.example.tools", "2.0.0"));

        f.workflows->allowPendingTrust();
        QVERIFY(f.workflows->pendingTrust().isEmpty());
        QVERIFY(f.workflows->isTrusted("io.example.tools", "2.0.0"));
        // Per version: an update asks again.
        QVERIFY(!f.workflows->isTrusted("io.example.tools", "2.1.0"));
        QVERIFY(QFileInfo::exists(f.locations.trustPath));

        // Remembered across starts, so opening it again does not ask.
        CanvasModel again(&f.registry);
        auto next = f.restart(again);
        QVERIFY(next->openPath(f.file("odd.fugue")));
        QVERIFY(next->pendingTrust().isEmpty());
    }

    void notNowOpensTheWorkflowButTrustsNothing() {
        Fixture f;
        QVERIFY(write(f.file("odd.fugue"), withMissingNode));
        QVERIFY(f.workflows->openPath(f.file("odd.fugue")));
        f.workflows->dismissPendingTrust();
        QVERIFY(f.workflows->pendingTrust().isEmpty());
        QVERIFY(!f.workflows->isTrusted("io.example.tools", "2.0.0"));
        QCOMPARE(f.canvas.nodes()->rowCount(), 3);
    }

    void theBuiltInPacksAreNeverAsked() {
        Fixture f;
        QVERIFY(write(f.file("a.fugue"), textNode));
        QVERIFY(f.workflows->openPath(f.file("a.fugue")));
        QVERIFY(f.workflows->pendingTrust().isEmpty());
        QVERIFY(f.workflows->notice().isEmpty());
        QVERIFY(f.workflows->isTrusted("fugue", ""));
        QVERIFY(f.workflows->isTrusted("local", ""));
    }

    // --- import ---

    void dropsAnOutputWithAnEmbeddedWorkflow() {
        Fixture f;
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(f.file("plain.png"), "PNG"));
        const Result<QByteArray> png = pngWithWorkflow(read(f.file("plain.png")), textNode);
        QVERIFY(png.ok());
        QVERIFY(write(f.file("output.png"), png.value()));

        f.workflows->openUrls({QUrl::fromLocalFile(f.file("output.png"))});
        QCOMPARE(f.canvas.nodes()->rowCount(), 1);
        // It is a new workflow with no file: Save asks where, and never writes the image.
        QCOMPARE(f.workflows->pathOf(f.workflows->current()), QString());
        QCOMPARE(f.workflows->tabs().at(f.workflows->current()).toMap().value("title").toString(), QString("output"));
        QSignalSpy asked(f.workflows.get(), &Workflows::saveAsRequested);
        f.workflows->save();
        QCOMPARE(asked.size(), 1);

        // An image with no workflow is a notice.
        QVERIFY(!f.workflows->openPath(f.file("plain.png")));
        QVERIFY(f.workflows->notice().contains("plain.png"));
    }

    void importsJsonFromTheClipboardText() {
        Fixture f;
        QVERIFY(f.workflows->openText(QString("  \n") + textNode + "\n"));
        QCOMPARE(f.canvas.nodes()->rowCount(), 1);
        QVERIFY(!f.workflows->unsaved());
        QVERIFY(!f.workflows->openText("just some words"));
        QVERIFY(f.workflows->notice().contains("not a Fugue workflow"));
        QCOMPARE(f.workflows->count(), 1);
    }

    void aFileOnlyOpensOnceAndNonFilesAreRefused() {
        Fixture f;
        QVERIFY(write(f.file("a.fugue"), textNode));
        QVERIFY(write(f.file("b.fugue"), textNode));
        QVERIFY(f.workflows->openPath(f.file("a.fugue")));
        QVERIFY(f.workflows->openPath(f.file("b.fugue")));
        QCOMPARE(f.workflows->count(), 2);
        QVERIFY(f.workflows->openPath("file://" + f.file("a.fugue")));
        QCOMPARE(f.workflows->count(), 2);
        QCOMPARE(f.workflows->current(), 0);
        f.workflows->openUrls({QUrl("https://example.com/x.fugue")});
        QVERIFY(f.workflows->notice().contains("Only files"));
    }

    // --- tabs ---

    void tabsKeepTheirOwnDocumentsAndUnsavedState() {
        Fixture f;
        QVERIFY(write(f.file("a.fugue"), textNode));
        QVERIFY(f.workflows->openPath(f.file("a.fugue")));
        f.canvas.addNode("fugue/text", 300, 0);
        f.workflows->newWorkflow();
        QCOMPARE(f.workflows->count(), 2);
        QCOMPARE(f.canvas.nodes()->rowCount(), 0);
        QVERIFY(f.workflows->unsaved(0));
        QVERIFY(!f.workflows->unsaved(1));
        QVERIFY(f.workflows->unsaved()); // the quit question sees every tab
        f.canvas.addNode("fugue/number", 0, 0);

        QSignalSpy switched(f.workflows.get(), &Workflows::currentChanged);
        f.workflows->setCurrent(0);
        QCOMPARE(switched.size(), 1);
        QCOMPARE(switched.first().at(0).toInt(), 1);
        QCOMPARE(f.canvas.nodes()->rowCount(), 2);
        f.workflows->setCurrent(1);
        QCOMPARE(f.canvas.nodes()->rowCount(), 1);
        QCOMPARE(f.canvas.document().graph.nodes.first().type, QString("fugue/number"));
        QCOMPARE(f.workflows->tabs().at(1).toMap().value("title").toString(), QString("Untitled"));
    }

    void closingAnUnsavedTabAsksAndEachAnswerWorks() {
        Fixture f;
        QVERIFY(write(f.file("a.fugue"), textNode));
        QVERIFY(f.workflows->openPath(f.file("a.fugue")));
        f.canvas.addNode("fugue/text", 300, 0);
        f.workflows->newWorkflow();
        QSignalSpy asked(f.workflows.get(), &Workflows::closeConfirmationRequested);
        QSignalSpy closed(f.workflows.get(), &Workflows::tabClosed);

        // Cancel: the question is asked and nothing closes.
        f.workflows->closeTab(0);
        QCOMPARE(asked.size(), 1);
        QCOMPARE(asked.first().at(1).toString(), QString("a.fugue"));
        f.workflows->cancelPendingClose();
        QCOMPARE(f.workflows->count(), 2);

        // Save: written, then closed.
        f.workflows->saveAndClose(0);
        QCOMPARE(f.workflows->count(), 1);
        QCOMPARE(Document::loadFile(f.file("a.fugue"))->graph.nodes.size(), 2);
        QCOMPARE(closed.size(), 1);

        // Discard, on a tab with no file: save-as comes first when saving.
        f.canvas.addNode("fugue/number", 0, 0);
        QSignalSpy where(f.workflows.get(), &Workflows::saveAsRequested);
        f.workflows->saveAndClose(0);
        QCOMPARE(where.size(), 1);
        QVERIFY(f.workflows->saveAs(f.file("n.fugue")));
        QVERIFY(QFileInfo::exists(f.file("n.fugue")));
        // It was the only tab, so a fresh workflow takes its place.
        QCOMPARE(f.workflows->count(), 1);
        QCOMPARE(f.canvas.nodes()->rowCount(), 0);
        QVERIFY(f.workflows->pathOf(0).isEmpty());

        f.canvas.addNode("fugue/number", 0, 0);
        f.workflows->discardAndClose(0);
        QCOMPARE(f.canvas.nodes()->rowCount(), 0);
    }

    // #59: the tab menu's commands.
    void renameDuplicateAndCloseOthersWork() {
        Fixture f;
        QVERIFY(write(f.file("a.fugue"), textNode));
        QVERIFY(write(f.file("taken.fugue"), textNode));
        QVERIFY(f.workflows->openPath(f.file("a.fugue")));
        f.workflows->setTabColor(0, 3);
        // A saved workflow's file is renamed, keeps its color, ".fugue" is added, a taken name is refused.
        QVERIFY(f.workflows->renameTab(0, "b"));
        QVERIFY(!QFileInfo::exists(f.file("a.fugue")));
        QVERIFY(QFileInfo::exists(f.file("b.fugue")));
        QCOMPARE(f.workflows->pathOf(0), f.file("b.fugue"));
        QCOMPARE(f.workflows->tabs().first().toMap().value("title").toString(), QString("b.fugue"));
        QCOMPARE(f.workflows->tabs().first().toMap().value("color").toInt(), 3);
        QVERIFY(!f.workflows->renameTab(0, "taken.fugue"));
        QVERIFY(f.workflows->notice().contains("already exists"));
        QVERIFY(QFileInfo::exists(f.file("b.fugue")));
        QVERIFY(!f.workflows->renameTab(0, "x/y"));
        QVERIFY(!f.workflows->renameTab(0, "  "));

        // Duplicate: a new, unsaved tab with the same nodes, shown.
        const int nodes = f.canvas.nodes()->rowCount();
        f.workflows->duplicateTab(0);
        QCOMPARE(f.workflows->count(), 2);
        QCOMPARE(f.workflows->current(), 1);
        QCOMPARE(f.canvas.nodes()->rowCount(), nodes);
        QVERIFY(f.workflows->pathOf(1).isEmpty());
        QCOMPARE(f.workflows->tabs().at(1).toMap().value("title").toString(), QString("b copy"));
        QVERIFY(f.workflows->tabs().at(1).toMap().value("unsaved").toBool());
        // An unsaved tab renames in place.
        QVERIFY(f.workflows->renameTab(1, "scratch"));
        QCOMPARE(f.workflows->tabs().at(1).toMap().value("title").toString(), QString("scratch"));

        // Close others keeps the tab and any with unsaved content (and says so).
        f.workflows->newWorkflow();
        f.workflows->newWorkflow();
        QCOMPARE(f.workflows->count(), 4);
        f.workflows->closeOtherTabs(0);
        QCOMPARE(f.workflows->count(), 2); // b.fugue and the unsaved copy
        QCOMPARE(f.workflows->current(), 0);
        QVERIFY(f.workflows->notice().contains("unsaved"));
    }

    void aCleanTabClosesWithoutAskingAndTheWindowCloseCommandUsesIt() {
        Fixture f;
        registerFileCommands(f.commands, *f.workflows);
        QVERIFY(write(f.file("a.fugue"), textNode));
        QVERIFY(f.workflows->openPath(f.file("a.fugue")));
        f.workflows->newWorkflow();
        QSignalSpy asked(f.workflows.get(), &Workflows::closeConfirmationRequested);
        QVERIFY(f.commands.run("file.close")); // closes the (empty) current tab
        QCOMPARE(asked.size(), 0);
        QCOMPARE(f.workflows->count(), 1);
        QCOMPARE(f.workflows->current(), 0);
        QCOMPARE(f.canvas.nodes()->rowCount(), 1);
    }

    void theFileCommandsAreWired() {
        Fixture f;
        registerFileCommands(f.commands, *f.workflows);
        QSignalSpy open(f.workflows.get(), &Workflows::openDialogRequested);
        QSignalSpy saveAs(f.workflows.get(), &Workflows::saveAsRequested);
        QVERIFY(f.commands.run("file.open"));
        QVERIFY(f.commands.run("file.save-as"));
        QCOMPARE(open.size(), 1);
        QCOMPARE(saveAs.size(), 1);
        QVERIFY(f.commands.run("file.new"));
        QCOMPARE(f.workflows->count(), 2);
        QVERIFY(f.commands.run("file.open-clipboard"));
        QVERIFY(f.commands.run("file.save"));
        QVERIFY(f.commands.hasHandler("file.open-clipboard"));
    }

    // --- the shipped example ---

    void theDemoWorkflowLoadsCleanAndCompiles() {
        Fixture f;
        const QString demo = QStringLiteral(FUGUE_SOURCE_DIR "/examples/demo.fugue");
        QVERIFY(f.workflows->openPath(demo));
        QVERIFY(f.workflows->notice().isEmpty());
        QVERIFY(f.workflows->pendingTrust().isEmpty());
        QVERIFY(!f.workflows->unsaved());
        QCOMPARE(f.canvas.nodes()->rowCount(), 6);
        QCOMPARE(f.canvas.links()->rowCount(), 4);
        for (int i = 0; i < f.canvas.nodes()->rowCount(); ++i)
            QVERIFY(!f.canvas.nodes()->data(f.canvas.nodes()->index(i), NodeList::Missing).toBool());

        const Compilation compiled = compile(Document::loadFile(demo).value(), f.registry);
        QVERIFY2(compiled.ok(), qPrintable(compiled.error().message));
        QVERIFY(compiled->nodes.contains("show-prompt"));
        QVERIFY(!hasErrors(compiled.problems()));
    }

    // --- the window ---

    void theWindowShowsTheTabsAndTheNotice() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language");
        QCoreApplication::installTranslator(&translator);
        Theme theme(themeFolder(dir));
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        CanvasModel canvas(&registry);
        CommandRegistry commands;
        registerDefaultCommands(commands);
        WorkflowLocations locations;
        locations.recoveryDir = dir.filePath("recovery");
        locations.trustPath = dir.filePath("trust.json");
        Workflows workflows(&canvas, &registry, &commands, locations);
        registerFileCommands(commands, workflows);
        QVERIFY(write(dir.filePath("odd.fugue"), withMissingNode));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        fugue::exposeToQml(engine, {&translator, &theme, &commands, &canvas, &workflows});
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QVERIFY(window->findChild<QObject *>("workflowTabs"));
        QVERIFY(workflows.openPath(dir.filePath("odd.fugue")));
        workflows.newWorkflow();
        QTRY_VERIFY(item(window->contentItem(), "tab-1"));
        QCOMPARE(window->title(), QString("Untitled — Fugue"));
        // The unknown pack's question and the missing-node notice are up.
        workflows.setCurrent(0);
        auto *trust = window->findChild<QObject *>("trustAsk");
        QVERIFY(trust);
        QTRY_VERIFY(trust->property("visible").toBool());
        QVERIFY(item(window->contentItem(), "workflowNotice")->isVisible());
        QMetaObject::invokeMethod(window->findChild<QObject *>("trustAllow"), "clicked");
        QTRY_VERIFY(!trust->property("visible").toBool());
        QVERIFY(workflows.isTrusted("io.example.tools", "2.0.0"));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join("\n")));
    }
};

QTEST_MAIN(TestWorkflows)
#include "test_workflows.moc"
