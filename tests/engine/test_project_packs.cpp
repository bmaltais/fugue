// Packs, trust, templates and prompts of a project (spec 1.4.2, epic #204 step 2 slice 5): what a project
// that inherits them sees of the Default's, read-only, and that nothing of the Default is ever written.
// Seam: engine/pack_loader.h, pack_manager.h, trust_store.h, templates.h, prompt_templates.h and
// registry_reloader.h in a real project root (setFugueHome) beside a Default made of temporary folders.
// The environment is the process's, so each test saves and restores what it sets.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "../disk_pack.h"
#include "engine/document.h"
#include "engine/fugue_home.h"
#include "engine/json.h"
#include "engine/model_catalog.h"
#include "engine/pack_loader.h"
#include "engine/pack_manager.h"
#include "engine/paths.h"
#include "engine/project.h"
#include "engine/prompt_templates.h"
#include "engine/providers.h"
#include "engine/registry.h"
#include "engine/registry_reloader.h"
#include "engine/settings.h"
#include "engine/templates.h"
#include "engine/trust_store.h"

using namespace fugue;
using pack_fixture::writeFile;
using pack_fixture::writeSayPack;

namespace {

const char *const kVariables[] = {"FUGUE_HOME", "FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME",
                                  "FUGUE_BASE_XDG_STATE_HOME", "FUGUE_BASE_XDG_CACHE_HOME", "FUGUE_BASE_XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
                                  "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "TMPDIR", "WAYLAND_DISPLAY",
                                  "DBUS_SESSION_BUS_ADDRESS", "CUDA_CACHE_PATH", "__GL_SHADER_DISK_CACHE_PATH",
                                  "PIPEWIRE_RUNTIME_DIR", "PULSE_RUNTIME_PATH", "HOME", "FUGUE_TEMPLATES"};

const QString kSay = QStringLiteral("io.example.tools");

// Every file under the Default's three folders (and the Documents folder), with its content.
QStringList snapshot(const QString &base, const QString &documents) {
    QStringList lines;
    for (const QString &folder : {base + "/config", base + "/data", base + "/state", documents}) {
        QDirIterator it(folder, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            QFile file(it.filePath());
            const QByteArray content = it.fileInfo().isFile() && file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
            lines << QStringLiteral("%1 %2 %3").arg(it.filePath()).arg(it.fileInfo().isDir() ? -1 : it.fileInfo().size()).arg(QString::fromLatin1(content.toHex()));
        }
    }
    lines.sort();
    return lines;
}

QStringList idsOf(const PackReport &report) {
    QStringList ids;
    for (const LoadedPack &pack : report.packs)
        ids << QStringLiteral("%1 %2%3%4").arg(pack.id, pack.version, pack.inherited ? " D" : "", pack.hidden ? " hidden" : "");
    return ids;
}

QByteArray workflowUsing(const QString &type) {
    return QJsonDocument(QJsonObject{{"fugue", 1},
                                     {"graph", QJsonObject{{"nodes", QJsonObject{{"n1", QJsonObject{{"type", type}, {"version", "1.0.0"}, {"inputs", QJsonObject()}}}}}}}})
        .toJson();
}

PackPrompts yes() {
    PackPrompts prompts;
    prompts.install = [](const PackSummary &) { return Answer::Yes; };
    prompts.remove = [](const PackSummary &) { return Answer::Yes; };
    prompts.trust = [](const PackSummary &) { return Answer::Yes; };
    prompts.runSetup = [](const PackSummary &) { return Answer::No; };
    return prompts;
}

} // namespace

class TestProjectPacks : public QObject {
    Q_OBJECT

    QHash<QByteArray, QByteArray> m_saved;
    QSet<QByteArray> m_unset;
    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }
    QString basePacks() const { return path("base/data/fugue/packs"); }
    QString baseTrust() const { return path("base/state/fugue/trust.json"); }
    QString ownPacks() const { return path("p/acme/data/fugue/packs"); }

    void enterProject(const QStringList &items, const QString &name = QStringLiteral("p/acme")) {
        ProjectManifest manifest;
        manifest.name = "Acme";
        manifest.inherits = items;
        QVERIFY(QDir().mkpath(path(name)));
        QVERIFY(writeProject(path(name), manifest).isEmpty());
        QVERIFY(setFugueHome(path(name), nullptr));
    }

    void trustInDefault(const QString &id, const QString &version) {
        TrustStore base(baseTrust());
        QVERIFY(base.trust(id, version).isEmpty());
    }

private slots:
    void init() {
        for (const char *variable : kVariables) {
            if (qEnvironmentVariableIsSet(variable))
                m_saved.insert(variable, qgetenv(variable));
            else
                m_unset.insert(variable);
            qunsetenv(variable);
        }
        qputenv("HOME", path("home").toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", path("base/config").toLocal8Bit());
        qputenv("XDG_DATA_HOME", path("base/data").toLocal8Bit());
        qputenv("XDG_STATE_HOME", path("base/state").toLocal8Bit());
        qputenv("XDG_RUNTIME_DIR", path("base/run").toLocal8Bit());
        QDir().mkpath(path("base/run"));
        QDir().mkpath(path("home"));
    }
    void cleanup() {
        for (auto it = m_saved.begin(); it != m_saved.end(); ++it)
            qputenv(it.key().constData(), it.value());
        for (const QByteArray &variable : std::as_const(m_unset))
            qunsetenv(variable.constData());
        m_saved.clear();
        m_unset.clear();
        QDir(path("base")).removeRecursively();
        QDir(path("p")).removeRecursively();
        QDir(path("home")).removeRecursively();
    }

    // Packs

    void inheritedPacksAreListedAndUsableAfterTheProjectsOwn() {
        QVERIFY(writeSayPack(basePacks(), "default"));
        enterProject({"packs"});
        Registry registry;
        const PackReport report = loadPacks(registry, packSearchPaths(), inheritedPackPaths());
        QCOMPARE(idsOf(report), (QStringList{"io.example.tools 1.0.0 D"}));
        QVERIFY(registry.nodeType("io.example.tools/say"));
        QCOMPARE(inheritedPackPaths().value(0), path("base/data/fugue/packs"));
        QVERIFY(isInheritedPack(kSay, "1.0.0"));
        QVERIFY(!report.hasProblems());
    }

    void theDefaultsPackPathsAreInheritedToo() {
        QVERIFY(writeSayPack(path("elsewhere"), "default"));
        QVERIFY(writeFile(path("base/config/fugue/settings.json"), writeJson(QJsonObject{{"pack_paths", QJsonArray{path("elsewhere")}}})));
        enterProject({"packs"});
        QCOMPARE(inheritedPackPaths(), (QStringList{path("base/data/fugue/packs"), path("elsewhere")}));
        QVERIFY(isInheritedPack(kSay, "1.0.0"));
        QVERIFY(packSearchPaths().size() == 1); // the project's own pack_paths are not the Default's
    }

    void aProjectReadsTheCatalogsOfTheInheritedPacksAndItsOwnWinById() {
        const auto catalog = [](const QString &id, const QString &title) {
            return QJsonDocument(QJsonObject{{"catalog", 1},
                                             {"models", QJsonArray{QJsonObject{{"id", id}, {"resource", "fugue/upscale-model"}, {"title", title},
                                                                              {"file", "a.safetensors"}, {"url", "https://example.org/a"},
                                                                              {"size", 1}, {"sha256", QString(64, 'a')}, {"scale", 4},
                                                                              {"architecture", "ESRGAN"}, {"license", "MIT"}}}}})
                .toJson();
        };
        QVERIFY(writeSayPack(basePacks(), "default"));
        QVERIFY(writeFile(basePacks() + "/io.example.tools/catalog/m.json", catalog("shared", "From Default pack")));
        QVERIFY(writeFile(basePacks() + "/io.example.tools/catalog/n.json", catalog("only-default-pack", "Only")));
        enterProject({"packs"});
        QVERIFY(writeFile(ownPacks() + "/io.example.other/pack.json", "{}"));
        QVERIFY(writeFile(ownPacks() + "/io.example.other/catalog/o.json", catalog("shared", "From own pack")));
        const ModelCatalog layered = ModelCatalog::load({}, {});
        QVERIFY(layered.find("only-default-pack") != nullptr);
        QCOMPARE(layered.find("shared")->title, QString("From own pack"));
        // Without `packs` the Default's packs' catalogs stay out.
        enterProject({}, "p/closed");
        QVERIFY(ModelCatalog::load({}, {}).find("only-default-pack") == nullptr);
    }

    void aProjectWithoutInheritsSeesNoPacksOfTheDefault() {
        QVERIFY(writeSayPack(basePacks(), "default"));
        trustInDefault(kSay, "1.0.0");
        enterProject({"models", "preferences"});
        QVERIFY(inheritedPackPaths().isEmpty());
        QVERIFY(inheritedPacks().isEmpty());
        Registry registry;
        QVERIFY(loadInstalledPacks(registry).packs.isEmpty());
        QVERIFY(!registry.nodeType("io.example.tools/say"));
        QVERIFY(!TrustStore().isTrusted(kSay, "1.0.0"));
        QVERIFY(!isInheritedPack(kSay, "1.0.0"));
        // The Default session and an anonymous root inherit nothing either.
        QVERIFY(setFugueHome(path("p/bare"), nullptr));
        QVERIFY(inheritedPackPaths().isEmpty());
    }

    void anOwnPackWithTheSameIdAndMajorHidesTheInheritedOne() {
        QVERIFY(writeSayPack(basePacks(), "default", "1.2.0"));
        enterProject({"packs"});
        QVERIFY(writeSayPack(ownPacks(), "mine", "1.0.0"));
        Registry registry;
        const PackReport report = loadPacks(registry, packSearchPaths(), inheritedPackPaths());
        QCOMPARE(idsOf(report), (QStringList{"io.example.tools 1.0.0", "io.example.tools 1.2.0 D hidden"}));
        QCOMPARE(registry.packVersion(kSay), QString("1.0.0"));
        QVERIFY(!report.hasProblems()); // hiding is not a problem
        QVERIFY(!isInheritedPack(kSay, "1.2.0"));
        QVERIFY(inheritedPacks().isEmpty());
        // Another major of the same id is not hidden.
        QVERIFY(writeSayPack(path("stage"), "default2", "2.0.0"));
        QVERIFY(QDir().rename(path("stage/io.example.tools"), basePacks() + "/io.example.tools@2"));
        Registry second;
        QCOMPARE(idsOf(loadPacks(second, packSearchPaths(), inheritedPackPaths())).size(), 3);
        QVERIFY(second.hasPackMajor(kSay, 2));
    }

    // Trust

    void theDefaultsTrustIsHonouredForTheExactInheritedVersionOnly() {
        QVERIFY(writeSayPack(basePacks(), "default", "1.0.0"));
        trustInDefault(kSay, "1.0.0");
        trustInDefault(kSay, "9.9.9"); // a version nothing here has
        enterProject({"packs"});
        const TrustStore trust;
        QVERIFY(trust.isTrusted(kSay, "1.0.0"));
        QVERIFY(trust.isTrustedByDefault(kSay, "1.0.0"));
        QVERIFY(!trust.isTrusted(kSay, "1.1.0")); // another version asks again
        QVERIFY(!trust.isTrusted(kSay, "9.9.9")); // not an inherited pack at all
        QVERIFY(!QFileInfo::exists(TrustStore::defaultPath())); // read-only: nothing written in the project either

        // A pack of the project's own with that id and version is not trusted by the Default's say-so.
        QVERIFY(writeSayPack(ownPacks(), "mine", "1.0.0"));
        QVERIFY(!trust.isTrusted(kSay, "1.0.0"));

        // The Default's trust of a pack the project does not inherit is not honoured.
        enterProject({"models"}, "p/closed");
        QVERIFY(!TrustStore().isTrusted(kSay, "1.0.0"));
    }

    void aWorkflowUsingAnInheritedTrustedPackNeedsNoPrompt() {
        QVERIFY(writeSayPack(basePacks(), "default"));
        trustInDefault(kSay, "1.0.0");
        enterProject({"packs"});
        Registry registry;
        loadInstalledPacks(registry);
        const Result<Document> document = Document::load(workflowUsing("io.example.tools/say"));
        QVERIFY(document.ok());
        QVERIFY(TrustStore().untrusted(*document, registry).isEmpty());
    }

    void revokingTrustThatIsTheDefaultsIsRefusedAndNamesWhereToChangeIt() {
        QVERIFY(writeSayPack(basePacks(), "default"));
        trustInDefault(kSay, "1.0.0");
        enterProject({"packs"});
        TrustStore trust;
        for (const Errors &errors : {trust.revoke(kSay, "1.0.0"), trust.revoke(kSay)}) {
            QVERIFY(!errors.isEmpty());
            QCOMPARE(errors.first().code, QString("usage"));
            QVERIFY(errors.first().hint.contains("Default project"));
        }
        QVERIFY(trust.isTrusted(kSay, "1.0.0"));
        QVERIFY(TrustStore(baseTrust()).isTrusted(kSay, "1.0.0")); // the Default's file is as it was
        // Trust of the project's own is its own to revoke.
        QVERIFY(writeSayPack(ownPacks(), "mine"));
        QVERIFY(trust.trust(kSay, "1.0.0").isEmpty());
        QVERIFY(trust.revoke(kSay, "1.0.0").isEmpty());
        QVERIFY(!trust.isTrusted(kSay, "1.0.0")); // the own copy hides the inherited pack, so the Default's trust is not its
    }

    // Writes

    void removeUpdateAndTrustOfAnInheritedPackAreRefusedWithAMessageNamingTheDefault() {
        QVERIFY(writeSayPack(basePacks(), "default"));
        enterProject({"packs"});
        TrustStore trust;
        const Result<PackSummary> removed = removePack(kSay, ownPacks(), trust, yes());
        QVERIFY(!removed.ok());
        QCOMPARE(removed.errors().first().code, QString("usage"));
        QVERIFY(removed.errors().first().message.contains("inherits from the Default project"));
        QVERIFY(removed.errors().first().hint.contains("Default project"));
        QVERIFY(QFileInfo(basePacks() + "/io.example.tools/pack.json").exists());

        const Result<QList<PackAdded>> updated = updatePacks(kSay, {}, ownPacks(), trust, yes());
        QVERIFY(!updated.ok());
        QCOMPARE(updated.errors().first().code, QString("usage"));

        // Trusting what the Default has not trusted is refused too; the Default's own decision is not ours to make.
        const Result<PackSummary> trusted = trustPack(kSay, ownPacks(), trust, yes());
        QVERIFY(!trusted.ok());
        QCOMPARE(trusted.errors().first().code, QString("usage"));
        QVERIFY(!QFileInfo::exists(TrustStore::defaultPath()));

        // Once the Default has trusted that version, trusting here is a no-op, not an error.
        trustInDefault(kSay, "1.0.0");
        QVERIFY(trustPack(kSay, ownPacks(), trust, yes()).ok());

        // An id nothing has is still unknown_pack.
        QCOMPARE(removePack("io.example.none", ownPacks(), trust, yes()).errors().first().code, QString("unknown_pack"));
    }

    void anInstallGoesToTheProjectsOwnPacksFolderEvenOverAnInheritedId() {
        QVERIFY(writeSayPack(basePacks(), "default", "1.2.0"));
        enterProject({"packs"});
        const QString source = path("src");
        QVERIFY(writeSayPack(source, "mine", "1.0.0"));
        TrustStore trust;
        const Result<PackAdded> added = addPack(source + "/io.example.tools", {}, packsDir(), trust, yes());
        QVERIFY2(added.ok(), added.ok() ? "" : qPrintable(added.errors().first().message));
        QCOMPARE(added->pack.folder, ownPacks() + "/io.example.tools");
        QVERIFY(QFileInfo::exists(ownPacks() + "/io.example.tools/pack.json"));
        // It hides the inherited one, which is untouched.
        Registry registry;
        loadInstalledPacks(registry);
        QCOMPARE(registry.packVersion(kSay), QString("1.0.0"));
        QVERIFY(QFileInfo::exists(basePacks() + "/io.example.tools/pack.json"));
        // Now it is the project's own to remove.
        QVERIFY(removePack(kSay, packsDir(), trust, yes()).ok());
        Registry again;
        loadInstalledPacks(again);
        QCOMPARE(again.packVersion(kSay), QString("1.2.0")); // the inherited one shows again
    }

    // The registry reloader

    void theReloaderRereadsProjectJsonWhenItChanges() {
        QVERIFY(writeSayPack(basePacks(), "default"));
        enterProject({"models"}); // not packs yet
        Registry registry;
        const auto fill = [](Registry &into) { loadInstalledPacks(into); };
        fill(registry);
        QVERIFY(!registry.nodeType("io.example.tools/say"));
        RegistryReloader reloader(&registry, fill);
        reloader.setSettleMs(50);
        QSignalSpy reloaded(&reloader, &RegistryReloader::reloaded);
        ProjectManifest manifest;
        manifest.name = "Acme";
        manifest.inherits = {"models", "packs"};
        QVERIFY(writeProject(path("p/acme"), manifest).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(registry.nodeType("io.example.tools/say"), 5000);
        QVERIFY(reloaded.count() >= 1);
        // And back: the inherited pack goes again.
        manifest.inherits = {"models"};
        QVERIFY(writeProject(path("p/acme"), manifest).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!registry.nodeType("io.example.tools/say"), 5000);
    }

    // Templates

    void inheritedTemplatesAreListedReadOnlyAndASaveMakesAProjectCopy() {
        const QString documents = path("home/Documents/Fugue/templates");
        QVERIFY(writeFile(documents + "/city/template.json", R"({"title":"City","category":"mine"})"));
        QVERIFY(writeFile(documents + "/city/workflow.fugue", workflowUsing("io.example.tools/say")));
        QVERIFY(writeFile(documents + "/city/inputs/a.png", "png"));
        QVERIFY(writeFile(documents + "/loose.fugue", workflowUsing("io.example.tools/say")));
        enterProject({"templates"});
        QVERIFY(setenv("FUGUE_TEMPLATES", path("none").toLocal8Bit().constData(), 1) == 0);
        QVERIFY(writeSayPack(path("tp"), "x"));
        Registry registry;
        loadPacks(registry, {path("tp")});
        const TemplateLibrary library(TemplateLibrary::standardRoots(), &registry);
        QCOMPARE(TemplateLibrary::defaultOwnFolder(), documents);
        QCOMPARE(TemplateLibrary::ownFolder(), path("p/acme/documents/templates"));
        QList<Template> listed = library.list();
        QStringList shown;
        for (const Template &t : listed)
            shown << QStringLiteral("%1%2").arg(t.id, t.inherited ? " D" : "");
        shown.sort();
        QCOMPARE(shown, (QStringList{"city D", "loose D"}));

        const QStringList before = snapshot(path("base"), path("home/Documents"));
        QVERIFY(library.find("city"));
        const Template city = *library.find("city");
        QVERIFY(library.saveOwn(city, workflowUsing("io.example.tools/say") + "\n").isEmpty());
        QCOMPARE(snapshot(path("base"), path("home/Documents")), before); // the Default's copy is untouched
        const QString copy = path("p/acme/documents/templates/city");
        QVERIFY(QFileInfo::exists(copy + "/template.json"));
        QVERIFY(QFileInfo::exists(copy + "/inputs/a.png"));
        QVERIFY(QFileInfo::exists(copy + "/workflow.fugue"));
        const TemplateLibrary after(TemplateLibrary::standardRoots(), &registry);
        QVERIFY(!after.find("city")->inherited); // the copy hides the inherited one
        QCOMPARE(after.find("city")->folder, copy);
        QCOMPARE(after.list().size(), 2);
        // Saving again is in place, a loose inherited file becomes a folder with a title.
        QVERIFY(after.saveOwn(*after.find("city"), workflowUsing("io.example.tools/say") + "\n\n").isEmpty());
        QVERIFY(after.saveOwn(*after.find("loose"), {}).isEmpty());
        QVERIFY(QFileInfo::exists(path("p/acme/documents/templates/loose/template.json")));
        QCOMPARE(snapshot(path("base"), path("home/Documents")), before);
        // Without `templates` in inherits none of it is listed.
        enterProject({"packs"}, "p/other");
        QVERIFY(TemplateLibrary(TemplateLibrary::standardRoots(), &registry).list().isEmpty());
    }

    // Prompts

    void inheritedPromptTemplatesAreReadOnlyAndAnOwnFileHidesThem() {
        const auto file = [](const QString &id, const QString &label) {
            return QJsonDocument(QJsonObject{{"prompt_templates", 1},
                                             {"templates", QJsonArray{QJsonObject{{"id", id}, {"label", label}, {"family", "image"}, {"system", "Write."}}}}})
                .toJson();
        };
        QVERIFY(writeFile(path("base/data/fugue/prompts/a.json"), file("acme-style", "Default style")));
        QVERIFY(writeFile(path("base/data/fugue/prompts/b.json"), file("shared-style", "Default shared")));
        enterProject({"prompts"});
        QVERIFY(writeFile(path("p/acme/data/fugue/prompts/own.json"), file("shared-style", "Own shared")));
        const PromptTemplates templates = PromptTemplates::load();
        QVERIFY(templates.find("acme-style"));
        QVERIFY(templates.find("acme-style")->inherited);
        QCOMPARE(templates.find("shared-style")->label, QString("Own shared")); // an own file with the id hides it
        QVERIFY(!templates.find("shared-style")->inherited);
        QVERIFY(templates.find("detailed")); // Fugue's own are still there
        QVERIFY(!templates.find("detailed")->inherited);
        enterProject({"packs"}, "p/other");
        QVERIFY(!PromptTemplates::load().find("acme-style")); // not inherited: not read
    }

    // The Default's providers and keys never cross

    void providersAndKeysOfTheDefaultAreNeverRead() {
        QVERIFY(writeFile(path("base/config/fugue/providers.json"),
                          R"({"providers":[{"name":"secret","type":"fugue-helper","address":"x:1","key":"hunter2"}]})"));
        enterProject({"models", "packs", "templates", "prompts", "preferences"});
        QCOMPARE(providersPath(), path("p/acme/config/fugue/providers.json"));
        const Result<QList<ProviderInstance>> loaded = loadProviderInstances(providersPath());
        for (const ProviderInstance &instance : loaded.value())
            QVERIFY2(instance.name != "secret" && instance.key != "hunter2", "the Default's provider crossed into the project");
    }

    // The Default is never changed

    void theDefaultsFoldersAreUnchangedByWhatAProjectDoes() {
        QVERIFY(writeSayPack(basePacks(), "default"));
        trustInDefault(kSay, "1.0.0");
        QVERIFY(writeFile(path("base/config/fugue/settings.json"), "{}"));
        QVERIFY(writeFile(path("base/data/fugue/prompts/a.json"), "{}"));
        QVERIFY(writeFile(path("home/Documents/Fugue/templates/t/template.json"), R"({"title":"T"})"));
        QVERIFY(writeFile(path("home/Documents/Fugue/templates/t/workflow.fugue"), workflowUsing("io.example.tools/say")));
        const QStringList before = snapshot(path("base"), path("home/Documents"));
        QVERIFY(!before.isEmpty());

        enterProject({"models", "packs", "templates", "prompts", "preferences"});
        Registry registry;
        loadInstalledPacks(registry);
        QVERIFY(registry.nodeType("io.example.tools/say"));
        QVERIFY(qputenv("FUGUE_TEMPLATES", path("none").toLocal8Bit()));
        TrustStore trust;
        QVERIFY(trust.isTrusted(kSay, "1.0.0"));
        (void)trust.revoke(kSay);
        (void)removePack(kSay, packsDir(), trust, yes());
        (void)updatePacks({}, {}, packsDir(), trust, yes());
        (void)trustPack(kSay, packsDir(), trust, yes());
        (void)PromptTemplates::load();
        const TemplateLibrary library(TemplateLibrary::standardRoots(), &registry);
        QVERIFY(library.find("t"));
        QVERIFY(library.saveOwn(*library.find("t"), workflowUsing("io.example.tools/say") + " ").isEmpty());
        QCOMPARE(snapshot(path("base"), path("home/Documents")), before);
    }
};

QTEST_GUILESS_MAIN(TestProjectPacks)
#include "test_project_packs.moc"
