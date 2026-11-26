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
#include <QImage>
#include <QJsonObject>
#include <QPainter>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>

#include "apps/app_ref.h"
#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/model_library.h"
#include "engine/model_offers.h"
#include "engine/pack_loader.h"
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
#include "../window_grab.h"

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

// The title of the workflow file's app block, as written.
QString blockTitle(const QString &path) {
    return QJsonDocument::fromJson(readFile(path)).object().value("app").toObject().value("title").toString();
}

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

// Scrolls the templates grid so that `target` (a card or something on it) is inside the grid's view: the header of the kit
// window leaves room for fewer rows, and a click outside the dialog's clip would miss.
void reveal(QQuickItem *window, QQuickItem *target) {
    QQuickItem *grid = item(window, "templatesGrid");
    QVERIFY(grid && target);
    const QRectF at = target->mapRectToItem(grid, QRectF(QPointF(), target->size()));
    const double over = at.bottom() - grid->height();
    if (over > 0)
        grid->setProperty("contentY", grid->property("contentY").toDouble() + over + 8);
    else if (at.top() < 0)
        grid->setProperty("contentY", grid->property("contentY").toDouble() + at.top() - 8);
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


// A template that needs the diffusion model `bytes` and the text encoder `encoder` (#164).
void makeTemplateNeedingTwo(const QString &folder, const QString &title, const QByteArray &diffusion,
                            const QByteArray &encoder, const QString &diffusionName, const QString &encoderName) {
    const QJsonObject first{{"resource", "fugue/diffusion-model"}, {"hash", sha256(diffusion)}, {"name", diffusionName}};
    const QJsonObject second{{"resource", "fugue/text-encoder"}, {"hash", sha256(encoder)}, {"name", encoderName}};
    makeTemplate(folder, {{"title", title}},
                 workflow({{"models", node("fugue/load-models", {{"diffusion_model", first}, {"text_encoder", second}})}}));
}

// What a paired host serves: the model of `bytes`, by its hash.
ListedModel servedBy(const QString &host, const QString &resource, const QByteArray &bytes, const QString &name,
                     const QString &status = "ready") {
    ListedModel model;
    model.name = name;
    model.handle = QJsonObject{{"resource", resource}, {"hash", sha256(bytes)}, {"name", name}};
    model.status = status;
    model.provider = host;
    model.host = host;
    return model;
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
    void readsTheSourceAndTheFamiliesOfTheModelPickers() {
        Fixture f;
        const QJsonObject klein{{"resource", "fugue/diffusion-model"}, {"hash", "sha256:" + QString(64, 'a')}, {"name", "flux-2-klein-9b-Q8_0.gguf"}};
        const QJsonObject vae{{"resource", "fugue/vae"}, {"hash", "sha256:" + QString(64, 'b')}, {"name", "flux2-vae.safetensors"}};
        makeTemplate(f.bundled.filePath("from-da"),
                     {{"title", "From DA"},
                      {"source", QJsonObject{{"name", "DA flux2"}, {"author", "DemonAlone"},
                                             {"url", "https://example.org/da"}, {"note", "ported"}}}},
                     workflow({{"models", node("fugue/load-models", {{"diffusion_model", klein}, {"vae", vae}})}}));
        // A bad url and a field that is not text are left out and warned; the template stays listed.
        makeTemplate(f.bundled.filePath("bad"),
                     {{"title", "Bad"}, {"source", QJsonObject{{"name", "X"}, {"url", "javascript:alert(1)"}, {"note", 3}}}});
        makeTemplate(f.bundled.filePath("plain"), {{"title", "Plain"}});

        const TemplateLibrary library = f.library();
        const std::optional<Template> da = library.find("from-da");
        QCOMPARE(da->source.value("author").toString(), QStringLiteral("DemonAlone"));
        QCOMPARE(da->source.value("url").toString(), QStringLiteral("https://example.org/da"));
        QCOMPARE(da->families, QStringList{"FLUX.2 Klein 9B"}); // the VAE is no family
        const std::optional<Template> bad = library.find("bad");
        QCOMPARE(bad->source, (QJsonObject{{"name", "X"}}));
        QCOMPARE(library.problems().size(), 2);
        QVERIFY(library.find("plain")->source.isEmpty());
        QVERIFY(library.find("plain")->families.isEmpty());
    }

    // template.json's `app` and the workflow's block (#387, 11.10): the block decides what is an app, template.json gives the card's
    // words and may say `false`; what does not agree is a problem of the catalog check and never keeps a template out of the list.
    void readsTheAppFlagAgainstTheWorkflowsBlock() {
        Fixture f;
        const QJsonObject block{{"format", 1}, {"title", "Block title"}, {"description", "Block words"},
                                {"inputs", QJsonArray{QJsonObject{{"role", "prompt"}, {"targets", QJsonArray{"prompt.text"}}}}},
                                {"outputs", QJsonArray{QJsonObject{{"node", "prompt"}}}}};
        QJsonObject app = workflow({{"prompt", node("fugue/text", {{"text", "a fox"}})}});
        app.insert("app", block);
        QJsonObject broken = app;
        broken.insert("app", QJsonObject{{"format", 1}, {"inputs", "nope"}});
        const QJsonObject card{{"title", "Card title"}, {"summary", "Card words"}, {"kind", "image"}};

        makeTemplate(f.bundled.filePath("plain"), {{"title", "Plain"}});
        makeTemplate(f.bundled.filePath("yes"), {{"title", "Yes"}, {"description", "Own words"}, {"app", true}}, app);
        makeTemplate(f.bundled.filePath("card"), {{"title", "Card"}, {"app", card}}, app);
        makeTemplate(f.bundled.filePath("off"), {{"title", "Off"}, {"app", false}}, app);
        writeFile(f.own.filePath("loose.fugue"), QJsonDocument(app).toJson()); // a plain file has no template.json to agree with
        const TemplateLibrary library = f.library();
        QVERIFY(library.problems().isEmpty());

        QVERIFY(!library.find("plain")->app);
        QVERIFY(!library.find("off")->app); // false keeps it out of the Apps; its block still runs by name
        const TemplateApp yes = *library.find("yes")->app;
        QCOMPARE(yes.title, QStringLiteral("Block title")); // the block's, then the template's own
        QCOMPARE(yes.summary, QStringLiteral("Block words"));
        QCOMPARE(yes.kind, QString());
        const TemplateApp named = *library.find("card")->app;
        QCOMPARE(named.title, QStringLiteral("Card title"));
        QCOMPARE(named.summary, QStringLiteral("Card words"));
        QCOMPARE(named.kind, QStringLiteral("image"));
        QVERIFY(library.find("loose")->app);

        // What does not agree is said, and the template stays listed.
        makeTemplate(f.bundled.filePath("ghost"), {{"title", "Ghost"}, {"app", true}});
        makeTemplate(f.bundled.filePath("unflagged"), {{"title", "Unflagged"}}, app);
        makeTemplate(f.bundled.filePath("unreadable"), {{"title", "Unreadable"}, {"app", true}}, broken);
        makeTemplate(f.bundled.filePath("odd"), {{"title", "Odd"}, {"app", QJsonObject{{"kind", 3}, {"colour", "red"}}}}, app);
        makeTemplate(f.bundled.filePath("text"), {{"title", "Text"}, {"app", "yes"}}, app);
        QStringList said;
        for (const Error &problem : library.problems())
            said << problem.message;
        const auto about = [&](const char *id) {
            QStringList mine;
            for (const QString &line : said)
                if (line.startsWith(QStringLiteral("Template %1:").arg(QLatin1String(id))))
                    mine << line;
            return mine;
        };
        QVERIFY(!library.find("ghost")->app);
        QCOMPARE(about("ghost").size(), 1);
        QVERIFY(about("ghost").first().contains("no app block"));
        QVERIFY(library.find("unflagged")->app); // lenient to run, strict to check
        QCOMPARE(about("unflagged").size(), 1);
        QVERIFY(!library.find("unreadable")->app);
        QCOMPARE(about("unreadable").size(), 1);
        QVERIFY(about("unreadable").first().contains("does not read"));
        QCOMPARE(about("odd").size(), 2); // a field that is not text, an unknown field
        QCOMPARE(about("text").size(), 1);
        QVERIFY(library.find("odd")->app && library.find("text")->app);
        QCOMPARE(said.size(), 6);
    }

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

        // A card's Download for a template that needs a pack opens it, and the Get ready sheet plans the pack
        // (a direct download refuses a model whose type the pack declares).
        QSignalSpy sheet(workflows.readiness(), &ReadinessSheet::shown);
        const int tabs = workflows.count();
        QVERIFY(workflows.downloadTemplate("carrier"));
        QCOMPARE(workflows.count(), tabs + 1);
        QCOMPARE(sheet.size(), 1);

        // Another major installed beside it does not satisfy the node's recorded major.
        QVERIFY(f.registry.addManifest(sharpen("2.0.0")).isEmpty());
        f.registry.setPackVersion("io.example.tools", "2.0.0");
        workflows.refreshNodeTypes();
        QCOMPARE(workflows.packOffer().value("id").toString(), QStringLiteral("io.example.tools"));

        // A template that carries nothing offers the recorded source, as before (#91).
        QVERIFY(workflows.openTemplateById("bare"));
        QVERIFY(workflows.packOffer().isEmpty()); // installed at another major: needs review, not missing
    }

    // #264: a missing pack is named in words (the name the workflow records, else the id's last part) and
    // installed from where it says, with or without the banner.
    void aMissingPackIsNamedInWordsAndInstallable() {
        QCOMPARE(packTitle("io.github.bmaltais.fugue-remove-background"), QStringLiteral("Remove Background"));
        QCOMPARE(packTitle("io.example.tools"), QStringLiteral("Tools"));
        QCOMPARE(packTitle("io.example.tools", " Sharp Tools "), QStringLiteral("Sharp Tools"));

        Fixture f;
        makeTemplate(f.bundled.filePath("many"), {{"title", "Many"}},
                     workflow({{"a", node("io.example.tools/sharpen", {}, "1.0.0")},
                               {"b", node("io.example.tools/sharpen", {}, "1.0.0")},
                               {"c", node("io.example.other-pack/shrink", {}, "1.0.0")}},
                              QJsonObject{{"packs", QJsonObject{{"io.example.tools", QJsonObject{{"version", "1.0.0"},
                                                                                              {"source", "https://example.com/tools"},
                                                                                              {"name", "Sharp Tools"}}}}}}));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QVERIFY(workflows.openTemplateById("many"));
        const QVariantMap need = workflows.packNeed("io.example.tools");
        QCOMPARE(need.value("text").toString(), QStringLiteral("Needs the Sharp Tools pack"));
        QVERIFY(need.value("installable").toBool());
        QVERIFY(!workflows.packNeed("io.example.other-pack").value("installable").toBool()); // no source recorded
        QSignalSpy install(&workflows, &Workflows::packInstallRequested);
        QVERIFY(!workflows.installPackById("io.example.other-pack"));
        QVERIFY(workflows.installPackById("io.example.tools"));
        QCOMPARE(install.size(), 1);
        QCOMPARE(install.first().at(0).toString(), QStringLiteral("https://example.com/tools"));
        workflows.dismissNotice();
        QVERIFY(workflows.packNeed("io.example.tools").value("installable").toBool()); // the card keeps it
    }

    // #266: the banner names each missing pack once, by display name, and offers each one.
    void theBannerNamesAndOffersEachMissingPackOnce() {
        Fixture f;
        const auto entry = [](const char *source, const char *name) {
            return QJsonObject{{"version", "1.0.0"}, {"source", source}, {"name", name}};
        };
        makeTemplate(f.bundled.filePath("two"), {{"title", "Two"}},
                     workflow({{"a", node("io.example.tools/sharpen", {}, "1.0.0")},
                               {"b", node("io.example.tools/blur", {}, "1.0.0")},
                               {"c", node("io.example.other-pack/shrink", {}, "1.0.0")}},
                              QJsonObject{{"packs", QJsonObject{{"io.example.tools", entry("https://example.com/tools", "Sharp Tools")},
                                                                {"io.example.other-pack", entry("https://example.com/other", "Shrinker")}}}}));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QVERIFY(workflows.openTemplateById("two"));
        const QString notice = workflows.notice();
        QVERIFY2(notice.contains("(Sharp Tools, Shrinker)") || notice.contains("(Shrinker, Sharp Tools)"), qPrintable(notice));
        QCOMPARE(notice.count("Sharp Tools"), 1);
        QVERIFY(!notice.contains("io.example"));
        const QVariantList offers = workflows.packOffers();
        QCOMPARE(offers.size(), 2);
        QSet<QString> texts;
        for (const QVariant &offer : offers)
            texts << offer.toMap().value("text").toString();
        QCOMPARE(texts, (QSet<QString>{"Install Sharp Tools…", "Install Shrinker…"}));
        QSignalSpy install(&workflows, &Workflows::packInstallRequested);
        QVERIFY(workflows.installPackById("io.example.other-pack"));
        QCOMPARE(install.first().at(0).toString(), QStringLiteral("https://example.com/other"));
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

    // The preview of a folder (11.10): template.json's `preview` may name a WebM, `poster` its still; without the keys the stills are
    // found by name and a preview.webm beside them is the moving one; a name that leaves the folder is ignored.
    void aTemplateMayHaveAMovingPreviewWithAPoster() {
        Fixture f;
        makeTemplate(f.bundled.filePath("still"), {{"title", "Still"}});
        writeFile(f.bundled.filePath("still/preview.webp"), "webp");
        makeTemplate(f.bundled.filePath("moving"), {{"title", "Moving"}, {"preview", "preview.webm"}, {"poster", "poster.png"}});
        writeFile(f.bundled.filePath("moving/preview.webm"), "webm");
        writeFile(f.bundled.filePath("moving/poster.png"), png());
        writeFile(f.bundled.filePath("moving/preview.webp"), "webp");
        makeTemplate(f.bundled.filePath("noposter"), {{"title", "No poster"}, {"preview", "preview.webm"}});
        writeFile(f.bundled.filePath("noposter/preview.webm"), "webm");
        writeFile(f.bundled.filePath("noposter/preview.webp"), "webp"); // the still fallback
        makeTemplate(f.bundled.filePath("bymame"), {{"title", "By name"}});
        writeFile(f.bundled.filePath("bymame/preview.webm"), "webm");
        writeFile(f.bundled.filePath("bymame/poster.png"), png());
        makeTemplate(f.bundled.filePath("outside"), {{"title", "Outside"}, {"preview", "../still/preview.webp"}, {"poster", "../still/preview.webp"}});
        const TemplateLibrary library = f.library();
        QVERIFY(library.problems().isEmpty());

        const Template still = *library.find("still");
        QVERIFY(still.preview.endsWith("still/preview.webp"));
        QVERIFY(still.video.isEmpty());
        const Template moving = *library.find("moving");
        QVERIFY(moving.video.endsWith("moving/preview.webm"));
        QVERIFY2(moving.preview.endsWith("moving/poster.png"), qPrintable(moving.preview)); // the poster, not the webp
        const Template noposter = *library.find("noposter");
        QVERIFY(noposter.video.endsWith("noposter/preview.webm"));
        QVERIFY(noposter.preview.endsWith("noposter/preview.webp"));
        const Template byName = *library.find("bymame");
        QVERIFY(byName.video.endsWith("bymame/preview.webm"));
        QVERIFY(byName.preview.endsWith("bymame/poster.png"));
        const Template outside = *library.find("outside");
        QVERIFY(outside.video.isEmpty() && outside.preview.isEmpty());
    }

    // Duplicate (#470): a whole copy as the person's own under a new id, titled as asked, the original untouched; Rename only for the
    // person's own.
    void duplicateAndRenameKeepTheOriginalAndOnlyRenameTheOwn() {
        Fixture f;
        QJsonObject app = workflow({{"prompt", node("fugue/text", {{"text", "a fox"}})}});
        app.insert("app", QJsonObject{{"format", 1}, {"title", "Fox"}, {"inputs", QJsonArray{QJsonObject{{"role", "prompt"}, {"targets", QJsonArray{"prompt.text"}}}}},
                                      {"outputs", QJsonArray{QJsonObject{{"node", "prompt"}}}}});
        makeTemplate(f.bundled.filePath("fox"), {{"title", "Fox"}, {"app", true}}, app);
        writeFile(f.bundled.filePath("fox/preview.webm"), "webm");
        writeFile(f.bundled.filePath("fox/inputs/cat.png"), png());
        const TemplateLibrary library = f.library();
        const Template fox = *library.find("fox");

        QString copy;
        QVERIFY(library.duplicate(fox, "Fox copy", &copy).isEmpty());
        QCOMPARE(copy, QString("fox-copy"));
        QString second;
        QVERIFY(library.duplicate(fox, "Fox copy", &second).isEmpty());
        QCOMPARE(second, QString("fox-copy-2"));
        QCOMPARE(library.duplicate(fox, "  ", &second).value(0).code, QString("usage")); // a title is needed
        const Template made = *library.find("fox-copy");
        QVERIFY(made.own && !made.inherited);
        QCOMPARE(made.folder, f.own.filePath("fox-copy"));
        QCOMPARE(made.app->title, QString("Fox copy"));
        QCOMPARE(blockTitle(made.workflow), QString("Fox copy")); // the App view says it too
        QVERIFY(QFileInfo(f.own.filePath("fox-copy/preview.webm")).isFile());
        QVERIFY(QFileInfo(f.own.filePath("fox-copy/inputs/cat.png")).isFile());
        QCOMPARE(library.find("fox")->app->title, QString("Fox")); // the original is untouched
        QCOMPARE(blockTitle(fox.workflow), QString("Fox"));

        QVERIFY(library.rename(made, "Silver fox").isEmpty());
        QCOMPARE(library.find("fox-copy")->app->title, QString("Silver fox"));
        QCOMPARE(library.find("fox-copy")->title, QString("Silver fox"));
        QCOMPARE(blockTitle(made.workflow), QString("Silver fox"));
        QCOMPARE(library.rename(fox, "Mine now").value(0).code, QString("usage")); // a bundled one is not the person's
        QCOMPARE(library.find("fox")->app->title, QString("Fox"));
        QCOMPARE(library.rename(made, " ").value(0).code, QString("usage"));
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
                     workflow({{"load", node("fugue/load-image", {{"file", "inputs/cat.png"}})},
                               {"grade", node("fugue/apply-lut", {{"file", "inputs/look.cube"}})}}));
        writeFile(f.bundled.filePath("edit/inputs/cat.png"), png());
        const QByteArray cube("LUT_1D_SIZE 2\n0 0 0\n1 1 1\n");
        writeFile(f.bundled.filePath("edit/inputs/look.cube"), cube);
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
        // The details view's rows: each model with its type and where it is, the sample inputs as file URLs.
        const QVariantMap row = gen.value("modelList").toList().at(0).toMap();
        QCOMPARE(row.value("name").toString(), QString("m.gguf"));
        QCOMPARE(row.value("kind").toString(), QString("Diffusion model"));
        QCOMPARE(row.value("status").toString(), QString("missing"));
        QVERIFY(gen.value("inputs").toList().isEmpty());
        QCOMPARE(listed.at(1).toMap().value("inputs").toList().size(), 1);
        QVERIFY(listed.at(1).toMap().value("inputs").toList().at(0).toString().endsWith("/inputs/cat.png"));

        QVERIFY(workflows.openTemplateById("edit"));
        QCOMPARE(workflows.count(), 1); // the empty Untitled it started with is replaced
        QCOMPARE(workflows.tabs().at(0).toMap().value("title").toString(), QString("Klein edit"));
        QVERIFY(workflows.pathOf(0).isEmpty());
        QVERIFY(workflows.unsaved());
        const QString file = canvas.document().graph.nodes.value("load").inputs.value("file").literal().toString();
        QVERIFY2(file.startsWith(inputsDir() + "/"), qPrintable(file));
        QCOMPARE(readFile(file), png());
        // A file that is not a picture (a LUT, a song, a clip) is taken in too: the template's folder may move (an AppImage's mount).
        const QString lut = canvas.document().graph.nodes.value("grade").inputs.value("file").literal().toString();
        QVERIFY2(lut.startsWith(inputsDir() + "/") && lut.endsWith(".cube"), qPrintable(lut));
        QCOMPARE(readFile(lut), cube);
    }

    // File > Browse templates opens the dialog; a card shows what the template lacks, a category
    // narrows the cards, and a click opens the template and closes the dialog.
    void theDialogListsTheCardsAndAClickOpensOne() {
        Fixture f;
        writeJson(f.bundled.filePath("categories.json"),
                  {{"categories", QJsonArray{QJsonObject{{"id", "image"}, {"title", "Image generation"}}}}});
        const QString hash = QStringLiteral("sha256:") + QString(64, u'7');
        makeTemplate(f.bundled.filePath("gen"), {{"title", "Gen"}, {"category", "image"}, {"description", "Makes one."},
                                {"source", QJsonObject{{"name", "Ann's flow"}, {"author", "Ann"}, {"url", "https://example.org/a"}}}},
                     workflow({{"models", node("fugue/load-models", {{"diffusion_model",
                                QJsonObject{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", "z-image-turbo.gguf"}}}})}}));
        makeTemplate(f.bundled.filePath("talk"), {{"title", "Talk"}, {"category", "text"}});
        QJsonObject appFlow = workflow({{"prompt", node("fugue/text", {{"text", "a fox"}})}});
        appFlow.insert("app", QJsonObject{{"format", 1}, {"title", "Pic block"}, {"inputs", QJsonArray{QJsonObject{{"role", "prompt"}, {"targets", QJsonArray{"prompt.text"}}}}},
                                          {"outputs", QJsonArray{QJsonObject{{"node", "prompt"}}}}});
        makeTemplate(f.bundled.filePath("pic"), {{"title", "Pic"}, {"category", "text"}, {"app", QJsonObject{{"title", "Picture app"}}}}, appFlow);
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

        // The source line and the two drop-downs (#369): counts say what a choice leaves; a choice narrows the cards.
        // (The card no longer carries the source line: it is on the details, so every card has the same height.)
        QVERIFY(!item(window->contentItem(), "templateSource-gen"));
        QObject *sourceDialog = window->findChild<QObject *>("templatesDialog");
        QVERIFY(QMetaObject::invokeMethod(sourceDialog, "showDetails", Q_ARG(QVariant, "gen"), Q_ARG(QVariant, QVariant())));
        QTRY_COMPARE(item(window->contentItem(), "templateDetailsSource")->property("text").toString(), QString("Based on Ann's flow by Ann"));
        QVERIFY(QMetaObject::invokeMethod(sourceDialog, "showDetails", Q_ARG(QVariant, "talk"), Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(!item(window->contentItem(), "templateDetailsSource")->isVisible());
        QVERIFY(QMetaObject::invokeMethod(sourceDialog, "closeDetails"));
        QObject *dialog = window->findChild<QObject *>("templatesDialog");
        const auto labels = [&](const char *property) {
            QStringList out;
            for (const QVariant &option : dialog->property(property).toList())
                out << option.toMap().value("text").toString();
            return out;
        };
        QCOMPARE(labels("sourceOptions"), (QStringList{"All sources (3)", "Ann's flow (1)"}));
        QCOMPARE(labels("familyOptions"), (QStringList{"All model families (3)", "Z-Image (1)"}));
        QVERIFY(dialog->setProperty("family", "Z-Image"));
        QTRY_VERIFY(!item(window->contentItem(), "templateCard-talk"));
        QVERIFY(item(window->contentItem(), "templateCard-gen"));
        QVERIFY(dialog->setProperty("family", ""));
        QTRY_VERIFY(item(window->contentItem(), "templateCard-talk"));

        // The family drop-down's list is searched by word start: "ima" begins Z-Image's second word, "mage" is inside it.
        QQuickItem *familyBox = item(window->contentItem(), "templatesFamily");
        QVERIFY(familyBox);
        QTest::mouseClick(window, Qt::LeftButton, {}, familyBox->mapToScene(QPointF(familyBox->width() / 2, familyBox->height() / 2)).toPoint());
        QObject *menu = nullptr;
        QTRY_VERIFY((menu = familyBox->findChild<QObject *>("widgetMenu")) && menu->property("opened").toBool());
        const auto shownTexts = [&] {
            QStringList out;
            for (const QVariant &entry : menu->property("shown").toList())
                out << entry.toMap().value("text").toString();
            return out;
        };
        QCOMPARE(shownTexts().size(), 2);
        for (const char c : QByteArray("mage"))
            QTest::keyClick(window, c);
        QTRY_COMPARE(shownTexts(), (QStringList{"Nothing matches “mage”"}));
        QTest::keyClick(window, Qt::Key_Escape); // empties the field first
        QTRY_COMPARE(shownTexts().size(), 2);
        for (const char c : QByteArray("ima"))
            QTest::keyClick(window, c);
        QTRY_COMPARE(shownTexts(), (QStringList{"Z-Image (1)"}));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(dialog->property("family").toString(), QString("Z-Image"));
        QVERIFY(dialog->setProperty("family", ""));
        QTRY_VERIFY(item(window->contentItem(), "templateCard-talk"));

        // The Apps chip (#387) narrows to the apps, which are called by the app's own title; a second tap lifts it.
        QQuickItem *appsChip = item(window->contentItem(), "templatesApps");
        QVERIFY(appsChip && appsChip->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, {}, appsChip->mapToScene(QPointF(appsChip->width() / 2, appsChip->height() / 2)).toPoint());
        QTRY_VERIFY(dialog->property("appsOnly").toBool());
        QTRY_VERIFY(!item(window->contentItem(), "templateCard-talk"));
        QVERIFY(item(window->contentItem(), "templateCard-pic"));
        for (const QVariant &entry : workflows.templates())
            if (entry.toMap().value("id") == "pic")
                QCOMPARE(entry.toMap().value("title").toString(), QStringLiteral("Picture app"));
        QTest::mouseClick(window, Qt::LeftButton, {}, appsChip->mapToScene(QPointF(appsChip->width() / 2, appsChip->height() / 2)).toPoint());
        QTRY_VERIFY(!dialog->property("appsOnly").toBool());
        QTRY_VERIFY(item(window->contentItem(), "templateCard-talk"));

        QQuickItem *card = item(window->contentItem(), "templateCard-talk");
        // The info button, and I on the focused card, show the details and open nothing; Escape closes them first (the dialog
        // stays) and the card has the focus again; Cancel too.
        const auto detailsShown = [&] {
            QQuickItem *d = item(window->contentItem(), "templateDetails");
            return d && d->isVisible();
        };
        QQuickItem *info = item(window->contentItem(), "templateInfo-talk");
        QVERIFY(info);
        reveal(window->contentItem(), info);
        // The card is laid out at the next polish: the chip is at the picture's bottom corner once the picture is 16:10.
        QTRY_VERIFY(info->parentItem()->height() > 0 && qAbs(info->parentItem()->height() - info->parentItem()->width() * 10 / 16) < 1);
        QTest::mouseClick(window, Qt::LeftButton, {}, info->mapToScene(QPointF(info->width() / 2, info->height() / 2)).toPoint());
        QTRY_VERIFY(detailsShown());
        QCOMPARE(item(window->contentItem(), "templateDetailsTitle")->property("text").toString(), QString("Talk"));
        QVERIFY(window->findChild<QObject *>("templatesDialog")->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!detailsShown());
        QVERIFY(window->findChild<QObject *>("templatesDialog")->property("visible").toBool());
        QTRY_VERIFY(item(window->contentItem(), "templatesGrid")->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_I);
        QTRY_VERIFY(detailsShown());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!detailsShown());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(detailsShown());
        // Open, from the details, opens it.
        QQuickItem *openButton = item(window->contentItem(), "templateDetailsOpen");
        QTest::mouseClick(window, Qt::LeftButton, {}, openButton->mapToScene(QPointF(openButton->width() / 2, openButton->height() / 2)).toPoint());
        QTRY_COMPARE(workflows.tabs().at(workflows.current()).toMap().value("title").toString(), QString("Talk"));
        QTRY_VERIFY(!window->findChild<QObject *>("templatesDialog")->property("visible").toBool());
        // A click on a card opens it at once.
        QVERIFY(commands.run("file.templates"));
        QTRY_VERIFY(window->findChild<QObject *>("templatesDialog")->property("opened").toBool());
        card = item(window->contentItem(), "templateCard-gen");
        reveal(window->contentItem(), card);
        const QPoint genCenter = card->mapToScene(QPointF(card->width() / 2, card->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, {}, genCenter);
        QTRY_COMPARE(workflows.tabs().at(workflows.current()).toMap().value("title").toString(), QString("Gen"));
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

    // #379: the Get ready sheet says where models go, a picked folder joins the model folders (ComfyUI layout) and becomes
    // the default, and the one yes downloads into it.
    void theSheetSaysWhereModelsGoAndAPickedFolderBecomesTheDefault() {
        forgetDownloads();
        Fixture f;
        const QByteArray bytes = "a diffusion model";
        fugue::test::FakeHttpServer server;
        QVERIFY(server.listen());
        server.on("GET", "/m.gguf", [&](const fugue::test::HttpRequest &) {
            return fugue::test::HttpResponse{200, bytes, "application/octet-stream"};
        });
        makeTemplateNeeding(f.bundled.filePath("gen"), "Gen", bytes, QString::fromLatin1(server.origin() + "/m.gguf"));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state, picked;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QVERIFY(workflows.openTemplateById("gen"));
        QVERIFY(workflows.getReadyOffer());
        ReadinessSheet *sheet = workflows.readiness();
        sheet->show();
        QVERIFY(sheet->isOpen());
        QCOMPARE(sheet->view().value("destination").toString(), dataDir() + "/models");
        QCOMPARE(sheet->view().value("models").toList().size(), 1);
        QCOMPARE(sheet->view().value("destinations").toList().size(), 1);
        QVERIFY(!QFileInfo::exists(dataDir() + "/models/diffusion-model/m.gguf")); // nothing before the yes

        sheet->pickDestination(picked.path());
        // Chosen on the sheet, but nothing is written before the yes.
        QVERIFY(currentSettings().downloadFolder.isEmpty());
        QVERIFY(!currentSettings().modelPaths.contains(picked.path()));
        QCOMPARE(sheet->view().value("destination").toString(), QFileInfo(picked.path()).canonicalFilePath());
        QCOMPARE(sheet->view().value("destinations").toList().size(), 2);

        sheet->start();
        QTRY_COMPARE(sheet->phase(), QString("finished"));
        QCOMPARE(currentSettings().downloadFolder, QFileInfo(picked.path()).canonicalFilePath());
        QVERIFY(currentSettings().modelPaths.contains(QFileInfo(picked.path()).canonicalFilePath()));
        QCOMPARE(currentSettings().modelFolderKinds.value(QFileInfo(picked.path()).canonicalFilePath()), ModelLibrary::kLayout);
        QVERIFY2(sheet->view().value("ok").toBool(), qPrintable(sheet->view().value("message").toString()));
        QCOMPARE(readFile(picked.filePath("diffusion-model/m.gguf")), bytes);
        QVERIFY(sheet->waitIdle());
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
        server.on("GET", "/last.gguf", [&](const fugue::test::HttpRequest &) {
            return fugue::test::HttpResponse{200, "the last model", "application/octet-stream"};
        });
        makeTemplateNeeding(f.bundled.filePath("last"), "Last", "the last model", QString::fromLatin1(server.origin() + "/last.gguf"), "last.gguf");
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

        // The banner of a workflow that lacks one: one Get ready button (#379), the sheet with where models go and what
        // they need, then progress and Stop. The banner has no "Into" choice and no Download of its own any more.
        hold = true;
        QVERIFY(workflows.openTemplateById("more"));
        QTRY_VERIFY(item(window->contentItem(), "getReady") && item(window->contentItem(), "getReady")->isVisible());
        QVERIFY(!item(window->contentItem(), "downloadModels"));
        QVERIFY(!item(window->contentItem(), "downloadInto"));
        // The sheet opened by itself, and did nothing: Cancel closes it, and the banner's button opens it again.
        QTRY_VERIFY(window->findChild<QObject *>("getReadySheet") && window->findChild<QObject *>("getReadySheet")->property("visible").toBool());
        QCOMPARE(server.requestsTo("GET", "/other.gguf").size(), 0);
        click(item(window->contentItem(), "getReadyCancel"));
        QTRY_VERIFY(!window->findChild<QObject *>("getReadySheet")->property("visible").toBool());
        QCOMPARE(server.requestsTo("GET", "/other.gguf").size(), 0);
        click(item(window->contentItem(), "getReady"));
        QTRY_VERIFY(window->findChild<QObject *>("getReadySheet")->property("visible").toBool());
        QTRY_VERIFY(item(window->contentItem(), "getReadyDestination") && item(window->contentItem(), "getReadyDestination")->isVisible());
        QVERIFY(workflows.readiness()->view().value("destinationTitle").toString().contains("models folder"));
        QCOMPARE(server.requestsTo("GET", "/other.gguf").size(), 0); // nothing before the yes
        click(item(window->contentItem(), "getReadyAll"));
        QTRY_COMPARE(server.requestsTo("GET", "/other.gguf").size(), 1);
        QCOMPARE(workflows.readiness()->phase(), QString("running"));
        click(item(window->contentItem(), "getReadyCancel"));
        QTRY_COMPARE(workflows.readiness()->phase(), QString("finished"));
        QVERIFY(!workflows.readiness()->view().value("ok").toBool());
        QTRY_VERIFY(item(window->contentItem(), "getReadyClose")->isVisible());
        QMetaObject::invokeMethod(item(window->contentItem(), "getReadyClose"), "tapped");
        QTRY_VERIFY(!window->findChild<QObject *>("getReadySheet")->property("visible").toBool());
        QTRY_VERIFY(!workflows.downloads()->busy());
        // What it lacks again, with its button: a cancel needs no news of its own.
        QVERIFY(workflows.notice().contains("other.gguf"));
        QTRY_VERIFY(item(window->contentItem(), "getReady") && item(window->contentItem(), "getReady")->isVisible());
        QVERIFY(!QFileInfo::exists(ModelLibrary::current(&f.registry).downloadFolder(*PortType::parse("fugue/diffusion-model"))
                                   + "/other.gguf.part"));

        // A cancel from elsewhere with nothing arrived says nothing of its own.
        workflows.dismissNotice();
        workflows.downloadTemplate("more");
        QTRY_VERIFY(workflows.downloads()->busy());
        workflows.downloads()->cancel();
        QTRY_VERIFY(!workflows.downloads()->busy());
        QVERIFY2(!workflows.notice().contains("Downloaded"), qPrintable(workflows.notice()));

        // One click (#419 follow-up): the button reads Install, and when everything is ready the sheet closes by itself and
        // the template it was for is the tab shown, even when the person looked at another tab meanwhile.
        QVERIFY(workflows.openTemplateById("last"));
        const int lastTab = workflows.current();
        QTRY_VERIFY(window->findChild<QObject *>("getReadySheet")->property("visible").toBool());
        QTRY_VERIFY(item(window->contentItem(), "getReadyAll") && item(window->contentItem(), "getReadyAll")->isVisible());
        QCOMPARE(item(window->contentItem(), "getReadyAll")->property("text").toString(), QString("Install"));
        workflows.setCurrent(0);
        click(item(window->contentItem(), "getReadyAll"));
        QTRY_VERIFY(!window->findChild<QObject *>("getReadySheet")->property("visible").toBool());
        QCOMPARE(workflows.current(), lastTab);
        QCOMPARE(workflows.tabs().at(lastTab).toMap().value("title").toString(), QString("Last"));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join("\n")));
        forgetDownloads();
    }

    // #605: a template whose model is not on disk but whose same weights are in another file is ready to run with it, says so, and a
    // click on its card (openTemplateById) opens a tab with the file in place and no sheet, the template's own file untouched; the banner
    // names the file and its Undo puts the template's own model back (UI design 2026-11-26).
    void aTemplateWithTheSameWeightsOnDiskIsReadyAndOpensWithThem() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        writeFile(models.filePath("diffusion_models/z_image_turbo_bf16.safetensors"), file + header);
        const QString hash = QStringLiteral("sha256:") + QString(64, u'6');
        makeTemplate(f.bundled.filePath("zit"), {{"title", "Z"}},
                     workflow({{"models", node("fugue/load-models", {{"diffusion_model", QJsonObject{{"resource", "fugue/diffusion-model"}, {"hash", hash},
                                                                                              {"name", "z_image_turbo_int8_convrot.safetensors"}}}})}}));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        const QVariantMap card = workflows.templates().at(0).toMap();
        QVERIFY(card.value("ready").toBool());
        QVERIFY(card.value("usesOnDisk").toBool());
        QCOMPARE(card.value("onDisk").toList().size(), 1);
        QCOMPARE(card.value("onDisk").toList().at(0).toMap().value("file").toString(), QString("z_image_turbo_bf16.safetensors"));
        QCOMPARE(card.value("onDisk").toList().at(0).toMap().value("same").toString(), QString("same model, bf16"));
        QCOMPARE(card.value("status").toString(), QString("Ready · uses your bf16 file"));
        QVERIFY(card.value("opensWithFiles").toBool());
        QCOMPARE(card.value("downloadSize").toString(), QString()); // the model the file stands in for is not a download of this card
        QVERIFY(card.value("download").toString().isEmpty());

        QVERIFY(workflows.openTemplateById("zit")); // the hash is read on a worker: the tab comes when it is known
        QVERIFY(workflows.hashingUses());
        QVERIFY(workflows.reading().contains(hash)); // this model's own reading, not a global one
        QVERIFY(workflows.openTemplateById("zit")); // a second click while it is read opens nothing more
        QTRY_VERIFY(canvas.document().graph.nodes.contains("models"));
        QTRY_VERIFY(!workflows.hashingUses());
        QVERIFY(workflows.reading().isEmpty());
        QVERIFY(!workflows.readiness()->isOpen()); // no sheet: the card was ready through the person's file
        int opened_ = 0;
        for (const QVariant &tab : workflows.tabs())
            opened_ += tab.toMap().value("title").toString() == QLatin1String("Z");
        QCOMPARE(opened_, 1);
        const QJsonObject opened = canvas.document().graph.nodes.value("models").inputs.value("diffusion_model").handle();
        QCOMPARE(opened.value("name").toString(), QString("z_image_turbo_bf16.safetensors"));
        QVERIFY(opened.value("hash").toString() != hash);
        QVERIFY(!workflows.missingSomething()); // nothing left to download
        QFile own(f.bundled.filePath("zit/workflow.fugue")); // the template itself is as it was
        QVERIFY(own.open(QIODevice::ReadOnly));
        QVERIFY(own.readAll().contains("z_image_turbo_int8_convrot.safetensors"));
        QVERIFY2(workflows.notice().contains("Opened with your file z_image_turbo_bf16.safetensors in place of z_image_turbo_int8_convrot.safetensors"),
                 qPrintable(workflows.notice()));
        QVERIFY(workflows.noticeUndoable());
        workflows.undoNotice(); // back to the template's own model, which needs a download
        QCOMPARE(canvas.document().graph.nodes.value("models").inputs.value("diffusion_model").handle().value("hash").toString(), hash);
        QVERIFY(!workflows.noticeUndoable());
        QVERIFY(workflows.missingSomething());
    }

    // #616, the end state the owner wants: every missing model has a file of the same weights, the file's hash is not known yet, and one
    // yes on the sheet (Use my file, its default) reads the file, sets it in the document (one undo step), closes the sheet and starts no
    // download: the person is on the tab, ready to run.
    void theYesOnTheGetReadySheetWithEveryModelCoveredUsesTheFilesAndStartsNoDownload() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        const QString weights = models.filePath("diffusion_models/z_image_turbo_bf16.safetensors");
        writeFile(weights, file + header);
        const QString hash = QStringLiteral("sha256:") + QString(64, u'7');
        const QJsonObject wanted{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", "z_image_turbo_int8_convrot.safetensors"}};
        QJsonObject recorded = wanted;
        recorded.insert("source", "http://127.0.0.1:9/z.safetensors");
        recorded.insert("size", 5000000000.0);
        makeTemplate(f.bundled.filePath("zit"), {{"title", "Z"}},
                     workflow({{"models", node("fugue/load-models", {{"diffusion_model", wanted}})}}, {{"models", QJsonArray{recorded}}}));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Translator translator(state.filePath("language")); // the plural forms of the catalog, as in the app
        QCoreApplication::installTranslator(&translator);
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QVERIFY(workflows.openTemplateForDownload("zit")); // as it is, with the sheet on Download (what the details' button does)
        ReadinessSheet *sheet = workflows.readiness();
        QVERIFY(sheet->isOpen());
        QCOMPARE(sheet->view().value("models").toList().at(0).toMap().value("choice").toString(), QString("download"));
        const int tab = workflows.current();
        sheet->show(); // opened again from the banner: the defaults, Use my file for the model that has one
        QCOMPARE(sheet->view().value("models").toList().size(), 1);
        const QVariantMap before = sheet->view().value("models").toList().at(0).toMap();
        QCOMPARE(before.value("hash").toString(), hash);
        QCOMPARE(before.value("choice").toString(), QString("disk"));
        QVERIFY(before.value("use").toMap().value("file").toString().contains("z_image_turbo_bf16"));
        QCOMPARE(sheet->view().value("primary").toString(), QString("Use my file"));
        QVERIFY(FileHashes(FileHashes::defaultPath()).known(weights).isEmpty()); // not read yet
        const int steps = canvas.undoStack()->count();

        QList<int> percents; // what the row is told as the file is hashed: unknown first, then the real percent
        connect(&workflows, &Workflows::readingChanged, this, [&] { if (workflows.reading().contains(hash)) percents << workflows.reading().value(hash).toInt(); });
        sheet->start();
        QVERIFY(workflows.hashingUses());
        QVERIFY(workflows.reading().contains(hash)); // the row's own busy state
        QCOMPARE(sheet->view().value("models").toList().at(0).toMap().value("state").toString(), QString("reading"));
        QTRY_VERIFY(!sheet->isOpen()); // closed by itself
        QVERIFY(!workflows.hashingUses());
        QVERIFY(workflows.reading().isEmpty());
        QCOMPARE(percents.first(), -1);
        QVERIFY2(percents.contains(100), "the hash's progress reaches the row"); // the worker tells its percent (ModelWorkers::hash)
        const QJsonObject handle = canvas.document().graph.nodes.value("models").inputs.value("diffusion_model").handle();
        QCOMPARE(handle.value("name").toString(), QString("z_image_turbo_bf16.safetensors")); // the document holds the file on disk
        QVERIFY(handle.value("hash").toString() != hash);
        QCOMPARE(canvas.undoStack()->count(), steps + 1);
        QCOMPARE(workflows.current(), tab); // the person is on the tab, ready to run
        QVERIFY(!workflows.downloads()->busy()); // no download was started
        QCOMPARE(sheet->phase(), QString("review"));
        QVERIFY(!workflows.missingSomething());
    }

    // Check 4: Cancel after choosing leaves the document and the undo stack as they were, also while the file is being read.
    void cancelLeavesTheDocumentAndTheUndoStackAsTheyWere() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        writeFile(models.filePath("diffusion_models/z_image_turbo_bf16.safetensors"), file + header);
        const QString hash = QStringLiteral("sha256:") + QString(64, u'9');
        const QJsonObject wanted{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", "z_image_turbo_int8_convrot.safetensors"}};
        QJsonObject recorded = wanted;
        recorded.insert("source", "http://127.0.0.1:9/z.safetensors");
        recorded.insert("size", 5000000000.0);
        makeTemplate(f.bundled.filePath("zit"), {{"title", "Z"}},
                     workflow({{"models", node("fugue/load-models", {{"diffusion_model", wanted}})}}, {{"models", QJsonArray{recorded}}}));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QVERIFY(workflows.openTemplateForDownload("zit"));
        ReadinessSheet *sheet = workflows.readiness();
        const int steps = canvas.undoStack()->count();
        sheet->setChoice(hash, true);
        sheet->cancel(); // the choice is discarded with the sheet
        QVERIFY(!sheet->isOpen());
        QCOMPARE(canvas.undoStack()->count(), steps);
        QCOMPARE(canvas.document().graph.nodes.value("models").inputs.value("diffusion_model").handle().value("hash").toString(), hash);
        // Cancel while the file is read: the read ends and nothing is applied.
        sheet->show();
        sheet->start();
        QVERIFY(workflows.hashingUses());
        sheet->cancel();
        QVERIFY(workflows.reading().isEmpty());
        QTRY_VERIFY(!workflows.hashingUses());
        QCOMPARE(canvas.undoStack()->count(), steps);
        QCOMPARE(canvas.document().graph.nodes.value("models").inputs.value("diffusion_model").handle().value("hash").toString(), hash);
    }

    // Check 5: a card whose models are all covered has no download to offer (the size is of the models a download would fetch); a mixed
    // card counts only the model without a file of its own and says how many the person has.
    void aCardCountsOnlyTheModelsAFileDoesNotStandInFor() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        writeFile(models.filePath("diffusion_models/z_image_turbo_bf16.safetensors"), file + header);
        const QJsonObject covered{{"resource", "fugue/diffusion-model"}, {"hash", "sha256:" + QString(64, u'c')}, {"name", "z_image_turbo_int8_convrot.safetensors"}};
        const QJsonObject other{{"resource", "fugue/diffusion-model"}, {"hash", "sha256:" + QString(64, u'd')}, {"name", "other-model.gguf"}};
        QJsonObject coveredRecord = covered, otherRecord = other;
        coveredRecord.insert("source", "http://127.0.0.1:9/z.safetensors");
        coveredRecord.insert("size", 8700000000.0);
        otherRecord.insert("source", "http://127.0.0.1:9/o.gguf");
        otherRecord.insert("size", 5900000000.0);
        makeTemplate(f.bundled.filePath("all"), {{"title", "All"}},
                     workflow({{"models", node("fugue/load-models", {{"diffusion_model", covered}})}}, {{"models", QJsonArray{coveredRecord}}}));
        makeTemplate(f.bundled.filePath("mix"), {{"title", "Mix"}},
                     workflow({{"a", node("fugue/load-models", {{"diffusion_model", covered}})}, {"b", node("fugue/load-models", {{"diffusion_model", other}})}},
                              {{"models", QJsonArray{coveredRecord, otherRecord}}}));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Translator translator(state.filePath("language"));
        QCoreApplication::installTranslator(&translator);
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QVariantMap all, mix;
        for (const QVariant &card : workflows.templates())
            (card.toMap().value("id").toString() == "all" ? all : mix) = card.toMap();
        QCOMPARE(all.value("downloadSize").toString(), QString());
        QCOMPARE(all.value("ownDownloadSize").toString(), QString("8.7 GB")); // the details' Download <size>…
        QVERIFY(all.value("opensWithFiles").toBool());
        QCOMPARE(mix.value("downloadSize").toString(), QString("5.9 GB"));
        QVERIFY(!mix.value("opensWithFiles").toBool()); // a click opens Get ready with its defaults
        QVERIFY2(mix.value("status").toString().contains("Models missing: 1 of 2 · you have 1 of them"), qPrintable(mix.value("status").toString()));
        QCoreApplication::removeTranslator(&translator);
    }

    // #616: a file that cannot be read says so in the sheet (a notice would be hidden behind it), the row is busy while it is read, and
    // it is offered again with Download chosen; the stale message goes when the choice changes.
    void aFileThatCannotBeReadIsSaidInTheGetReadySheet() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        const QString weights = models.filePath("diffusion_models/z_image_turbo_fp8.safetensors");
        writeFile(weights, file + header);
        const QString hash = QStringLiteral("sha256:") + QString(64, u'8');
        const QJsonObject wanted{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", "z_image_turbo_int8_convrot.safetensors"}};
        QJsonObject recorded = wanted;
        recorded.insert("source", "http://127.0.0.1:9/z.safetensors");
        recorded.insert("size", 5000000000.0);
        makeTemplate(f.bundled.filePath("zit"), {{"title", "Z"}},
                     workflow({{"models", node("fugue/load-models", {{"diffusion_model", wanted}})}}, {{"models", QJsonArray{recorded}}}));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QVERIFY(workflows.openTemplateForDownload("zit"));
        ReadinessSheet *sheet = workflows.readiness();
        sheet->show();
        QCOMPARE(sheet->view().value("models").toList().size(), 1); // the header is read and remembered here
        QVERIFY(QFile::setPermissions(weights, QFileDevice::Permissions()));
        sheet->start();
        QVERIFY(workflows.hashingUses());
        sheet->start(); // a second press while it is read: nothing more
        QTRY_VERIFY(!workflows.hashingUses());
        QVERIFY2(sheet->view().value("message").toString().contains("z_image_turbo_fp8.safetensors"), qPrintable(sheet->view().value("message").toString()));
        QVERIFY(!sheet->view().value("ok").toBool());
        QVERIFY(!workflows.notice().contains("could not be read") && !workflows.notice().contains("Could not read")); // said in the sheet, not behind it
        QCOMPARE(sheet->view().value("models").toList().size(), 1); // the row stays: the file is not used
        QCOMPARE(sheet->view().value("models").toList().at(0).toMap().value("choice").toString(), QString("download"));
        QVERIFY(sheet->isOpen());
        sheet->setChoice(hash, true); // the person tries again: the old message would be stale
        QCOMPARE(sheet->view().value("message").toString(), QString());
        QVERIFY(QFile::setPermissions(weights, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    }

    // A file chosen in the App view is the app's own: switching to the graph while its hash is read must not edit the workflow.
    void aChoiceMadeInTheAppViewStaysThereWhenTheViewSwitchesWhileItsFileIsRead() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        writeFile(models.filePath("diffusion_models/z_image_turbo_bf16.safetensors"), file + header);
        const QString hash = QStringLiteral("sha256:") + QString(64, u'4');
        makeTemplate(f.bundled.filePath("zit"), {{"title", "Z"}},
                     workflow({{"models", node("fugue/load-models", {{"diffusion_model", QJsonObject{{"resource", "fugue/diffusion-model"}, {"hash", hash},
                                                                                              {"name", "z_image_turbo_int8_convrot.safetensors"}}}})}}));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QVERIFY(workflows.openTemplateForDownload("zit"));
        bool appShown = true;
        int taken = 0;
        workflows.setAppShown([&appShown](const QString &) { return appShown; });
        workflows.setUseTaker([&taken](const QString &, const QList<OnDiskUse> &, const Uses &) {
            ++taken;
            return true;
        });
        const int steps = canvas.undoStack()->count();
        workflows.useOnDisk({hash});
        appShown = false; // the person switches to the graph while the file is read
        QTRY_COMPARE(taken, 1);
        QCOMPARE(canvas.undoStack()->count(), steps);
        QCOMPARE(canvas.document().graph.nodes.value("models").inputs.value("diffusion_model").handle().value("hash").toString(), hash);
    }

    // A file that cannot be read when its hash is wanted is said: the template opens without it and the banner says why.
    void aFileThatCannotBeReadIsSaidAndTheTemplateOpensWithout() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        const QString weights = models.filePath("diffusion_models/z_image_turbo_fp8.safetensors");
        writeFile(weights, file + header);
        makeTemplate(f.bundled.filePath("zit"), {{"title", "Z"}},
                     workflow({{"models", node("fugue/load-models", {{"diffusion_model", QJsonObject{{"resource", "fugue/diffusion-model"}, {"hash", "sha256:" + QString(64, u'5')},
                                                                                              {"name", "z_image_turbo_int8_convrot.safetensors"}}}})}}));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QCOMPARE(workflows.templates().at(0).toMap().value("onDisk").toList().size(), 1); // the header is read and remembered
        QVERIFY(QFile::setPermissions(weights, QFileDevice::Permissions()));
        QVERIFY(workflows.openTemplateWithWhatYouHave("zit"));
        QTRY_VERIFY(workflows.notice().contains("could not be read"));
        QVERIFY2(workflows.notice().contains("z_image_turbo_fp8.safetensors"), qPrintable(workflows.notice()));
        QVERIFY(!workflows.hashingUses());
        QVERIFY(QFile::setPermissions(weights, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    }

    // Undo of the banner puts back exactly the inputs that were changed, each its own value: two missing models that one file stands in
    // for get their two originals back, and a node that already used that file is not touched.
    void undoOfTheOpenedWithBannerRestoresEachInputItsOwnModel() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        writeFile(models.filePath("diffusion_models/z_image_turbo_bf16.safetensors"), file + header);
        const QString hashX = "sha256:" + QString(64, u'e'), hashY = "sha256:" + QString(64, u'f');
        const QJsonObject x{{"resource", "fugue/diffusion-model"}, {"hash", hashX}, {"name", "z_image_turbo_int8_convrot.safetensors"}};
        const QJsonObject y{{"resource", "fugue/diffusion-model"}, {"hash", hashY}, {"name", "z_image_turbo_fp8.safetensors"}};
        QJsonObject xr = x, yr = y;
        xr.insert("source", "http://127.0.0.1:9/x");
        yr.insert("source", "http://127.0.0.1:9/y");
        makeTemplate(f.bundled.filePath("twin"), {{"title", "Twin"}},
                     workflow({{"a", node("fugue/load-models", {{"diffusion_model", x}})}, {"b", node("fugue/load-models", {{"diffusion_model", y}})}},
                              {{"models", QJsonArray{xr, yr}}}));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QVERIFY(workflows.openTemplateById("twin"));
        QTRY_VERIFY(canvas.document().graph.nodes.contains("a"));
        const auto handleOf = [&](const char *node) { return canvas.document().graph.nodes.value(node).inputs.value("diffusion_model").handle(); };
        QCOMPARE(handleOf("a").value("name").toString(), QString("z_image_turbo_bf16.safetensors"));
        QCOMPARE(handleOf("b").value("hash").toString(), handleOf("a").value("hash").toString()); // one file for both
        QTRY_VERIFY(workflows.noticeUndoable());
        workflows.dismissNotice(); // the line is gone or replaced: Undo is still the tab's until it is edited
        workflows.notify("something else");
        QVERIFY(workflows.noticeUndoable());
        workflows.undoNotice();
        QCOMPARE(handleOf("a").value("hash").toString(), hashX);
        QCOMPARE(handleOf("b").value("hash").toString(), hashY);
        QVERIFY(!workflows.noticeUndoable());
    }

    // Cancelling the sheet while its files are read takes only the sheet's ask away: another surface's read of the same file goes on and applies.
    void aSheetsCancelLeavesAnotherSurfacesReadAlone() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        writeFile(models.filePath("diffusion_models/z_image_turbo_bf16.safetensors"), file + header);
        const QString hash = "sha256:" + QString(64, u'8');
        const QJsonObject wanted{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", "z_image_turbo_int8_convrot.safetensors"}};
        QJsonObject recorded = wanted;
        recorded.insert("source", "http://127.0.0.1:9/z");
        makeTemplate(f.bundled.filePath("zit"), {{"title", "Z"}}, workflow({{"models", node("fugue/load-models", {{"diffusion_model", wanted}})}}, {{"models", QJsonArray{recorded}}}));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QVERIFY(workflows.openTemplateForDownload("zit"));
        ReadinessSheet *sheet = workflows.readiness();
        sheet->show();
        workflows.useOnDisk({hash}); // the Needs panel's read of the file
        sheet->start();              // the sheet joins the same read
        sheet->cancel();
        QVERIFY(workflows.reading().contains(hash)); // the other surface still reads
        QTRY_VERIFY(!workflows.hashingUses());
        QCOMPARE(canvas.document().graph.nodes.value("models").inputs.value("diffusion_model").handle().value("name").toString(),
                 QString("z_image_turbo_bf16.safetensors")); // and its result applied
    }

    // The view a caller asks for (--edit: the graph, --app: the App view) is chosen when the tab exists: a template that opens with the
    // person's files does so after reading them, and its caller's callback runs then, never before; an ordinary template's at once.
    void theCallersViewIsChosenWhenTheTabExists() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        writeFile(models.filePath("diffusion_models/z_image_turbo_bf16.safetensors"), file + header);
        const QJsonObject wanted{{"resource", "fugue/diffusion-model"}, {"hash", "sha256:" + QString(64, u'a')}, {"name", "z_image_turbo_int8_convrot.safetensors"}};
        QJsonObject recorded = wanted;
        recorded.insert("source", "http://127.0.0.1:9/z");
        makeTemplate(f.bundled.filePath("zit"), {{"title", "Z"}}, workflow({{"models", node("fugue/load-models", {{"diffusion_model", wanted}})}}, {{"models", QJsonArray{recorded}}}));
        makeTemplate(f.bundled.filePath("plain"), {{"title", "Plain"}});
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        int calls = 0;
        bool tabThere = false;
        const auto then = [&] { ++calls; tabThere = canvas.document().graph.nodes.contains("models"); };
        AppRef zit;
        zit.templ = f.library().find("zit");
        QVERIFY(openApp(workflows, zit, then));
        QCOMPARE(calls, 0); // the files are read first: the view is not chosen yet
        QTRY_COMPARE(calls, 1);
        QVERIFY(tabThere); // and then the tab is there, with the file in it
        AppRef plain;
        plain.templ = f.library().find("plain");
        QVERIFY(openApp(workflows, plain, then));
        QCOMPARE(calls, 2); // an ordinary template: at once
    }

    // A read cancelled in the graph and asked again in the App view applies to the App view: each ask keeps its own destination.
    void aCancelledReadAskedAgainInTheAppViewIsTheAppViews() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        writeFile(models.filePath("diffusion_models/z_image_turbo_bf16.safetensors"), file + header);
        const QString hash = "sha256:" + QString(64, u'b');
        const QJsonObject wanted{{"resource", "fugue/diffusion-model"}, {"hash", hash}, {"name", "z_image_turbo_int8_convrot.safetensors"}};
        QJsonObject recorded = wanted;
        recorded.insert("source", "http://127.0.0.1:9/z");
        makeTemplate(f.bundled.filePath("zit"), {{"title", "Z"}}, workflow({{"models", node("fugue/load-models", {{"diffusion_model", wanted}})}}, {{"models", QJsonArray{recorded}}}));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        QVERIFY(workflows.openTemplateForDownload("zit"));
        bool inApp = false;
        int taken = 0;
        workflows.setAppShown([&inApp](const QString &) { return inApp; });
        workflows.setUseTaker([&taken](const QString &, const QList<OnDiskUse> &, const Uses &) {
            ++taken;
            return true;
        });
        const int steps = canvas.undoStack()->count();
        workflows.useOnDisk({hash}, 7); // the sheet's ask, in the graph
        workflows.cancelUses(7);        // cancelled while the file is read
        inApp = true;                   // the person goes to the App view and asks again
        workflows.useOnDisk({hash}, 0);
        workflows.useOnDisk({hash}, 0); // a second press during the read: the same destination takes the result once
        QTRY_VERIFY(!workflows.hashingUses());
        QCOMPARE(taken, 1);             // the App view's state took it
        QCOMPARE(canvas.undoStack()->count(), steps); // the workflow itself was not edited
        QCOMPARE(canvas.document().graph.nodes.value("models").inputs.value("diffusion_model").handle().value("hash").toString(), hash);
    }

    // A mixed card's Download fetches only what its label counts: the model a file of the person's stands in for is not downloaded.
    void aMixedCardsDownloadFetchesOnlyTheModelsWithoutAFileOfTheirOwn() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        writeFile(models.filePath("diffusion_models/z_image_turbo_bf16.safetensors"), file + header);
        fugue::test::FakeHttpServer server;
        QVERIFY(server.listen());
        const QByteArray bytes = "the other model";
        server.on("GET", "/z.safetensors", [&](const fugue::test::HttpRequest &) { return fugue::test::HttpResponse{200, "z", "application/octet-stream"}; });
        server.on("GET", "/o.gguf", [&](const fugue::test::HttpRequest &) { return fugue::test::HttpResponse{200, bytes, "application/octet-stream"}; });
        QJsonObject covered{{"resource", "fugue/diffusion-model"}, {"hash", "sha256:" + QString(64, u'c')}, {"name", "z_image_turbo_int8_convrot.safetensors"}};
        QJsonObject other{{"resource", "fugue/diffusion-model"}, {"hash", sha256(bytes)}, {"name", "o.gguf"}};
        QJsonObject coveredRecord = covered, otherRecord = other;
        coveredRecord.insert("source", QString::fromLatin1(server.origin() + "/z.safetensors"));
        coveredRecord.insert("size", 1);
        otherRecord.insert("source", QString::fromLatin1(server.origin() + "/o.gguf"));
        otherRecord.insert("size", bytes.size());
        makeTemplate(f.bundled.filePath("mix"), {{"title", "Mix"}},
                     workflow({{"a", node("fugue/load-models", {{"diffusion_model", covered}})}, {"b", node("fugue/load-models", {{"diffusion_model", other}})}},
                              {{"models", QJsonArray{coveredRecord, otherRecord}}}));
        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        workflows.downloadTemplate("mix");
        QTRY_COMPARE(server.requestsTo("GET", "/o.gguf").size(), 1);
        QTRY_VERIFY(!workflows.downloads()->busy());
        QCOMPARE(server.requestsTo("GET", "/z.safetensors").size(), 0); // the one the person's file stands in for
        forgetDownloads();
    }

    // The window side of "Use my file" (UI design 2026-11-26, #616): in the Templates dialog a card that is ready only through the person's
    // file says so with a check, Enter on it opens the template with the file and no sheet, a banner names the file and has Undo, the
    // details offer the template's own download as a secondary, and a card with a model that has no file shows the one download. With
    // FUGUE_SHOT_DIR it also draws the pictures of the card and of the Get ready sheet (Use my file, Download chosen, mixed).
    void theDialogOpensAReadyThroughYourFileCardWithItAndSaysSo() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray header = R"({"cap_embedder.1.weight": {"dtype": "F16", "shape": [3840, 2560], "data_offsets": [0, 0]},
                                      "x_embedder.weight": {"dtype": "F16", "shape": [3840, 64], "data_offsets": [0, 0]}})";
        QByteArray file;
        for (int i = 0; i < 8; ++i)
            file.append(char((quint64(header.size()) >> (8 * i)) & 0xff));
        writeFile(models.filePath("diffusion_models/z_image_turbo_bf16.safetensors"), file + header);
        QJsonObject covered{{"resource", "fugue/diffusion-model"}, {"hash", "sha256:" + QString(64, u'1')}, {"name", "z_image_turbo_int8_convrot.safetensors"}};
        QJsonObject other{{"resource", "fugue/diffusion-model"}, {"hash", "sha256:" + QString(64, u'2')}, {"name", "flux-2-klein-9b-Q4_K_M.gguf"}};
        const QJsonObject coveredHandle = covered, otherHandle = other;
        covered.insert("source", "http://127.0.0.1:9/z.safetensors");
        covered.insert("size", 8700000000.0);
        other.insert("source", "http://127.0.0.1:9/k.gguf");
        other.insert("size", 5900000000.0);
        makeTemplate(f.bundled.filePath("zit"), {{"title", "Z-Image Turbo"}, {"description", "Text to image, 8 steps, from a prompt."}},
                     workflow({{"models", node("fugue/load-models", {{"diffusion_model", coveredHandle}})}}, {{"models", QJsonArray{covered}}}));
        makeTemplate(f.bundled.filePath("mix"), {{"title", "Z-Image and Klein"}, {"description", "Two models, one of them in your folders."}},
                     workflow({{"a", node("fugue/load-models", {{"diffusion_model", coveredHandle}})}, {"b", node("fugue/load-models", {{"diffusion_model", otherHandle}})}},
                              {{"models", QJsonArray{covered, other}}}));
        QTemporaryDir dir;
        QFile shell(dir.filePath("shell.toml"));
        QVERIFY(shell.open(QIODevice::WriteOnly));
        shell.write("[font]\nbase-size = 12\n");
        shell.close();
        Translator translator(dir.path() + "/language");
        QCoreApplication::installTranslator(&translator); // the English plural forms, as main() does
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
        const QString shots = qEnvironmentVariable("FUGUE_SHOT_DIR");
        const auto shoot = [&](const QString &name) {
            if (shots.isEmpty())
                return;
            QDir().mkpath(shots);
            window->resize(900, 700);
            QTest::qWait(500);
            QVERIFY(test_support::grab(window).save(shots + "/" + name + ".png"));
        };

        QVERIFY(commands.run("file.templates"));
        QTRY_VERIFY(window->findChild<QObject *>("templatesDialog")->property("opened").toBool());
        QTRY_VERIFY(item(window->contentItem(), "templateCard-zit") && item(window->contentItem(), "templateCard-mix"));
        QCOMPARE(item(window->contentItem(), "templateStatus-zit")->property("text").toString(), QString("Ready · uses your bf16 file"));
        QVERIFY(item(window->contentItem(), "templateReadyGlyph-zit")->isVisible());
        QVERIFY(!item(window->contentItem(), "templateDownload-zit")->isVisible()); // nothing a download would fetch
        QCOMPARE(item(window->contentItem(), "templateStatus-mix")->property("text").toString(), QString("Models missing: 1 of 2 · you have 1 of them"));
        QVERIFY(!item(window->contentItem(), "templateReadyGlyph-mix")->isVisible());
        QCOMPARE(item(window->contentItem(), "templateDownload-mix")->property("text").toString(), QString("Download 5.9 GB")); // the uncovered one only
        shoot("template-card-ready-through-your-file");

        // The details: the template's own download is the secondary, left of Open.
        QObject *dialog = window->findChild<QObject *>("templatesDialog");
        QVERIFY(QMetaObject::invokeMethod(dialog, "showDetails", Q_ARG(QVariant, "zit"), Q_ARG(QVariant, QVariant())));
        QQuickItem *own = nullptr;
        QTRY_VERIFY((own = item(window->contentItem(), "templateDetailsDownload")) && own->isVisible());
        QCOMPARE(own->property("text").toString(), QString("Download 8.7 GB…"));
        shoot("template-details-download");
        QVERIFY(QMetaObject::invokeMethod(dialog, "closeDetails"));

        // The mixed card's chip fetches only what it counts (5.9 GB): the download line names the one model, never the one the file stands in for.
        QQuickItem *chip = item(window->contentItem(), "templateDownload-mix");
        QVERIFY(chip && chip->isVisible());
        QMetaObject::invokeMethod(chip, "tapped");
        QTRY_VERIFY(workflows.downloads()->busy() || workflows.downloads()->failedWhat() == QString("template:mix"));
        const QString line = workflows.downloads()->busy() ? workflows.downloads()->label() : workflows.downloads()->failure();
        QVERIFY2(!line.contains("z_image_turbo"), qPrintable(line));
        workflows.downloads()->cancel();
        QTRY_VERIFY(!workflows.downloads()->busy());

        // Enter on the card: opens with the file, no sheet, the banner says so, Undo puts the template's own model back.
        QTRY_VERIFY(item(window->contentItem(), "templatesGrid"));
        item(window->contentItem(), "templatesGrid")->forceActiveFocus();
        int at = 0;
        for (const QVariant &card : dialog->property("shown").toList())
            if (card.toMap().value("id").toString() == "zit")
                break;
            else
                ++at;
        item(window->contentItem(), "templatesGrid")->setProperty("currentIndex", at);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(canvas.document().graph.nodes.contains("models"));
        QCOMPARE(canvas.document().graph.nodes.value("models").inputs.value("diffusion_model").handle().value("name").toString(),
                 QString("z_image_turbo_bf16.safetensors"));
        QVERIFY(!workflows.readiness()->isOpen());
        QTRY_VERIFY(!window->findChild<QObject *>("templatesDialog")->property("visible").toBool());
        QQuickItem *undo = nullptr;
        QTRY_VERIFY((undo = item(window->contentItem(), "noticeUndo")) && undo->isVisible());
        QVERIFY(workflows.notice().contains("Opened with your file z_image_turbo_bf16.safetensors"));
        shoot("notice-opened-with-your-file");
        QMetaObject::invokeMethod(undo, "clicked");
        QTRY_COMPARE(canvas.document().graph.nodes.value("models").inputs.value("diffusion_model").handle().value("hash").toString(),
                     coveredHandle.value("hash").toString());

        // Get ready, by the real plan: the covered model on Use my file, then on Download, then the mixed plan.
        ReadinessSheet *sheet = workflows.readiness();
        QVERIFY(workflows.openTemplateForDownload("zit"));
        sheet->show();
        QObject *sheetItem = nullptr;
        QTRY_VERIFY((sheetItem = window->findChild<QObject *>("getReadySheet")) && sheetItem->property("visible").toBool());
        QQuickItem *choice = nullptr;
        QTRY_VERIFY((choice = item(window->contentItem(), "getReadyChoice-z_image_turbo_int8_convrot.safetensors")) && choice->isVisible());
        QCOMPARE(item(window->contentItem(), "getReadyAll")->property("text").toString(), QString("Use my file"));
        QVERIFY(!item(window->contentItem(), "getReadyDestination")->isVisible());
        shoot("get-ready-use-my-file");
        sheet->setChoice(coveredHandle.value("hash").toString(), false);
        QTRY_COMPARE(item(window->contentItem(), "getReadyAll")->property("text").toString(), QString("Install"));
        QTRY_VERIFY(item(window->contentItem(), "getReadyDestination")->isVisible());
        shoot("get-ready-download-chosen");
        sheet->cancel();
        QTRY_VERIFY(!sheetItem->property("visible").toBool());
        QVERIFY(workflows.openTemplateForDownload("mix"));
        sheet->show();
        QTRY_VERIFY(sheetItem->property("visible").toBool() && item(window->contentItem(), "getReadyDestination")->isVisible());
        QCOMPARE(item(window->contentItem(), "getReadyAll")->property("text").toString(), QString("Install"));
        QTRY_VERIFY(sheet->view().value("headline").toString().contains("download 1 model, 5.9 GB and use 1 file you already have"));
        shoot("get-ready-mixed");
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join("\n")));
    }

    // #164: a card says where its models are: on this machine, on a paired host, both, or
    // nowhere (then the Models missing line); a host that is offline is not a place.
    void aTemplateSaysWhereItsModelsAre() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QDir().mkpath(models.filePath("diffusion_models"));
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray a = "model a", b = "model b", c = "model c", d = "model d";
        writeFile(models.filePath("diffusion_models/a.gguf"), a);
        makeTemplateNeeding(f.bundled.filePath("local"), "Local", a, "https://x/a", "a.gguf");
        makeTemplateNeeding(f.bundled.filePath("host"), "Host", b, "https://x/b", "b.gguf");
        makeTemplateNeedingTwo(f.bundled.filePath("mix"), "Mix", a, b, "a.gguf", "b.gguf"); // a here, b's encoder on the host
        makeTemplateNeeding(f.bundled.filePath("gone"), "Gone", c, "https://x/c", "c.gguf");
        makeTemplateNeeding(f.bundled.filePath("dark"), "Dark", d, "https://x/d", "d.gguf");
        makeTemplate(f.bundled.filePath("plain"), {{"title", "Plain"}}); // no model file

        CanvasModel canvas(&f.registry);
        CommandRegistry commands;
        QTemporaryDir state;
        WorkflowLocations locations;
        locations.recoveryDir = state.filePath("recovery");
        locations.trustPath = state.filePath("trust.json");
        locations.templateRoots = {{f.own.path(), true}, {f.bundled.path(), false}};
        Workflows workflows(&canvas, &f.registry, &commands, locations);
        workflows.setModelSource([&](const QString &, const QStringList &, std::function<void(QList<ListedModel>)> done) {
            done({servedBy("gx10", "fugue/diffusion-model", b, "b.gguf"),
                  servedBy("gx10", "fugue/text-encoder", b, "b.gguf"),
                  servedBy("spark", "fugue/diffusion-model", d, "d.gguf", "unavailable")});
        });
        const auto byId = [&] {
            QHash<QString, QVariantMap> out;
            for (const QVariant &t : workflows.templates())
                out.insert(t.toMap().value("id").toString(), t.toMap());
            return out;
        };
        // Before the hosts have answered: only the disk is known.
        QVERIFY(byId().value("local").value("ready").toBool());
        QVERIFY(!byId().value("host").value("ready").toBool());

        QSignalSpy changed(&workflows, &Workflows::templatesChanged);
        workflows.loadHostModels();
        QCOMPARE(changed.size(), 1);
        const QHash<QString, QVariantMap> t = byId();
        QVERIFY(t.value("local").value("local").toBool());
        QVERIFY(t.value("local").value("hosts").toStringList().isEmpty());
        QVERIFY(t.value("local").value("ready").toBool());
        QVERIFY(!t.value("host").value("local").toBool());
        QCOMPARE(t.value("host").value("hosts").toStringList(), QStringList{"gx10"});
        QVERIFY(t.value("host").value("ready").toBool());
        QVERIFY(t.value("mix").value("local").toBool()); // a on disk,
        QCOMPARE(t.value("mix").value("hosts").toStringList(), QStringList{"gx10"}); // the encoder on the host
        QVERIFY(t.value("mix").value("ready").toBool());
        QVERIFY(!t.value("gone").value("ready").toBool());
        QVERIFY(!t.value("gone").value("local").toBool());
        QVERIFY(t.value("gone").value("hosts").toStringList().isEmpty());
        QCOMPARE(t.value("gone").value("status").toString(), QString("Models missing: 1 of 1"));
        QVERIFY(!t.value("dark").value("ready").toBool());
        QVERIFY(t.value("dark").value("hosts").toStringList().isEmpty());
        QCOMPARE(t.value("dark").value("offlineHosts").toStringList(), QStringList{"spark"});
        QVERIFY(t.value("plain").value("ready").toBool());
        QVERIFY(!t.value("plain").value("local").toBool());
    }

    // #164 in the window: the badges on the cards, their tooltips, the Ready to run chip, and
    // ready templates first within a category.
    void theDialogShowsThePlacesAndFiltersByReadyToRun() {
        forgetDownloads();
        Fixture f;
        QTemporaryDir models;
        QDir().mkpath(models.filePath("diffusion_models"));
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{models.path()}).isEmpty());
        const QByteArray a = "model a", b = "model b", c = "model c", d = "model d";
        writeFile(models.filePath("diffusion_models/a.gguf"), a);
        writeJson(f.bundled.filePath("categories.json"),
                  {{"categories", QJsonArray{QJsonObject{{"id", "image"}, {"title", "Image generation"}}}}});
        // Title order is Alpha, Beta, Gamma, Delta: Alpha needs c (nowhere), so it sorts after the ready ones.
        makeTemplateNeeding(f.bundled.filePath("alpha"), "Alpha", c, "https://x/c", "c.gguf");
        makeTemplateNeeding(f.bundled.filePath("beta"), "Beta", a, "https://x/a", "a.gguf");
        makeTemplateNeeding(f.bundled.filePath("gamma"), "Gamma", b, "https://x/b", "b.gguf");
        makeTemplateNeedingTwo(f.bundled.filePath("delta"), "Delta", a, b, "a.gguf", "b.gguf");
        for (const char *id : {"alpha", "beta", "gamma", "delta"}) {
            QJsonObject about = QJsonDocument::fromJson(readFile(f.bundled.filePath(QString(id) + "/template.json"))).object();
            about.insert("category", "image");
            writeJson(f.bundled.filePath(QString(id) + "/template.json"), about);
        }
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
        workflows.setModelSource([&](const QString &, const QStringList &, std::function<void(QList<ListedModel>)> done) {
            done({servedBy("gx10", "fugue/diffusion-model", b, "b.gguf"), servedBy("gx10", "fugue/text-encoder", b, "b.gguf")});
        });

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
        const auto visible = [&](const QString &name) {
            QQuickItem *found = item(window->contentItem(), name);
            return found && found->isVisible();
        };

        QVERIFY(commands.run("file.templates"));
        QTRY_VERIFY(visible("templatePlaceLocal-beta"));
        // Beta: this machine only. Gamma: the host only. Delta: both. Alpha: nowhere.
        QTRY_VERIFY(visible("templatePlaceHost-gamma"));
        QVERIFY(!visible("templatePlaceHost-beta"));
        QVERIFY(!visible("templatePlaceLocal-gamma"));
        QVERIFY(visible("templatePlaceLocal-delta") && visible("templatePlaceHost-delta"));
        QVERIFY(!visible("templatePlaces-alpha"));
        QCOMPARE(item(window->contentItem(), "templatePlaceLocal-beta")->property("tip").toString(), QString("On this machine"));
        QCOMPARE(item(window->contentItem(), "templatePlaceHost-gamma")->property("tip").toString(), QString("Shared by gx10"));
        QVERIFY(qFuzzyCompare(item(window->contentItem(), "templatePlaceHost-gamma")->opacity(), 1.0));
        // Ready ones first within the category (list order is Alpha, Beta, Delta, Gamma).
        const auto order = [&] {
            QList<QPair<qreal, QString>> cards;
            for (const char *id : {"alpha", "beta", "gamma", "delta"})
                if (QQuickItem *card = item(window->contentItem(), QString("templateCard-") + id))
                    cards.append({card->mapToScene(QPointF(0, 0)).y() * 10000 + card->mapToScene(QPointF(0, 0)).x(), id});
            std::sort(cards.begin(), cards.end());
            QStringList ids;
            for (const auto &card : cards)
                ids << card.second;
            return ids;
        };
        QTRY_COMPARE(order().last(), QString("alpha"));

        // The chip hides the one that cannot run.
        QQuickItem *chip = item(window->contentItem(), "templatesReadyOnly");
        QVERIFY(chip);
        QTest::mouseClick(window, Qt::LeftButton, {}, chip->mapToScene(QPointF(chip->width() / 2, chip->height() / 2)).toPoint());
        QTRY_VERIFY(!item(window->contentItem(), "templateCard-alpha"));
        QVERIFY(item(window->contentItem(), "templateCard-beta"));
        QVERIFY(item(window->contentItem(), "templateCard-gamma"));
        QVERIFY(item(window->contentItem(), "templateCard-delta"));
        QTest::mouseClick(window, Qt::LeftButton, {}, chip->mapToScene(QPointF(chip->width() / 2, chip->height() / 2)).toPoint());
        QTRY_VERIFY(item(window->contentItem(), "templateCard-alpha"));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join("\n")));
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
            // A pack the template needs is one it carries (11.10), so the window can offer it; its
            // nodes are checked below with the carried packs loaded.
            Registry withPacks;
            nodes::addBuiltinNodeTypes(withPacks);
            for (const QString &needed : t.neededPacks) {
                bool carried = false;
                for (const CarriedPack &pack : t.carriedPacks)
                    carried = carried || pack.id == needed;
                QVERIFY2(carried, qPrintable(t.id + " needs " + needed + " and does not carry it"));
            }
            for (const CarriedPack &pack : t.carriedPacks) {
                const LoadedPack loaded = loadPack(withPacks, pack.folder);
                QVERIFY2(loaded.enabled && loaded.disabled.isEmpty(), qPrintable(t.id + ": " + pack.id + " does not load"));
            }
            QVERIFY2(!t.preview.isEmpty(), qPrintable(t.id));
            for (const QJsonValue model : t.models) {
                QVERIFY2(model.toObject().value("source").toString().startsWith("https://"), qPrintable(t.id));
                QVERIFY2(model.toObject().value("size").toDouble() > 0, qPrintable(t.id)); // the Download button's total
            }
            const Document document = Document::loadFile(t.workflow).value();
            const Compilation compiled = compile(document, withPacks);
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
        for (const char *wanted : {"text-to-video", "image-to-video", "image-chain-to-video", "image-upscale", "video-upscale",
                                   "video-to-video", "inpaint-region", "outpaint"})
            QVERIFY2(found.contains(QLatin1String(wanted)),
                     qPrintable(QStringLiteral("the template %1 is not here").arg(wanted) + fix));
        // The two categories hold what they are for.
        QCOMPARE(library.find("text-to-video")->category, QStringLiteral("video"));
        QCOMPARE(library.find("image-upscale")->category, QStringLiteral("upscale"));
        QCOMPARE(library.find("video-upscale")->category, QStringLiteral("upscale"));
        QCOMPARE(library.find("image-upscale")->categoryTitle, QStringLiteral("Upscaling"));
        QCOMPARE(library.find("text-to-video")->categoryTitle, QStringLiteral("Video"));
    }

    // The generative fill template (#210, spec 14.5): Canvas Region, a Klein inpaint and Canvas Write
    // that lands its result on the canvas as a layer, in plain nodes (no group), with no canvas chosen.
    void theGenerativeFillTemplateFillsACanvasRegionAndLandsALayer() {
        const QString root = QStringLiteral(FUGUE_SOURCE_DIR "/templates");
        QVERIFY2(bundledProblem(root).isEmpty(), qPrintable(bundledProblem(root)));
        Fixture f;
        const TemplateLibrary library({{root, false}}, &f.registry);
        const auto found = library.find("generative-fill");
        QVERIFY2(found.has_value(), "the template generative-fill is not here; the templates submodule is stale");
        QCOMPARE(found->category, QStringLiteral("canvas"));
        const Document document = Document::loadFile(found->workflow).value();
        QVERIFY(!hasErrors(compile(document, f.registry).problems()));
        const auto &nodes = document.graph.nodes;
        QCOMPARE(nodes.value("region").type, QStringLiteral("fugue/canvas-region"));
        QCOMPARE(nodes.value("write").type, QStringLiteral("fugue/canvas-write"));
        QVERIFY(!nodes.value("write").inputs.contains("candidate")); // lands on the head, and never mentions candidates (#239)
        QVERIFY(nodes.value("region").inputs.value("canvas").literal().toString().isEmpty()); // the person picks one
        QCOMPARE(nodes.value("sample").inputs.value("steps").literal().toInt(), 4);
        QCOMPARE(nodes.value("sample").inputs.value("guidance").literal().toDouble(), 1.0);
        QVERIFY(document.groups.isEmpty());
    }

    // The flat Harmonize template: Load Image with a painted mask of the pasted parts, a seam softened in the reference, the
    // seam map, Klein 4B, colours matched back to the original, resized back, saved; in plain nodes, loading the sample
    // collage and its mask from inputs/.
    void theHarmonizeImageTemplateMakesACollageOneWithASeamMap() {
        const QString root = QStringLiteral(FUGUE_SOURCE_DIR "/templates");
        QVERIFY2(bundledProblem(root).isEmpty(), qPrintable(bundledProblem(root)));
        Fixture f;
        const TemplateLibrary library({{root, false}}, &f.registry);
        const auto found = library.find("harmonize-image");
        QVERIFY2(found.has_value(), "the template harmonize-image is not here; the templates submodule is stale");
        QCOMPARE(found->category, QStringLiteral("edit"));
        const Document document = Document::loadFile(found->workflow).value();
        QVERIFY(!hasErrors(compile(document, f.registry).problems()));
        const auto &nodes = document.graph.nodes;
        QCOMPARE(nodes.value("sample").inputs.value("steps").literal().toInt(), 4);
        QCOMPARE(nodes.value("sample").inputs.value("guidance").literal().toDouble(), 1.0);
        QCOMPARE(nodes.value("sample").inputs.value("denoise").literal().toDouble(), 1.0);
        QVERIFY(nodes.value("sample").inputs.value("soft_mask").literal().toBool());
        QCOMPARE(nodes.value("match").type, QStringLiteral("fugue/color-match"));
        QCOMPARE(nodes.value("match").inputs.value("reference").from().toString(), QStringLiteral("load.image"));
        QCOMPARE(nodes.value("save").type, QStringLiteral("fugue/save-image"));
        QVERIFY(document.groups.isEmpty());
        const QDir folder = QFileInfo(found->workflow).dir();
        for (const QString &input : {nodes.value("load").inputs.value("file").literal().toString(), nodes.value("load").inputs.value("mask").literal().toString()})
            QVERIFY2(QFileInfo::exists(folder.filePath(input)), qPrintable(input));
        // The canvas action of the same name is in the Canvas actions category.
        const auto action = library.find("harmonize");
        QVERIFY(action.has_value());
        QCOMPARE(action->category, QStringLiteral("canvas"));
    }

    // Refine, Remove and Expand (#231 slices 5 and 7): canvas actions in the Canvas category, each with a preview.
    void theRefineRemoveAndExpandTemplatesAreCanvasActions() {
        const QString root = QStringLiteral(FUGUE_SOURCE_DIR "/templates");
        Fixture f;
        const TemplateLibrary library({{root, false}}, &f.registry);
        for (const char *id : {"refine", "remove", "expand", "upscale", "upscale-no-model"}) {
            const auto found = library.find(id);
            QVERIFY2(found.has_value(), id);
            QCOMPARE(found->category, QStringLiteral("canvas"));
            QVERIFY(!hasErrors(compile(Document::loadFile(found->workflow).value(), f.registry).problems()));
        }
    }

    // The video to video template (#130): VAE Encode Video makes the start latent of a clip that
    // ships beside the template, and the Sampler 1.2.0 redraws it by Denoise, in plain nodes.
    void theVideoToVideoTemplateRedrawsAClipByDenoise() {
        const QString root = QStringLiteral(FUGUE_SOURCE_DIR "/templates");
        QVERIFY2(bundledProblem(root).isEmpty(), qPrintable(bundledProblem(root)));
        Fixture f;
        const TemplateLibrary library({{root, false}}, &f.registry);
        const auto found = library.find("video-to-video");
        QVERIFY2(found.has_value(), "the template video-to-video is not here; the templates submodule is stale");
        QCOMPARE(found->category, QStringLiteral("video"));
        const Document document = Document::loadFile(found->workflow).value();
        QVERIFY(!hasErrors(compile(document, f.registry).problems()));
        const auto &nodes = document.graph.nodes;
        QCOMPARE(nodes.value("clip").type, QStringLiteral("fugue/vae-encode-video"));
        QCOMPARE(nodes.value("sample").type, QStringLiteral("fugue/sampler"));
        QCOMPARE(nodes.value("sample").version.toString(), QStringLiteral("1.2.0"));
        const double denoise = nodes.value("sample").inputs.value("denoise").literal().toDouble();
        QVERIFY(denoise > 0.0 && denoise < 1.0);
        QVERIFY(nodes.value("clip").inputs.value("frames").literal().toInt() > 0);
        QVERIFY(document.groups.isEmpty());
        QVERIFY(QFileInfo::exists(QFileInfo(found->workflow).absolutePath() + "/inputs/fox.mp4"));
    }

    // The two masking templates (#177, spec 14.4): in Image editing, with the painted sample mask
    // beside the picture (an input file, as a picture is), and the settings that a group or a node
    // leaves without a default set for Klein: 4 steps, guidance 1.
    void theMaskTemplatesCarryTheirMaskAndSetStepsAndGuidance() {
        const QString root = QStringLiteral(FUGUE_SOURCE_DIR "/templates");
        QVERIFY2(bundledProblem(root).isEmpty(), qPrintable(bundledProblem(root)));
        Fixture f;
        const TemplateLibrary library({{root, false}}, &f.registry);
        for (const char *id : {"inpaint-region", "outpaint"}) {
            const auto found = library.find(id);
            QVERIFY2(found.has_value(), id);
            const Template *t = &*found;
            QCOMPARE(t->category, QStringLiteral("edit"));
            const Document document = Document::loadFile(t->workflow).value();
            QVERIFY2(!hasErrors(compile(document, f.registry).problems()), id);
            for (const QString &node : {QStringLiteral("inpaint"), QStringLiteral("sample")}) {
                if (!document.graph.nodes.contains(node))
                    continue;
                const auto &inputs = document.graph.nodes.value(node).inputs;
                QCOMPARE(inputs.value("steps").literal().toInt(), 4);
                QCOMPARE(inputs.value("guidance").literal().toDouble(), 1.0);
            }
        }
        const Document inpaint = Document::loadFile(library.find("inpaint-region")->workflow).value();
        const QString mask = inpaint.graph.nodes.value("load").inputs.value("mask").literal().toString();
        QVERIFY(mask.startsWith("inputs/"));
        QVERIFY2(QFileInfo::exists(QFileInfo(library.find("inpaint-region")->workflow).dir().filePath(mask)), qPrintable(mask));
        QVERIFY(inpaint.groups.contains("fugue/inpaint-crop-stitch"));
        // Outpaint's Pad: 256 px each side, and the padded picture is about a megapixel.
        const Document outpaint = Document::loadFile(library.find("outpaint")->workflow).value();
        QCOMPARE(outpaint.graph.nodes.value("pad").inputs.value("left").literal().toInt(), 256);
        QCOMPARE(outpaint.graph.nodes.value("pad").inputs.value("right").literal().toInt(), 256);
    }
};

QTEST_MAIN(TestTemplates)
#include "test_templates.moc"
