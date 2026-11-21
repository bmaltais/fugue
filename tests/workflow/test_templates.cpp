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
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/model_library.h"
#include "engine/paths.h"
#include "engine/settings.h"
#include "engine/registry.h"
#include "engine/templates.h"
#include "menu/menu_commands.h"
#include "nodes/builtin.h"
#include "qml_app.h"
#include "theme.h"
#include "translator.h"
#include "workflow/workflows.h"
#include "../engine/fake_http.h"

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

// Why the bundled templates (the `templates` submodule) cannot be tested, or empty when they
// are there: the folder is missing or empty (the submodule was never initialised) or has no
// categories.json (an old checkout). The same advice both times.
QString bundledProblem(const QString &root) {
    const QString fix = QStringLiteral("run: git submodule update --init templates");
    if (!QFileInfo(root).isDir() || QDir(root).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot))
        return QStringLiteral("the templates submodule is empty (%1); %2").arg(root, fix);
    if (!QFileInfo(root + "/categories.json").isFile())
        return QStringLiteral("the templates submodule is stale, it has no categories.json (%1); %2").arg(root, fix);
    return {};
}

struct Fixture {
    QTemporaryDir bundled, own;
    Registry registry;
    Fixture() { nodes::addBuiltinNodeTypes(registry); }
    TemplateLibrary library() const {
        return TemplateLibrary({{own.path(), true}, {bundled.path(), false}}, &registry);
    }
};

// A template whose Load Models names one diffusion model by the hash of `bytes`, from `source`.
void makeTemplateNeeding(const QString &folder, const QString &title, const QByteArray &bytes, const QString &source,
                         const QString &name = "m.gguf") {
    const QJsonObject model{{"resource", "fugue/diffusion-model"}, {"hash", sha256(bytes)}, {"name", name}};
    QJsonObject recorded = model;
    recorded.insert("source", source);
    recorded.insert("size", bytes.size());
    makeTemplate(folder, {{"title", title}},
                 workflow({{"models", node("fugue/load-models", {{"diffusion_model", model}})}},
                          {{"models", QJsonArray{recorded}}}));
}

// Under ctest a test's home stays between runs: no model, remembered hash or setting of an
// earlier run.
void forgetDownloads() {
    QDir(dataDir() + "/models").removeRecursively();
    QFile::remove(FileHashes::defaultPath());
    QFile::remove(settingsPath());
}

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

    // 11.10, #118: a template folder may carry the exact pack it needs as packs/<id>/. It is
    // listed as carried (the offer comes from it), and a carried pack that is not what the
    // workflow records, or is not named for its id, is a problem of the bundled templates.
    void aTemplateListsThePacksItCarries() {
        Fixture f;
        const QJsonObject recorded{{"packs", QJsonObject{{"io.example.tools", QJsonObject{{"version", "1.2.0"}, {"source", "https://example.com/tools"}}}}}};
        makeTemplate(f.bundled.filePath("carrier"), {{"title", "Carrier"}},
                     workflow({{"p", node("io.example.tools/sharpen", {}, "1.0.0")}}, recorded));
        writeJson(f.bundled.filePath("carrier/packs/io.example.tools/pack.json"),
                  {{"manifest", 1}, {"id", "io.example.tools"}, {"version", "1.2.0"}});
        makeTemplate(f.bundled.filePath("plain"), {{"title", "Plain"}});
        QVERIFY(f.library().problems().isEmpty());

        QVERIFY(f.library().find("carrier"));
        const Template carrier = *f.library().find("carrier");
        QCOMPARE(carrier.neededPacks, QStringList{"io.example.tools"}); // not installed here
        QCOMPARE(carrier.carriedPacks.size(), 1);
        QCOMPARE(carrier.carriedPacks.first().id, QStringLiteral("io.example.tools"));
        QCOMPARE(carrier.carriedPacks.first().version, QStringLiteral("1.2.0"));
        QCOMPARE(carrier.carriedPacks.first().folder, f.bundled.filePath("carrier/packs/io.example.tools"));
        QVERIFY(f.library().find("plain")->carriedPacks.isEmpty());

        // A version other than the record, a folder not named for its id, a pack.json that is not one.
        writeJson(f.bundled.filePath("carrier/packs/io.example.tools/pack.json"),
                  {{"manifest", 1}, {"id", "io.example.tools"}, {"version", "1.3.0"}});
        writeJson(f.bundled.filePath("carrier/packs/other/pack.json"), {{"manifest", 1}, {"id", "io.example.else"}, {"version", "1.0.0"}});
        writeFile(f.bundled.filePath("carrier/packs/broken/pack.json"), "{ not json");
        QStringList messages;
        for (const Error &error : f.library().problems())
            messages << error.message;
        const QString all = messages.join('\n');
        QVERIFY2(all.contains("records 1.2.0"), qPrintable(all));
        QVERIFY2(all.contains("packs/other must hold the pack other"), qPrintable(all));
        QVERIFY2(all.contains("broken"), qPrintable(all));
        QCOMPARE(messages.size(), 3);
    }

    // The window offers the carried pack from the template's own folder through the banner (never
    // installs it: the Packs page asks), also when another major of the pack is installed beside
    // it; the offer goes once the pack is there. A pack the template does not carry has no offer.
    void theBannerOffersThePackTheTemplateCarries() {
        const auto sharpen = [](const QString &version) {
            return QJsonObject{{"manifest", 1}, {"id", "io.example.tools/sharpen"}, {"version", version}, {"name", "Sharpen"},
                               {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                               {"inputs", QJsonArray()}, {"outputs", QJsonArray()}};
        };
        Fixture f;
        const QJsonObject recorded{{"packs", QJsonObject{{"io.example.tools", QJsonObject{{"version", "1.2.0"},
                                                                                        {"source", "https://example.com/tools"}}}}}};
        makeTemplate(f.bundled.filePath("carrier"), {{"title", "Carrier"}},
                     workflow({{"p", node("io.example.tools/sharpen", {}, "1.0.0")}}, recorded));
        writeJson(f.bundled.filePath("carrier/packs/io.example.tools/pack.json"),
                  {{"manifest", 1}, {"id", "io.example.tools"}, {"version", "1.2.0"}});
        makeTemplate(f.bundled.filePath("bare"), {{"title", "Bare"}},
                     workflow({{"p", node("io.example.tools/sharpen", {}, "1.0.0")}}, recorded));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);

        // Not installed: the offer names the folder the template holds, with no ref.
        QVERIFY(workflows.openTemplateById("carrier"));
        QVariantMap offer = workflows.packOffer();
        QCOMPARE(offer.value("id").toString(), QStringLiteral("io.example.tools"));
        QCOMPARE(offer.value("source").toString(), f.bundled.filePath("carrier/packs/io.example.tools"));
        QCOMPARE(offer.value("ref").toString(), QString());
        QVERIFY2(workflows.notice().contains("carries the pack io.example.tools 1.2.0"), qPrintable(workflows.notice()));
        QSignalSpy install(&workflows, &Workflows::packInstallRequested);
        workflows.installPack();
        QCOMPARE(install.size(), 1);
        QCOMPARE(install.first().at(0).toString(), f.bundled.filePath("carrier/packs/io.example.tools"));
        QCOMPARE(install.first().at(1).toString(), QString());

        // Another major installed beside it does not satisfy the node's recorded major.
        QVERIFY(f.registry.addManifest(sharpen("2.0.0")).isEmpty());
        f.registry.setPackVersion("io.example.tools", "2.0.0");
        workflows.refreshNodeTypes();
        QCOMPARE(workflows.packOffer().value("id").toString(), QStringLiteral("io.example.tools"));

        // A template that carries nothing offers the recorded source, as before (#91).
        QVERIFY(workflows.openTemplateById("bare"));
        QVERIFY(workflows.packOffer().isEmpty()); // installed at another major: needs review, not missing
    }

    void theBannerOffersNothingOnceTheCarriedPackIsInstalled() {
        Fixture f;
        makeTemplate(f.bundled.filePath("carrier"), {{"title", "Carrier"}},
                     workflow({{"p", node("io.example.tools/sharpen", {}, "1.0.0")}}));
        writeJson(f.bundled.filePath("carrier/packs/io.example.tools/pack.json"),
                  {{"manifest", 1}, {"id", "io.example.tools"}, {"version", "1.0.0"}});
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QVERIFY(workflows.openTemplateById("carrier"));
        QVERIFY(!workflows.packOffer().isEmpty());
        QVERIFY(f.registry.addManifest(QJsonObject{{"manifest", 1}, {"id", "io.example.tools/sharpen"}, {"version", "1.0.0"},
                                                   {"name", "Sharpen"},
                                                   {"runtime", QJsonObject{{"kind", "command"}, {"entry", "./run.sh"}}},
                                                   {"inputs", QJsonArray()}, {"outputs", QJsonArray()}}).isEmpty());
        f.registry.setPackVersion("io.example.tools", "1.0.0");
        workflows.refreshNodeTypes();
        QVERIFY(workflows.packOffer().isEmpty());
        QVERIFY2(!workflows.notice().contains("carries"), qPrintable(workflows.notice()));
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

    // #89: the banner offers the opened workflow's missing models, into the model folder the
    // person picks (remembered in the settings); what arrives is found, and the banner says so.
    void theBannerOffersTheMissingModelsAndTheFolderTheyGoIn() {
        forgetDownloads();
        Fixture f;
        const QByteArray bytes = "a diffusion model";
        fugue::test::FakeHttpServer server;
        QVERIFY(server.listen());
        server.on("GET", "/m.gguf", [&](const fugue::test::HttpRequest &) {
            return fugue::test::HttpResponse{200, bytes, "application/octet-stream"};
        });
        makeTemplateNeeding(f.bundled.filePath("gen"), "Gen", bytes, QString::fromLatin1(server.origin() + "/m.gguf"));
        // A ComfyUI models folder beside Fugue's own.
        QTemporaryDir comfy;
        QDir().mkpath(comfy.filePath("diffusion_models"));
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{comfy.path()}).isEmpty());

        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        // English plural forms come from the catalog, as in the app.
        Translator translator(state.filePath("language"));
        QCoreApplication::installTranslator(&translator);
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QCOMPARE(workflows.templates().at(0).toMap().value("download").toString(), QString("Download 1 model, 1 MB"));

        QVERIFY(workflows.openTemplateById("gen"));
        QCOMPARE(workflows.downloadOffer(), QString("Download 1 model, 1 MB"));
        const QString own = dataDir() + "/models";
        QCOMPARE(workflows.downloadRoot(), own);
        QCOMPARE(workflows.downloadRoots().size(), 2);
        QCOMPARE(workflows.downloadRoots().at(0).toMap().value("title").toString(), QString("Fugue's models folder"));
        QVERIFY(workflows.notice().contains(own + "/"));

        // Into the ComfyUI folder: remembered, and the banner names where it goes.
        workflows.setDownloadRoot(comfy.path());
        QCOMPARE(currentSettings().downloadFolder, comfy.path());
        QCOMPARE(workflows.downloadRoot(), comfy.path());
        QVERIFY(workflows.notice().contains(comfy.filePath("diffusion_models")));

        workflows.downloadMissing();
        QVERIFY(workflows.downloads()->busy());
        QVERIFY(workflows.downloadOffer().isEmpty()); // not twice
        QTRY_VERIFY(!workflows.downloads()->busy());
        QCOMPARE(readFile(comfy.filePath("diffusion_models/m.gguf")), bytes);
        QCOMPARE(workflows.notice(), QString("Downloaded 1 model."));
        QVERIFY(workflows.downloadOffer().isEmpty());
        QVERIFY(workflows.templates().at(0).toMap().value("status").toString().isEmpty()); // found by its hash

        // Back to Fugue's own: the default, so no key is left.
        workflows.setDownloadRoot(own);
        QVERIFY(currentSettings().downloadFolder.isEmpty());
        forgetDownloads();
    }

    // #89 in the window: a card's Download fetches its models without opening it; the banner's
    // button does the same for the workflow just opened, with progress and Cancel.
    void aCardAndTheBannerDownloadWithProgressAndCancel() {
        forgetDownloads();
        Fixture f;
        const QByteArray bytes = "a diffusion model", other = "another diffusion model";
        fugue::test::FakeHttpServer server;
        QVERIFY(server.listen());
        bool hold = false;
        server.on("GET", "/m.gguf", [&](const fugue::test::HttpRequest &) {
            return fugue::test::HttpResponse{200, bytes, "application/octet-stream"};
        });
        server.on("GET", "/other.gguf", [&](const fugue::test::HttpRequest &) {
            fugue::test::HttpResponse response{200, other, "application/octet-stream"};
            response.hold = hold;
            return response;
        });
        makeTemplateNeeding(f.bundled.filePath("gen"), "Gen", bytes, QString::fromLatin1(server.origin() + "/m.gguf"));
        makeTemplateNeeding(f.bundled.filePath("more"), "More", other,
                            QString::fromLatin1(server.origin() + "/other.gguf"), "other.gguf");
        // A source that serves another file than the one named.
        makeTemplateNeeding(f.bundled.filePath("wrong"), "Wrong", "the named bytes",
                            QString::fromLatin1(server.origin() + "/m.gguf"), "wrong.gguf");
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
        registerMenuCommands(commands);
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
        // A control made just now is placed at the next polish: click once it is in the window.
        const auto click = [&](QQuickItem *target) {
            QVERIFY(target);
            const auto center = [target] { return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)); };
            QTRY_VERIFY(QRectF(0, 0, window->width(), window->height()).contains(center()));
            QTest::mouseClick(window, Qt::LeftButton, {}, center().toPoint());
        };

        QVERIFY(commands.run("file.templates"));
        QTRY_VERIFY(window->findChild<QObject *>("templatesDialog")->property("opened").toBool());
        QTRY_VERIFY(item(window->contentItem(), "templateDownload-gen"));
        QQuickItem *download = item(window->contentItem(), "templateDownload-gen");
        QTRY_VERIFY(download->isVisible());
        QVERIFY(download->height() >= 24); // the floor
        click(download);
        QTRY_COMPARE(server.requestsTo("GET", "/m.gguf").size(), 1);
        QTRY_VERIFY(!workflows.downloads()->busy());
        // The card says it is ready now; no template was opened.
        QTRY_VERIFY(!item(window->contentItem(), "templateStatus-gen")->isVisible());
        QVERIFY(window->findChild<QObject *>("templatesDialog")->property("visible").toBool());
        QCOMPARE(workflows.count(), 1);
        QCOMPARE(workflows.tabs().at(0).toMap().value("title").toString(), QString("Untitled"));
        // The size is on the button; a failure shows on its own card, not only under the scrim.
        item(window->contentItem(), "templatesSearch")->setProperty("text", "wrong"); // its card in view
        QTRY_VERIFY(!item(window->contentItem(), "templateCard-gen"));
        QTRY_VERIFY(item(window->contentItem(), "templateDownload-wrong"));
        QCOMPARE(item(window->contentItem(), "templateDownload-wrong")->property("text").toString(), QString("Download 1 MB"));
        click(item(window->contentItem(), "templateDownload-wrong"));
        QTRY_VERIFY(!workflows.downloads()->busy());
        // The cards are made again when the list is read again: look each time.
        const auto statusOf = [&](const QString &id) {
            QQuickItem *status = item(window->contentItem(), "templateStatus-" + id);
            return status ? status->property("text").toString() : QString();
        };
        QTRY_VERIFY2(statusOf("wrong").contains("another file than wrong.gguf"),
                     qPrintable(statusOf("wrong")));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!window->findChild<QObject *>("templatesDialog")->property("visible").toBool());

        // The banner of a workflow that lacks one: Download, then progress and Cancel.
        hold = true;
        QVERIFY(workflows.openTemplateById("more"));
        QTRY_VERIFY(item(window->contentItem(), "downloadModels") && item(window->contentItem(), "downloadModels")->isVisible());
        QVERIFY(!item(window->contentItem(), "downloadInto")); // one model folder: no choice
        click(item(window->contentItem(), "downloadModels"));
        QTRY_VERIFY(item(window->contentItem(), "downloadProgress"));
        QTRY_VERIFY(!item(window->contentItem(), "downloadModels"));
        QTRY_COMPARE(server.requestsTo("GET", "/other.gguf").size(), 1);
        click(item(window->contentItem(), "downloadCancel"));
        QTRY_VERIFY(!workflows.downloads()->busy());
        QTRY_VERIFY(!item(window->contentItem(), "downloadProgress"));
        // What it lacks again, with its button: a cancel needs no news of its own.
        QVERIFY(workflows.notice().contains("other.gguf"));
        QTRY_VERIFY(item(window->contentItem(), "downloadModels"));
        QVERIFY(!QFileInfo::exists(ModelLibrary::current(&f.registry).downloadFolder(*PortType::parse("fugue/diffusion-model"))
                                   + "/other.gguf.part"));

        // A cancel from elsewhere with nothing arrived says nothing of its own.
        workflows.dismissNotice();
        workflows.downloadTemplate("more");
        QTRY_VERIFY(workflows.downloads()->busy());
        workflows.downloads()->cancel();
        QTRY_VERIFY(!workflows.downloads()->busy());
        QVERIFY2(!workflows.notice().contains("Downloaded"), qPrintable(workflows.notice()));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join("\n")));
        forgetDownloads();
    }

    // The bundled templates (the templates submodule, fugue-workflows at the pinned commit): this
    // Fugue lists every one with nothing wrong, each compiles and opens with every node known,
    // and each model it names has a source and a size.
    void theBundledTemplatesAreWholeAndCompile() {
        const QString root = QStringLiteral(FUGUE_SOURCE_DIR "/templates");
        QVERIFY2(bundledProblem(root).isEmpty(), qPrintable(bundledProblem(root)));
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
            for (const QJsonValue model : t.models) {
                QVERIFY2(model.toObject().value("source").toString().startsWith("https://"), qPrintable(t.id));
                QVERIFY2(model.toObject().value("size").toDouble() > 0, qPrintable(t.id)); // the Download button's total
            }
            const Document document = Document::loadFile(t.workflow).value();
            const Compilation compiled = compile(document, f.registry);
            QVERIFY2(compiled.ok(), qPrintable(t.id + ": " + compiled.error().message));
            QVERIFY2(!hasErrors(compiled.problems()), qPrintable(t.id));
            // A size is always set, by a node, never left to a model's default (#72).
            if (document.graph.nodes.contains("generate"))
                QCOMPARE(document.graph.nodes.value("generate").inputs.value("width").kind(), InputValue::Kind::Link);
        }
    }

    // The guard for the submodule pointer: the templates Fugue ships with are the ones it expects
    // (the video and upscale ones came with a later pointer; a checkout left on an older one, or
    // an empty folder, must fail here with the fix, not later with a puzzling count).
    void theBundledTemplatesAreTheOnesFugueExpects() {
        const QString root = QStringLiteral(FUGUE_SOURCE_DIR "/templates");
        QVERIFY2(bundledProblem(root).isEmpty(), qPrintable(bundledProblem(root)));
        const QString fix = QStringLiteral("; the templates submodule is stale, run: git submodule update --init templates");

        const QJsonArray listedCategories = QJsonDocument::fromJson(readFile(root + "/categories.json"))
                                                .object().value("categories").toArray();
        QStringList categories;
        for (const QJsonValue category : listedCategories)
            categories << category.toObject().value("id").toString();
        for (const char *wanted : {"video", "upscale"})
            QVERIFY2(categories.contains(QLatin1String(wanted)),
                     qPrintable(QStringLiteral("categories.json lacks the category %1").arg(wanted) + fix));
        for (const QJsonValue category : listedCategories)
            QVERIFY2(!category.toObject().value("title").toString().isEmpty(),
                     qPrintable(category.toObject().value("id").toString() + " has no title"));

        Fixture f;
        const TemplateLibrary library({{root, false}}, &f.registry);
        QStringList found;
        for (const Template &t : library.list()) {
            found << t.id;
            // Every category a template uses is one categories.json names.
            QVERIFY2(categories.contains(t.category),
                     qPrintable(QStringLiteral("%1 is in the category \"%2\", which categories.json lacks").arg(t.id, t.category)));
        }
        for (const char *wanted : {"text-to-video", "image-to-video", "image-chain-to-video", "image-upscale", "video-upscale"})
            QVERIFY2(found.contains(QLatin1String(wanted)),
                     qPrintable(QStringLiteral("the template %1 is not here").arg(wanted) + fix));
        // The two categories hold what they are for.
        QCOMPARE(library.find("text-to-video")->category, QStringLiteral("video"));
        QCOMPARE(library.find("image-upscale")->category, QStringLiteral("upscale"));
        QCOMPARE(library.find("video-upscale")->category, QStringLiteral("upscale"));
        QCOMPARE(library.find("image-upscale")->categoryTitle, QStringLiteral("Upscaling"));
        QCOMPARE(library.find("text-to-video")->categoryTitle, QStringLiteral("Video"));
    }
};

QTEST_MAIN(TestTemplates)
#include "test_templates.moc"
