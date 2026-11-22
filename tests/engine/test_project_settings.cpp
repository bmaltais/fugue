// Layered settings of a project (spec 1.4.2, epic #204 step 2 slice 4): what a project takes from the
// Default project's settings.json, models folder, catalog and keymap, which keys never cross, and that
// nothing is ever written to the Default's folders. Seam: engine/settings.h, model_library.h and
// model_catalog.h in a real project root (setFugueHome) beside a Default made of temporary folders.
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
#include <QTemporaryDir>
#include <QtTest>

#include "engine/file_hashes.h"
#include "engine/fugue_home.h"
#include "engine/hash.h"
#include "engine/json.h"
#include "engine/model_catalog.h"
#include "engine/model_library.h"
#include "engine/paths.h"
#include "engine/project.h"
#include "engine/registry.h"
#include "engine/settings.h"

using namespace fugue;

namespace {

const char *const kVariables[] = {"FUGUE_HOME", "FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME",
                                  "FUGUE_BASE_XDG_STATE_HOME", "FUGUE_BASE_XDG_CACHE_HOME", "FUGUE_BASE_XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
                                  "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "TMPDIR", "WAYLAND_DISPLAY",
                                  "DBUS_SESSION_BUS_ADDRESS", "CUDA_CACHE_PATH", "__GL_SHADER_DISK_CACHE_PATH",
                                  "PIPEWIRE_RUNTIME_DIR", "PULSE_RUNTIME_PATH"};

void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

// Every file under the Default's three folders with its content, for a before and after.
QStringList snapshot(const QString &base) {
    QStringList lines;
    for (const char *folder : {"config", "data", "state"}) {
        QDirIterator it(base + QLatin1Char('/') + QLatin1String(folder), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                        QDirIterator::Subdirectories);
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

QJsonObject defaultSettings(const QString &models) {
    return QJsonObject{{"navigation", "standard"},
                       {"corner_radius", 12},
                       {"hover_magnifier", true},
                       {"new_seed", "fixed"},
                       {"language", "da"},
                       {"model_paths", QJsonArray{models + "/shared", models + "/both"}},
                       {"model_folder_kinds", QJsonObject{{models + "/shared", "fugue/lora"}}},
                       // never inherited:
                       {"output_folder", "/srv/default-out"},
                       {"inputs_folder", "/srv/default-in"},
                       {"download_folder", models + "/shared"},
                       {"pack_paths", QJsonArray{"/srv/default-packs"}},
                       {"cache_results_gb", 77},
                       {"cache_thumbnails_gb", 7},
                       {"runs_kept", 3}};
}

} // namespace

class TestProjectSettings : public QObject {
    Q_OBJECT

    QHash<QByteArray, QByteArray> m_saved;
    QSet<QByteArray> m_unset;
    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }
    QString baseConfig() const { return path("base/config/fugue"); }
    QString baseData() const { return path("base/data/fugue"); }

    // A project root that inherits `items`, entered as the session's own root.
    void enterProject(const QStringList &items, const QString &name = QStringLiteral("p/acme")) {
        ProjectManifest manifest;
        manifest.name = "Acme";
        manifest.inherits = items;
        QVERIFY(QDir().mkpath(path(name)));
        QVERIFY(writeProject(path(name), manifest).isEmpty());
        QVERIFY(setFugueHome(path(name), nullptr));
    }

    void writeDefault(const QJsonObject &settings) { write(baseConfig() + "/settings.json", writeJson(settings)); }

private slots:
    void init() {
        for (const char *variable : kVariables) {
            if (qEnvironmentVariableIsSet(variable))
                m_saved.insert(variable, qgetenv(variable));
            else
                m_unset.insert(variable);
            qunsetenv(variable);
        }
        qputenv("XDG_CONFIG_HOME", path("base/config").toLocal8Bit());
        qputenv("XDG_DATA_HOME", path("base/data").toLocal8Bit());
        qputenv("XDG_STATE_HOME", path("base/state").toLocal8Bit());
        qputenv("XDG_RUNTIME_DIR", path("base/run").toLocal8Bit());
        QDir().mkpath(path("base/run"));
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
    }

    // Preferences

    void aPreferenceIsTheProjectsThenTheDefaultsThenTheBuiltIn() {
        writeDefault(defaultSettings(path("models")));
        enterProject({"preferences"});
        write(configDir() + "/settings.json", R"({"navigation": "legacy", "runs_kept": 5})");
        const EffectiveSettings effective = effectiveSettings();
        QCOMPARE(effective.settings.navigation, QString("legacy")); // own wins over the Default's standard
        QCOMPARE(effective.origins.value("navigation"), SettingOrigin::Own);
        QCOMPARE(effective.settings.cornerRadius, 12); // inherited
        QCOMPARE(effective.origins.value("corner_radius"), SettingOrigin::Default);
        QVERIFY(effective.settings.hoverMagnifier);
        QCOMPARE(effective.settings.newSeed, QString("fixed"));
        QCOMPARE(effective.settings.language, QString("da"));
        QCOMPARE(effective.settings.embedWorkflow, QString("full")); // set nowhere
        QCOMPARE(effective.origins.value("embed_workflow"), SettingOrigin::BuiltIn);
        QCOMPARE(settingOriginName(SettingOrigin::Default), QString("inherited from Default"));
        QCOMPARE(currentSettings().cornerRadius, 12);
    }

    void keysThatAreNeverInheritableStayOwnEvenWhenTheDefaultHasThem() {
        writeDefault(defaultSettings(path("models")));
        enterProject({"models", "preferences", "packs", "templates", "prompts"});
        const EffectiveSettings effective = effectiveSettings();
        const Settings &s = effective.settings;
        QVERIFY(s.outputFolder.isEmpty());
        QVERIFY(s.inputsFolder.isEmpty());
        QVERIFY(s.downloadFolder.isEmpty());
        QVERIFY(s.packPaths.isEmpty());
        QCOMPARE(s.cacheResultsGb, 10.0);
        QCOMPARE(s.cacheThumbnailsGb, 1.0);
        QCOMPARE(s.runsKept, 20);
        for (const char *key : {"output_folder", "inputs_folder", "download_folder", "pack_paths", "cache_results_gb",
                                "cache_thumbnails_gb", "runs_kept"}) {
            QCOMPARE(settingClass(key), SettingClass::Own);
            QCOMPARE(effective.origins.value(key), SettingOrigin::BuiltIn);
        }
        // The folders that follow follow the project, not the Default.
        QCOMPARE(outputDir(), path("p/acme/outputs"));
    }

    void aProjectWithoutInheritsSeesNothingOfTheDefault() {
        writeDefault(defaultSettings(path("models")));
        enterProject({});
        const EffectiveSettings effective = effectiveSettings();
        QCOMPARE(effective.settings.navigation, QString("legacy"));
        QCOMPARE(effective.settings.cornerRadius, 8);
        QVERIFY(effective.settings.modelPaths.isEmpty());
        QVERIFY(!effective.settings.inheritsModels);
        for (auto it = effective.origins.begin(); it != effective.origins.end(); ++it)
            QCOMPARE(it.value(), SettingOrigin::BuiltIn);
    }

    void aClassNotListedIsNotRead() {
        writeDefault(defaultSettings(path("models")));
        enterProject({"models"}); // not preferences
        QCOMPARE(effectiveSettings().settings.cornerRadius, 8);
        QCOMPARE(effectiveSettings().settings.modelPaths.size(), 2);
    }

    void theDefaultSessionAndAnAnonymousRootSeeNoLayering() {
        writeDefault(defaultSettings(path("models")));
        QCOMPARE(effectiveSettings().origins.value("corner_radius"), SettingOrigin::Own); // its own file
        QVERIFY(!effectiveSettings().settings.inheritsModels);
        QVERIFY(setFugueHome(path("p/bare"), nullptr)); // no project.json
        QCOMPARE(currentSettings().cornerRadius, 8);
        QVERIFY(currentSettings().modelPaths.isEmpty());
    }

    // Lists

    void listsAreOwnFirstThenInheritedWithoutDuplicates() {
        writeDefault(defaultSettings(path("models")));
        enterProject({"models"});
        write(configDir() + "/settings.json",
              writeJson(QJsonObject{{"model_paths", QJsonArray{path("models/mine"), path("models/both")}}}));
        const Settings s = effectiveSettings().settings;
        QCOMPARE(s.modelPaths, (QStringList{path("models/mine"), path("models/both"), path("models/shared")}));
        QCOMPARE(s.inheritedModelPaths, (QStringList{path("models/shared")})); // "both" is the project's own
        QCOMPARE(effectiveSettings().origins.value("model_paths"), SettingOrigin::Own);
        // The folder kinds of the Default apply to its paths.
        QCOMPARE(s.modelFolderKinds.value(path("models/shared")), QString("fugue/lora"));
    }

    void modelRootsAreTaggedAndTheDefaultsOwnModelsFolderComesAfterTheProjects() {
        writeDefault(defaultSettings(path("models")));
        enterProject({"models"});
        write(configDir() + "/settings.json", writeJson(QJsonObject{{"model_paths", QJsonArray{path("models/both")}}}));
        const QList<SearchPath> roots = ModelLibrary::taggedRoots(currentSettings());
        QStringList shown;
        for (const SearchPath &root : roots)
            shown << (root.inherited ? "D " : "O ") + root.path;
        QCOMPARE(shown, (QStringList{"O " + path("p/acme/data/fugue/models"), "O " + path("models/both"),
                                     "D " + path("base/data/fugue/models"), "D " + path("models/shared")}));
        const QList<SearchPath> plain = taggedSearchPaths("/a", {"/b", "/a"}, {"/b", "/c", "relative"});
        QCOMPARE(plain.size(), 3); // a, b (own: the project's naming wins), c
        QVERIFY(!plain.at(1).inherited);
        QVERIFY(plain.at(2).inherited);
    }

    // Writing

    void changeSettingWritesOnlyTheProjectsFile() {
        writeDefault(defaultSettings(path("models")));
        enterProject({"models", "preferences"});
        QFile first(baseConfig() + "/settings.json");
        QVERIFY(first.open(QIODevice::ReadOnly));
        const QByteArray before = first.readAll();
        QVERIFY(changeSetting(settingsPath(), "navigation", "legacy").isEmpty());
        QVERIFY(changeSetting(settingsPath(), "corner_radius", 4).isEmpty());
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{path("models/mine")}).isEmpty());
        const SettingsFile own = readSettings(settingsPath());
        QCOMPARE(own.settings.cornerRadius, 4);
        QCOMPARE(own.settings.modelPaths, (QStringList{path("models/mine")})); // the file holds only its own entries
        QFile base(baseConfig() + "/settings.json");
        QVERIFY(base.open(QIODevice::ReadOnly));
        QCOMPARE(base.readAll(), before);
        QCOMPARE(effectiveSettings().settings.cornerRadius, 4);
        QCOMPARE(effectiveSettings().settings.inheritedModelPaths.size(), 2);
    }

    void aProjectCanSetAPreferenceBackToTheBuiltInValueOverTheDefaults() {
        writeDefault(defaultSettings(path("models")));
        enterProject({"preferences"});
        QVERIFY(changeSetting(settingsPath(), "corner_radius", 8).isEmpty()); // the built-in, but the Default says 12
        QCOMPARE(readSettings(settingsPath()).document.value("corner_radius").toInt(), 8);
        QCOMPARE(effectiveSettings().settings.cornerRadius, 8);
        QCOMPARE(effectiveSettings().origins.value("corner_radius"), SettingOrigin::Own);
        // Without a different inherited value the default is still not written.
        QVERIFY(changeSetting(settingsPath(), "embed_workflow", "full").isEmpty());
        QVERIFY(!readSettings(settingsPath()).document.contains("embed_workflow"));
    }

    void aProjectCanSetTheLanguageBackToFollowOmarchyOverTheDefaults() {
        writeDefault(defaultSettings(path("models"))); // language da
        enterProject({"preferences"});
        // Follow Omarchy is an absent key in the Default, but in a project it must shadow: it writes "".
        QVERIFY(changeSetting(settingsPath(), "language", settingUnsetValue("language")).isEmpty());
        QVERIFY(readSettings(settingsPath()).problems.isEmpty());
        QCOMPARE(readSettings(settingsPath()).document.value("language"), QJsonValue(QString()));
        QCOMPARE(effectiveSettings().settings.language, QString());
        QCOMPARE(effectiveSettings().origins.value("language"), SettingOrigin::Own);
        // Removing the key (undefined) is the way back to the Default's language.
        QVERIFY(changeSetting(settingsPath(), "language", QJsonValue::Undefined).isEmpty());
        QVERIFY(!readSettings(settingsPath()).document.contains("language"));
        QCOMPARE(effectiveSettings().settings.language, QString("da"));
    }

    void followOmarchyIsNotWrittenWhenThereIsNothingToShadow() {
        enterProject({"preferences"}); // the Default has no language
        QVERIFY(changeSetting(settingsPath(), "language", settingUnsetValue("language")).isEmpty());
        QVERIFY(!readSettings(settingsPath()).document.contains("language"));
        writeDefault(defaultSettings(path("models")));
        enterProject({}); // inherits nothing: the Default's language is not seen
        QVERIFY(changeSetting(settingsPath(), "language", settingUnsetValue("language")).isEmpty());
        QVERIFY(!readSettings(settingsPath()).document.contains("language"));
    }

    // Models

    void inheritedModelRootsAreReadOnlyAndNeverADownloadTarget() {
        writeDefault(defaultSettings(path("models")));
        enterProject({"models"});
        write(configDir() + "/settings.json", writeJson(QJsonObject{{"download_folder", path("models/shared")}}));
        Registry registry;
        const ModelLibrary library = ModelLibrary::current(&registry);
        const QString inherited = path("models/shared");
        QVERIFY(library.roots().contains(inherited));
        QVERIFY(library.isInherited(inherited));
        QVERIFY(library.isInherited(path("base/data/fugue/models")));
        QVERIFY(!library.isInherited(path("p/acme/data/fugue/models")));
        // The download folder names an inherited root: downloads still go to an own one.
        QCOMPARE(library.downloadRoot(), path("p/acme/data/fugue/models"));
        const std::optional<Error> refusal = library.writeRefusal(inherited + "/loras/x.safetensors");
        QVERIFY(refusal.has_value());
        QVERIFY(refusal->message.contains("inherits from the Default project"));
        QVERIFY(refusal->hint.contains("Default project"));
        QVERIFY(!library.writeRefusal(path("p/acme/data/fugue/models/x.safetensors")).has_value());
        QVERIFY(!library.writeRefusal("/somewhere/else").has_value());
    }

    void inheritedModelFilesAreListed() {
        writeDefault(defaultSettings(path("models")));
        enterProject({"models"});
        write(baseData() + "/models/upscaler/a.pth", "x");
        write(path("p/acme/data/fugue/models/upscaler/b.pth"), "y");
        Registry registry;
        DeclaredPortType declared;
        declared.type = *PortType::parse("fugue/upscaler");
        declared.kind = PortKind::Resource;
        declared.name = "Upscaler";
        declared.extensions = {".pth"};
        QVERIFY(registry.add(declared).isEmpty());
        const QList<ModelFile> files = ModelLibrary::current(&registry).list(declared.type);
        QStringList names;
        for (const ModelFile &file : files)
            names << file.name;
        QCOMPARE(names, (QStringList{"b.pth", "a.pth"})); // own first
    }

    void theDefaultsCatalogIsReadUnderTheProjects() {
        const auto catalog = [](const QString &id, const QString &title) {
            return QJsonDocument(QJsonObject{{"catalog", 1},
                                             {"models", QJsonArray{QJsonObject{{"id", id}, {"resource", "fugue/upscale-model"}, {"title", title},
                                                                              {"file", "a.safetensors"}, {"url", "https://example.org/a"},
                                                                              {"size", 1}, {"sha256", QString(64, 'a')}, {"scale", 4},
                                                                              {"architecture", "ESRGAN"}, {"license", "MIT"}}}}})
                .toJson();
        };
        write(baseData() + "/catalog/d.json", catalog("shared", "From Default"));
        write(baseData() + "/catalog/e.json", catalog("only-default", "Only"));
        enterProject({"models"});
        write(dataDir() + "/catalog/own.json", catalog("shared", "From project"));
        ModelCatalog layered = ModelCatalog::load({}, {});
        QVERIFY(layered.find("only-default") != nullptr);
        QCOMPARE(layered.find("shared")->title, QString("From project")); // own wins
        enterProject({}, "p/closed");
        // A project that does not inherit `models` has only its own entries.
        QDir(dataDir() + "/catalog").removeRecursively();
        QVERIFY(ModelCatalog::load({}, {}).find("only-default") == nullptr);
    }

    // Keybindings

    void theDefaultsKeymapIsUsedUntilTheProjectHasItsOwn() {
        write(baseConfig() + "/keybindings.json", "{}");
        enterProject({"preferences"});
        QCOMPARE(effectiveKeymapPath(), baseConfig() + "/keybindings.json");
        write(configDir() + "/keybindings.json", "{}");
        QCOMPARE(effectiveKeymapPath(), configDir() + "/keybindings.json");
        QFile::remove(configDir() + "/keybindings.json");
        enterProject({"models"}, "p/other");
        QCOMPARE(effectiveKeymapPath(), configDir() + "/keybindings.json"); // preferences not inherited
    }

    // Remembered hashes of inherited model files (slice 7)

    // The Default session remembers the hash of a model file, as its state folder holds it.
    QString defaultRemembers(const QString &file, const QString &hash) {
        FileHashes defaults(path("base/state/fugue/hashes.json"));
        defaults.remember(file, hash);
        return defaults.save() ? QString() : QStringLiteral("save failed");
    }

    void aProjectReusesTheDefaultsHashesOfInheritedModelFiles() {
        const QString planted = QStringLiteral("sha256:0000000000000000000000000000000000000000000000000000000000000000");
        writeDefault(defaultSettings(path("models")));
        write(baseData() + "/models/checkpoints/a.safetensors", "aaa");
        write(path("models/shared/b.safetensors"), "bbb");
        write(path("models/mine/c.safetensors"), "ccc");
        QVERIFY(defaultRemembers(baseData() + "/models/checkpoints/a.safetensors", planted).isEmpty());
        QVERIFY(defaultRemembers(path("models/shared/b.safetensors"), planted).isEmpty());
        QVERIFY(defaultRemembers(path("models/mine/c.safetensors"), planted).isEmpty());
        enterProject({"models"});
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{path("models/mine")}).isEmpty());
        FileHashes hashes(FileHashes::defaultPath());
        QCOMPARE(hashes.known(baseData() + "/models/checkpoints/a.safetensors"), planted); // the Default's models folder
        QCOMPARE(hashes.known(path("models/shared/b.safetensors")), planted);              // the Default's model_paths
        QCOMPARE(hashes.known(path("models/mine/c.safetensors")), QString());              // the project's own root
    }

    void aProjectThatDoesNotInheritModelsSeesNoneOfTheDefaultsHashes() {
        const QString planted = QStringLiteral("sha256:0000000000000000000000000000000000000000000000000000000000000000");
        writeDefault(defaultSettings(path("models")));
        write(baseData() + "/models/checkpoints/a.safetensors", "aaa");
        QVERIFY(defaultRemembers(baseData() + "/models/checkpoints/a.safetensors", planted).isEmpty());
        for (const QStringList &items : {QStringList{"packs", "preferences"}, QStringList{}}) {
            enterProject(items);
            FileHashes hashes(FileHashes::defaultPath());
            QCOMPARE(hashes.known(baseData() + "/models/checkpoints/a.safetensors"), QString());
            QCOMPARE(hashes.hash(baseData() + "/models/checkpoints/a.safetensors"), sha256("aaa"));
        }
    }

    void aProjectsOwnHashesGoToItsOwnStoreAndTheDefaultsIsUntouched() {
        const QString planted = QStringLiteral("sha256:0000000000000000000000000000000000000000000000000000000000000000");
        write(baseData() + "/models/checkpoints/a.safetensors", "aaa");
        QVERIFY(defaultRemembers(baseData() + "/models/checkpoints/a.safetensors", planted).isEmpty());
        write(path("models/mine/c.safetensors"), "ccc");
        const QStringList before = snapshot(path("base"));
        enterProject({"models"});
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{path("models/mine")}).isEmpty());
        {
            FileHashes hashes(FileHashes::defaultPath());
            QCOMPARE(hashes.hash(path("models/mine/c.safetensors")), sha256("ccc"));
            QCOMPARE(hashes.hash(baseData() + "/models/checkpoints/a.safetensors"), planted);
            QVERIFY(hashes.save());
        }
        QCOMPARE(snapshot(path("base")), before);
        QVERIFY(QFileInfo::exists(stateDir() + "/hashes.json"));
        QVERIFY(stateDir().startsWith(path("p/acme")));
        // The project's file holds its own hash and not a copy of the Default's.
        QCOMPARE(FileHashes(stateDir() + "/hashes.json", FileHashes::Inherited()).known(path("models/mine/c.safetensors")), sha256("ccc"));
        QCOMPARE(FileHashes(stateDir() + "/hashes.json", FileHashes::Inherited()).known(baseData() + "/models/checkpoints/a.safetensors"), QString());
    }

    void aHashTheDefaultMadeStaleIsNotTrustedByAProject() {
        const QString planted = QStringLiteral("sha256:0000000000000000000000000000000000000000000000000000000000000000");
        const QString file = baseData() + "/models/checkpoints/a.safetensors";
        write(file, "aaa");
        QVERIFY(defaultRemembers(file, planted).isEmpty());
        write(file, "aaaa");
        enterProject({"models"});
        QCOMPARE(FileHashes(FileHashes::defaultPath()).hash(file), sha256("aaaa"));
    }

    // The Default is never changed

    void theDefaultsFoldersAreUnchangedByWhatAProjectDoes() {
        writeDefault(defaultSettings(path("models")));
        write(baseData() + "/models/loras/x.safetensors", "x");
        write(baseData() + "/catalog/d.json", "{}");
        write(baseConfig() + "/keybindings.json", "{}");
        write(baseConfig() + "/projects.json", R"({"format":1,"projects":[]})");
        write(baseData() + "/../state/fugue/trust.json", "{}");
        const QStringList before = snapshot(path("base"));
        QVERIFY(!before.isEmpty());
        enterProject({"models", "preferences"});
        QVERIFY(changeSetting(settingsPath(), "corner_radius", 3).isEmpty());
        QVERIFY(changeSetting(settingsPath(), "corner_radius", 8).isEmpty());
        QVERIFY(changeSetting(settingsPath(), "model_paths", QJsonArray{path("models/mine")}).isEmpty());
        QVERIFY(changeSetting(settingsPath(), "navigation", QJsonValue::Undefined).isEmpty());
        Registry registry;
        ModelLibrary library = ModelLibrary::current(&registry);
        (void)library.roots();
        (void)ModelCatalog::load({}, {});
        (void)effectiveSettings();
        (void)effectiveKeymapPath();
        (void)outputDir();
        QCOMPARE(snapshot(path("base")), before);
    }
};

QTEST_GUILESS_MAIN(TestProjectSettings)
#include "test_project_settings.moc"
