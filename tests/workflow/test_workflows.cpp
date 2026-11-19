// Save, open, recovery, import, trust and tabs (spec 11, 12.10, 6.7), on the real Workflows
// with a canvas model and the built-in node types, and the window that shows them.

#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/compiler.h"
#include "engine/embed.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
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

    void aFailedSaveKeepsTheWorkAndSaysWhy() {
        Fixture f;
        f.canvas.addNode("fugue/text", 0, 0);
        QVERIFY(!f.workflows->saveAs(f.file("no-such-folder/x.fugue")));
        QVERIFY(f.workflows->unsaved());
        QVERIFY(f.workflows->notice().contains("Could not save"));
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

    void discardingTheOfferDeletesTheCopies() {
        Fixture f;
        f.canvas.addNode("fugue/text", 0, 0);
        f.workflows->writeRecoveryCopies();
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

    // --- missing nodes and trust ---

    void missingNodesOpenAsPlaceholdersKeepingInputsAndSurviveASave() {
        Fixture f;
        QVERIFY(write(f.file("odd.fugue"), withMissingNode));
        QVERIFY(f.workflows->openPath(f.file("odd.fugue")));
        QVERIFY(nodeRow(f.canvas, "odd").value("missing").toBool());
        QVERIFY(!nodeRow(f.canvas, "prompt").value("missing").toBool());
        QCOMPARE(f.canvas.links()->rowCount(), 2);
        QVERIFY(f.workflows->notice().contains("io.example.tools/sharpen"));
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

    void textToImageExampleLoadsAndCompiles() {
        Fixture f;
        const QString example = QStringLiteral(FUGUE_SOURCE_DIR "/examples/text-to-image.fugue");
        QVERIFY(f.workflows->openPath(example));
        QVERIFY(f.workflows->notice().isEmpty());
        QCOMPARE(f.canvas.nodes()->rowCount(), 6);
        for (int i = 0; i < f.canvas.nodes()->rowCount(); ++i)
            QVERIFY(!f.canvas.nodes()->data(f.canvas.nodes()->index(i), NodeList::Missing).toBool());
        const Compilation compiled = compile(Document::loadFile(example).value(), f.registry);
        QVERIFY2(compiled.ok(), qPrintable(compiled.error().message));
        QVERIFY(compiled->nodes.contains("generate"));
        QVERIFY(!hasErrors(compiled.problems()));
    }

    // --- the window ---

    void theWindowShowsTheTabsAndTheNotice() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language");
        QCoreApplication::installTranslator(&translator);
        Theme theme(themeFolder(dir));
        Backend backend;
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
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("canvasModel", &canvas);
        engine.rootContext()->setContextProperty("commands", &commands);
        engine.rootContext()->setContextProperty("workflows", &workflows);
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
