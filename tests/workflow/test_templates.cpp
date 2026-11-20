// Workflow templates (spec 11.10): folders of a workflow, its description, a preview and its
// sample inputs, listed from the roots and copied out as new workflows.
//
// Seam: TemplateLibrary (list, problems, find, copyTo) over real folders, with the built-in
// node types; Workflows::openTemplate for the window.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "engine/templates.h"
#include "menu/menu_commands.h"
#include "nodes/builtin.h"
#include "qml_app.h"
#include "theme.h"
#include "translator.h"
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

void writeJson(const QString &path, const QJsonObject &json) { writeFile(path, QJsonDocument(json).toJson()); }

QJsonObject node(const QString &type, const QJsonObject &inputs = {}, const QString &version = "1.0.0") {
    return {{"type", type}, {"version", version}, {"inputs", inputs}};
}

QJsonObject workflow(const QJsonObject &nodes, const QJsonObject &needs = {}) {
    QJsonObject json{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}};
    if (!needs.isEmpty())
        json.insert("requires", needs);
    return json;
}

// A template folder with a text node (a workflow any Fugue opens).
void makeTemplate(const QString &folder, const QJsonObject &about,
                  const QJsonObject &flow = workflow({{"prompt", node("fugue/text", {{"text", "a fox"}})}})) {
    writeJson(folder + "/template.json", about);
    writeJson(folder + "/workflow.fugue", flow);
}

// The smallest PNG (one transparent pixel), and another one.
QByteArray png(char tweak = 0) {
    QByteArray bytes = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=");
    if (tweak)
        bytes.append(tweak); // bytes after IEND: still a PNG, other bytes
    return bytes;
}

QStringList ids(const QList<Template> &templates) {
    QStringList out;
    for (const Template &t : templates)
        out << t.id;
    return out;
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

struct Fixture {
    QTemporaryDir bundled, own;
    Registry registry;
    Fixture() { nodes::addBuiltinNodeTypes(registry); }
    TemplateLibrary library() const {
        return TemplateLibrary({{own.path(), true}, {bundled.path(), false}}, &registry);
    }
};

} // namespace

class TestTemplates : public QObject {
    Q_OBJECT

private slots:
    void listsTheOwnFirstThenByCategoryOrderThenTitle() {
        Fixture f;
        writeJson(f.bundled.filePath("categories.json"),
                  {{"categories", QJsonArray{QJsonObject{{"id", "edit"}, {"title", "Image editing"}},
                                             QJsonObject{{"id", "image"}, {"title", "Image generation"}}}}});
        makeTemplate(f.bundled.filePath("zeta"), {{"title", "Zeta"}, {"category", "image"}});
        makeTemplate(f.bundled.filePath("alpha"), {{"title", "Alpha"}, {"category", "image"},
                                                   {"description", "A fox."}, {"tags", QJsonArray{"Klein"}}});
        makeTemplate(f.bundled.filePath("fix"), {{"title", "Fix"}, {"category", "edit"}});
        makeTemplate(f.bundled.filePath("loose"), {{"title", "Loose"}, {"category", "misc"}});
        writeFile(f.bundled.filePath("alpha/preview.webp"), "webp bytes");
        // The person's own: a folder and a plain workflow file.
        makeTemplate(f.own.filePath("mine"), {{"title", "Mine"}});
        writeJson(f.own.filePath("quick sketch.fugue"), workflow({{"p", node("fugue/text")}}));

        const QList<Template> listed = f.library().list();
        QCOMPARE(ids(listed), (QStringList{"mine", "quick sketch", "fix", "alpha", "zeta", "loose"}));
        QVERIFY(listed.at(0).own);
        QCOMPARE(listed.at(1).title, QStringLiteral("quick sketch"));
        QCOMPARE(listed.at(1).workflow, f.own.filePath("quick sketch.fugue"));
        const Template &alpha = listed.at(3);
        QCOMPARE(alpha.categoryTitle, QStringLiteral("Image generation"));
        QCOMPARE(alpha.description, QStringLiteral("A fox."));
        QCOMPARE(alpha.tags, QStringList{"Klein"});
        QCOMPARE(alpha.preview, f.bundled.filePath("alpha/preview.webp"));
        QCOMPARE(alpha.workflow, f.bundled.filePath("alpha/workflow.fugue"));
        QVERIFY(listed.at(2).preview.isEmpty());
        QCOMPARE(listed.at(5).categoryTitle, QStringLiteral("misc"));
        QVERIFY(f.library().problems().isEmpty());
    }

    void thePersonsOwnTemplateWinsAClash() {
        Fixture f;
        makeTemplate(f.bundled.filePath("same"), {{"title", "Bundled"}});
        makeTemplate(f.own.filePath("same"), {{"title", "Own"}});
        const QList<Template> listed = f.library().list();
        QCOMPARE(listed.size(), 1);
        QCOMPARE(listed.at(0).title, QStringLiteral("Own"));
        QCOMPARE(f.library().find("same")->title, QStringLiteral("Own"));
        QVERIFY(!f.library().find("other"));
    }

    // 3.6: a built-in node this Fugue lacks, or has at another major or an older minor, means a
    // newer Fugue made it: not listed, and not a problem. A pack's node is listed, its pack named.
    void aTemplateOfANewerFugueIsLeftOutAndAMissingPackNamed() {
        Fixture f;
        makeTemplate(f.bundled.filePath("newer-minor"), {{"title", "A"}},
                     workflow({{"p", node("fugue/text", {}, "1.9.0")}}));
        makeTemplate(f.bundled.filePath("newer-major"), {{"title", "B"}},
                     workflow({{"p", node("fugue/text", {}, "2.0.0")}}));
        makeTemplate(f.bundled.filePath("unknown"), {{"title", "C"}}, workflow({{"p", node("fugue/teleport")}}));
        makeTemplate(f.bundled.filePath("packed"), {{"title", "D"}},
                     workflow({{"p", node("io.example.tools/sharpen")}, {"q", node("fugue/text")}}));
        makeTemplate(f.bundled.filePath("older"), {{"title", "E"}}, workflow({{"p", node("fugue/text", {}, "1.0.0")}}));

        const QList<Template> listed = f.library().list();
        QCOMPARE(ids(listed), (QStringList{"packed", "older"}));
        QCOMPARE(listed.at(0).neededPacks, QStringList{"io.example.tools"});
        QVERIFY(f.library().problems().isEmpty());
    }

    void theModelsItNeedsComeFromItsWorkflow() {
        Fixture f;
        const QString hash = QStringLiteral("sha256:") + QString(64, u'4');
        const QJsonObject model{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", "m.gguf"}};
        QJsonObject sourced = model;
        sourced.insert("source", "https://huggingface.co/x/m.gguf");
        makeTemplate(f.bundled.filePath("t"), {{"title", "T"}},
                     workflow({{"models", node("fugue/load-models", {{"diffusion_model", model}})}},
                              {{"models", QJsonArray{sourced}}}));
        const QList<Template> listed = f.library().list();
        QCOMPARE(listed.size(), 1);
        QCOMPARE(listed.at(0).models, QJsonArray{sourced});
    }

    void problemsNameTheBrokenTemplates() {
        Fixture f;
        writeFile(f.bundled.filePath("bad-json/template.json"), "{ not json");
        writeJson(f.bundled.filePath("bad-json/workflow.fugue"), workflow({{"p", node("fugue/text")}}));
        makeTemplate(f.bundled.filePath("untitled"), {{"description", "no title"}});
        writeJson(f.bundled.filePath("no-workflow/template.json"), {{"title", "X"}});
        makeTemplate(f.bundled.filePath("lost-input"), {{"title", "Y"}},
                     workflow({{"load", node("fugue/load-image", {{"file", "inputs/gone.png"}})}}));
        makeTemplate(f.bundled.filePath("fine"), {{"title", "Z"}},
                     workflow({{"load", node("fugue/load-image", {{"file", "inputs/here.png"}})}}));
        writeFile(f.bundled.filePath("fine/inputs/here.png"), png());

        QCOMPARE(ids(f.library().list()), (QStringList{"lost-input", "fine"}));
        const Errors problems = f.library().problems();
        QStringList where;
        for (const Error &problem : problems)
            where << problem.message;
        const QString all = where.join('\n');
        QCOMPARE(problems.size(), 4);
        for (const char *name : {"bad-json", "untitled", "no-workflow", "gone.png"})
            QVERIFY2(all.contains(name), qPrintable(all));
    }

    void copyWritesTheWorkflowWithItsInputsBesideIt() {
        Fixture f;
        makeTemplate(f.bundled.filePath("edit"), {{"title", "Edit"}},
                     workflow({{"load", node("fugue/load-image", {{"file", "inputs/cat.png"}})}}));
        writeFile(f.bundled.filePath("edit/inputs/cat.png"), png());
        const Template chosen = *f.library().find("edit");
        QTemporaryDir out;

        QVERIFY(f.library().copyTo(chosen, out.filePath("mine.fugue")).isEmpty());
        QCOMPARE(readFile(out.filePath("inputs/cat.png")), png());
        const Document copied = Document::loadFile(out.filePath("mine.fugue")).value();
        QCOMPARE(copied.graph.nodes.value("load").inputs.value("file").literal().toString(), QString("inputs/cat.png"));

        // A second copy beside it shares the same bytes; it never replaces a workflow.
        QVERIFY(f.library().copyTo(chosen, out.filePath("again.fugue")).isEmpty());
        QCOMPARE(f.library().copyTo(chosen, out.filePath("mine.fugue")).value(0).code, QString("usage"));

        // Another picture under that name stays; the sample takes the next name, and the new
        // workflow names it.
        writeFile(out.filePath("inputs/cat.png"), png('x'));
        QVERIFY(f.library().copyTo(chosen, out.filePath("third.fugue")).isEmpty());
        QCOMPARE(readFile(out.filePath("inputs/cat.png")), png('x'));
        QCOMPARE(readFile(out.filePath("inputs/cat-2.png")), png());
        const Document third = Document::loadFile(out.filePath("third.fugue")).value();
        QCOMPARE(third.graph.nodes.value("load").inputs.value("file").literal().toString(), QString("inputs/cat-2.png"));
    }

    // A sample that leads out of its template, by a link, is not read.
    void aSampleLinkedOutOfItsFolderIsNotCopied() {
        Fixture f;
        QTemporaryDir elsewhere;
        writeFile(elsewhere.filePath("secret.png"), png());
        makeTemplate(f.bundled.filePath("sly"), {{"title", "Sly"}},
                     workflow({{"load", node("fugue/load-image", {{"file", "inputs/x.png"}})}}));
        QDir().mkpath(f.bundled.filePath("sly/inputs"));
        QVERIFY(QFile::link(elsewhere.filePath("secret.png"), f.bundled.filePath("sly/inputs/x.png")));
        QVERIFY(!f.library().problems().isEmpty()); // named as a sample not in its folder
        QTemporaryDir out;
        QVERIFY(f.library().copyTo(*f.library().find("sly"), out.filePath("w.fugue")).isEmpty());
        QVERIFY(!QFileInfo::exists(out.filePath("inputs/x.png")));
    }

    // A person's own template that is broken still hides the bundled one of its id.
    void aBrokenOwnTemplateStillHidesTheBundledOne() {
        Fixture f;
        makeTemplate(f.bundled.filePath("same"), {{"title", "Bundled"}});
        writeFile(f.own.filePath("same/template.json"), "{ broken");
        writeJson(f.own.filePath("same/workflow.fugue"), workflow({{"p", node("fugue/text")}}));
        QVERIFY(f.library().list().isEmpty());
        QCOMPARE(f.library().problems().value(0).code, QString("invalid_json"));
    }

    // The window opens a template as a new workflow with no file, its sample inputs taken into
    // the inputs folder as a pasted image is, so it runs as it opens and a save keeps it portable.
    void theWindowOpensATemplateAsANewWorkflowWithItsInputs() {
        Fixture f;
        makeTemplate(f.bundled.filePath("edit"), {{"title", "Klein edit"}},
                     workflow({{"load", node("fugue/load-image", {{"file", "inputs/cat.png"}})}}));
        writeFile(f.bundled.filePath("edit/inputs/cat.png"), png());
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");

        // What the dialog lists: a Load Models without its model says so.
        const QString hash = QStringLiteral("sha256:") + QString(64, u'6');
        makeTemplate(f.bundled.filePath("gen"), {{"title", "Gen"}},
                     workflow({{"models", node("fugue/load-models", {{"diffusion_model",
                                QJsonObject{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", "m.gguf"}}}})}}));
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        const QVariantList listed = workflows.templates();
        QCOMPARE(listed.size(), 2);
        const QVariantMap gen = listed.at(0).toMap();
        QCOMPARE(gen.value("id").toString(), QString("gen"));
        QCOMPARE(gen.value("status").toString(), QString("Models missing: 1 of 1"));
        QCOMPARE(gen.value("missingModels").toList().at(0).toMap().value("name").toString(), QString("m.gguf"));
        QVERIFY(listed.at(1).toMap().value("status").toString().isEmpty()); // the edit is ready

        QVERIFY(workflows.openTemplateById("edit"));
        QCOMPARE(workflows.count(), 1); // the empty Untitled it started with is replaced
        QCOMPARE(workflows.tabs().at(0).toMap().value("title").toString(), QString("Klein edit"));
        QVERIFY(workflows.pathOf(0).isEmpty());
        QVERIFY(workflows.unsaved());
        const QString file = canvas.document().graph.nodes.value("load").inputs.value("file").literal().toString();
        QVERIFY2(file.startsWith(inputsDir() + "/"), qPrintable(file));
        QCOMPARE(readFile(file), png());
    }

    // File > Browse templates opens the dialog; a card shows what the template lacks, a category
    // narrows the cards, and a click opens the template and closes the dialog.
    void theDialogListsTheCardsAndAClickOpensOne() {
        Fixture f;
        writeJson(f.bundled.filePath("categories.json"),
                  {{"categories", QJsonArray{QJsonObject{{"id", "image"}, {"title", "Image generation"}}}}});
        const QString hash = QStringLiteral("sha256:") + QString(64, u'7');
        makeTemplate(f.bundled.filePath("gen"), {{"title", "Gen"}, {"category", "image"}, {"description", "Makes one."}},
                     workflow({{"models", node("fugue/load-models", {{"diffusion_model",
                                QJsonObject{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", "m.gguf"}}}})}}));
        makeTemplate(f.bundled.filePath("talk"), {{"title", "Talk"}, {"category", "text"}});
        QTemporaryDir dir;
        QFile shell(dir.filePath("shell.toml"));
        QVERIFY(shell.open(QIODevice::WriteOnly));
        shell.write("[font]\nbase-size = 12\n");
        shell.close();
        Translator translator(dir.path() + "/language");
        Theme theme(dir.path());
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        registerDefaultCommands(commands);
        WorkflowLocations locations;
        locations.recoveryDir = dir.filePath("recovery");
        locations.trustPath = dir.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        registerMenuCommands(commands); // as main.cpp does, before the file commands
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        registerFileCommands(commands, workflows);

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

        QVERIFY(commands.run("file.templates"));
        QTRY_VERIFY(window->findChild<QObject *>("templatesDialog")
                    && window->findChild<QObject *>("templatesDialog")->property("opened").toBool());
        QTRY_VERIFY(item(window->contentItem(), "templateCard-gen"));
        QTRY_VERIFY(item(window->contentItem(), "templateCard-talk"));
        QCOMPARE(item(window->contentItem(), "templateStatus-gen")->property("text").toString(),
                 QString("Models missing: 1 of 1"));
        QVERIFY(!item(window->contentItem(), "templateStatus-talk")->isVisible());

        // A category shows its own; the search narrows by every word.
        QQuickItem *image = item(window->contentItem(), "templatesCategory-image");
        QVERIFY(image);
        QTest::mouseClick(window, Qt::LeftButton, {}, image->mapToScene(QPointF(image->width() / 2, image->height() / 2)).toPoint());
        QTRY_COMPARE(window->findChild<QObject *>("templatesDialog")->property("category").toString(), QString("image"));
        QTRY_VERIFY(!item(window->contentItem(), "templateCard-talk"));
        QQuickItem *all = item(window->contentItem(), "templatesCategory-all");
        QTest::mouseClick(window, Qt::LeftButton, {}, all->mapToScene(QPointF(all->width() / 2, all->height() / 2)).toPoint());
        QTRY_VERIFY(item(window->contentItem(), "templateCard-talk"));

        QQuickItem *card = item(window->contentItem(), "templateCard-talk");
        QTest::mouseClick(window, Qt::LeftButton, {}, card->mapToScene(QPointF(card->width() / 2, card->height() / 2)).toPoint());
        QTRY_COMPARE(workflows.tabs().at(workflows.current()).toMap().value("title").toString(), QString("Talk"));
        QTRY_VERIFY(!window->findChild<QObject *>("templatesDialog")->property("visible").toBool());

        // From the keyboard: the search field has the keys, Enter opens the first match.
        QVERIFY(commands.run("file.templates"));
        QTRY_VERIFY(window->findChild<QObject *>("templatesDialog")->property("opened").toBool());
        QTRY_VERIFY(item(window->contentItem(), "templatesSearch")->hasActiveFocus());
        for (const char c : {'g', 'e', 'n'})
            QTest::keyClick(window, c);
        QTRY_VERIFY(!item(window->contentItem(), "templateCard-talk"));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(workflows.tabs().at(workflows.current()).toMap().value("title").toString(), QString("Gen"));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join("\n")));
    }

    // The bundled templates (the templates submodule, fugue-workflows at the pinned commit): this
    // Fugue lists every one with nothing wrong, each compiles and opens with every node known,
    // and each model it names has a source.
    void theBundledTemplatesAreWholeAndCompile() {
        const QString root = QStringLiteral(FUGUE_SOURCE_DIR "/templates");
        QVERIFY2(QFileInfo(root + "/categories.json").isFile(),
                 "the templates submodule is not checked out: git submodule update --init templates");
        Fixture f;
        const TemplateLibrary library({{root, false}}, &f.registry);
        const Errors problems = library.problems();
        QStringList messages;
        for (const Error &problem : problems)
            messages << problem.message;
        QVERIFY2(problems.isEmpty(), qPrintable(messages.join('\n')));
        const QList<Template> listed = library.list();
        int folders = 0;
        for (const QFileInfo &entry : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            folders += QFileInfo(entry.filePath() + "/template.json").isFile() ? 1 : 0;
        QCOMPARE(listed.size(), folders); // none made by a newer Fugue than this one
        QVERIFY(listed.size() >= 9);

        for (const Template &t : listed) {
            QVERIFY2(t.neededPacks.isEmpty(), qPrintable(t.id));
            QVERIFY2(!t.preview.isEmpty(), qPrintable(t.id));
            for (const QJsonValue model : t.models)
                QVERIFY2(model.toObject().value("source").toString().startsWith("https://"), qPrintable(t.id));
            const Document document = Document::loadFile(t.workflow).value();
            const Compilation compiled = compile(document, f.registry);
            QVERIFY2(compiled.ok(), qPrintable(t.id + ": " + compiled.error().message));
            QVERIFY2(!hasErrors(compiled.problems()), qPrintable(t.id));
            // A size is always set, by a node, never left to a model's default (#72).
            if (document.graph.nodes.contains("generate"))
                QCOMPARE(document.graph.nodes.value("generate").inputs.value("width").kind(), InputValue::Kind::Link);
        }
    }
};

QTEST_MAIN(TestTemplates)
#include "test_templates.moc"
