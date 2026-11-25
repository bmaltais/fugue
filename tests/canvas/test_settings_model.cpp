// The settings panel's model (#51): every change is checked in C++, written at once, read
// back by a new model, and applied live. Nothing here touches the owner's real config: the
// files are in a temporary XDG_CONFIG_HOME.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/canvas_model.h"
#include "engine/fugue_home.h"
#include "engine/json.h"
#include "engine/paths.h"
#include "engine/project.h"
#include "engine/providers.h"
#include "engine/providers_file.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "settings/provider_presets.h"
#include "settings/settings_model.h"
#include "theme.h"
#include "translator.h"

using namespace fugue;

class TestSettingsModel : public QObject {
    Q_OBJECT

    QTemporaryDir m_home;

    QString settingsFile() const { return configDir() + "/settings.json"; }
    QString providersFile() const { return configDir() + "/providers.json"; }

    void writeFile(const QString &path, const QByteArray &bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }

private slots:
    void init() {
        // A fresh config folder per test; the model's default paths are under it.
        QDir(m_home.filePath("config")).removeRecursively();
        qputenv("XDG_CONFIG_HOME", m_home.filePath("config").toUtf8());
        QVERIFY(configDir().startsWith(m_home.path()));
    }

    // #379: Settings > Folders' "Download models into" is the banner's old choice with the same key.
    void downloadModelsIntoOneOfTheModelFolders() {
        QTemporaryDir comfy;
        SettingsModel model;
        QCOMPARE(model.downloadFolder(), dataDir() + "/models");
        QCOMPARE(model.downloadFolders().size(), 1);
        QCOMPARE(model.downloadFolders().at(0).toMap().value("title").toString(), QStringLiteral("Fugue's models folder"));
        QCOMPARE(model.addPath("model_paths", comfy.path()), QString());
        QCOMPARE(model.downloadFolders().size(), 2);
        QCOMPARE(model.set("download_folder", comfy.path()), QString());
        QCOMPARE(model.downloadFolder(), comfy.path());
        QCOMPARE(SettingsModel().downloadFolder(), comfy.path());
        // Fugue's own folder is the default: the key leaves the file.
        QCOMPARE(model.set("download_folder", dataDir() + "/models"), QString());
        QCOMPARE(model.downloadFolder(), dataDir() + "/models");
        QFile file(settingsFile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!QJsonDocument::fromJson(file.readAll()).object().contains("download_folder"));
    }

    void aChangeIsWrittenAndANewModelReadsItBack() {
        {
            SettingsModel model;
            QCOMPARE(model.path(), settingsFile());
            QCOMPARE(model.navigation(), QStringLiteral("legacy"));
            QSignalSpy changed(&model, &SettingsModel::changed);
            QCOMPARE(model.set("navigation", "standard"), QString());
            QCOMPARE(model.navigation(), QStringLiteral("standard"));
            QCOMPARE(changed.count(), 1);
        }
        SettingsModel again;
        QCOMPARE(again.navigation(), QStringLiteral("standard"));
    }

    void aBadValueIsRefusedAndNothingIsWritten() {
        SettingsModel model;
        QVERIFY(!model.set("navigation", "sideways").isEmpty());
        QVERIFY(!model.set("language", "xx").isEmpty()); // a code with no catalog
        QVERIFY(!model.set("output_folder", "relative").isEmpty());
        QVERIFY(!model.set("no_such_key", 1).isEmpty());
        QVERIFY(!QFile::exists(settingsFile()));
    }

    void unknownKeysInTheFileStay() {
        writeFile(settingsFile(), R"({"future_setting": [1, 2], "navigation": "standard"})");
        SettingsModel model;
        QCOMPARE(model.set("embed_workflow", "none"), QString());
        const auto document = readJsonObject(settingsFile());
        QCOMPARE(document.value().value("future_setting").toArray().size(), 2);
        QCOMPARE(document.value().value("embed_workflow").toString(), QStringLiteral("none"));
    }

    void aCorruptFileIsShownAndNotWritten() {
        writeFile(settingsFile(), "{oops");
        SettingsModel model;
        QVERIFY(!model.writable());
        QCOMPARE(model.problems().size(), 1);
        QVERIFY(!model.set("navigation", "standard").isEmpty());
        QFile file(settingsFile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("{oops"));
    }

    void foldersShowTheHomeFolderAsATildeAndResetWithEmptyText() {
        SettingsModel model;
        QVERIFY(model.outputFolderIsDefault());
        QCOMPARE(model.set("output_folder", QDir::homePath() + "/Art"), QString());
        QCOMPARE(model.outputFolder(), QStringLiteral("~/Art"));
        QCOMPARE(model.inputsFolder(), QStringLiteral("~/Art/inputs"));
        QVERIFY(!model.outputFolderIsDefault());
        QCOMPARE(outputDir(), QDir::homePath() + "/Art"); // the engine sees it too
        QCOMPARE(model.set("output_folder", "  "), QString());
        QVERIFY(model.outputFolderIsDefault());
        QVERIFY(!readJsonObject(settingsFile()).value().contains("output_folder"));
    }

    void searchPathsAreAddedAndRemoved() {
        SettingsModel model;
        QCOMPARE(model.addPath("model_paths", "~/ComfyUI/models"), QString());
        QCOMPARE(model.addPath("model_paths", "/srv/models"), QString());
        QVERIFY(!model.addPath("model_paths", "~/ComfyUI/models").isEmpty()); // twice
        QVERIFY(!model.addPath("model_paths", "models").isEmpty());
        QCOMPARE(model.modelPaths(), (QStringList{"~/ComfyUI/models", "/srv/models"}));
        QCOMPARE(model.removePath("model_paths", 0), QString());
        QCOMPARE(model.addPath("pack_paths", "/opt/packs"), QString());
        SettingsModel again;
        QCOMPARE(again.modelPaths(), QStringList{"/srv/models"});
        QCOMPARE(again.packPaths(), QStringList{"/opt/packs"});
    }

    void theLanguageAppliesLive() {
        QTemporaryDir omarchy;
        Translator translator(omarchy.filePath("language")); // Omarchy says nothing: English
        SettingsModel model;
        model.attach(&translator, nullptr);
        QCOMPARE(translator.language(), QStringLiteral("en"));
        QCOMPARE(model.set("language", "da"), QString());
        QCOMPARE(translator.language(), QStringLiteral("da"));
        QCOMPARE(model.set("language", ""), QString()); // follow Omarchy again
        QCOMPARE(translator.language(), QStringLiteral("en"));
        QVERIFY(model.languages().size() >= 2);
    }

    void followOmarchyInAProjectShadowsTheDefaultsLanguage() {
        writeFile(baseConfigDir() + "/settings.json", R"({"language": "da"})");
        ProjectManifest manifest;
        manifest.name = "Acme";
        manifest.inherits = {"preferences"};
        const QString root = m_home.filePath("acme");
        QVERIFY(QDir().mkpath(root));
        QVERIFY(writeProject(root, manifest).isEmpty());
        // Entering a project redirects the process's XDG variables: put the whole environment back after.
        QHash<QByteArray, QByteArray> saved;
        for (const char *variable : {"FUGUE_HOME", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME", "FUGUE_BASE_XDG_STATE_HOME",
                                     "FUGUE_BASE_XDG_CACHE_HOME", "FUGUE_BASE_XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
                                     "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "TMPDIR", "CUDA_CACHE_PATH",
                                     "__GL_SHADER_DISK_CACHE_PATH", "PIPEWIRE_RUNTIME_DIR", "PULSE_RUNTIME_PATH"})
            saved.insert(variable, qEnvironmentVariableIsSet(variable) ? qgetenv(variable) : QByteArray("\0unset", 6));
        const auto leave = qScopeGuard([&saved] {
            for (auto it = saved.begin(); it != saved.end(); ++it)
                if (it.value() == QByteArray("\0unset", 6))
                    qunsetenv(it.key().constData());
                else
                    qputenv(it.key().constData(), it.value());
        });
        QVERIFY(setFugueHome(root, nullptr));
        QTemporaryDir omarchy;
        Translator translator(omarchy.filePath("language"));
        SettingsModel model;
        model.attach(&translator, nullptr);
        QCOMPARE(model.language(), QStringLiteral("da")); // the Default's
        QCOMPARE(model.set("language", ""), QString()); // follow Omarchy, in this project
        QCOMPARE(model.language(), QString());
        QCOMPARE(translator.language(), QStringLiteral("en"));
        QCOMPARE(model.origins().value("language").toString(), QStringLiteral("override")); // can be given up
        SettingsModel again; // a window opened on the project later
        QCOMPARE(again.language(), QString());
        QCOMPARE(model.useDefault("language"), QString()); // giving it up shows the Default's again
        QCOMPARE(model.language(), QStringLiteral("da"));
    }

    void newNodesStartAsTheRunSettingsSay() {
        Registry registry;
        nodes::addBuiltinNodeTypes(registry);
        CanvasModel canvas(&registry);
        SettingsModel model;
        model.attach(nullptr, &canvas);
        // Defaults: a randomized seed, the manifest's embed.
        QString id = canvas.addNode("fugue/generate-image", 0, 0);
        QCOMPARE(canvas.document().graph.nodes.value(id).inputs.value("seed").kind(), InputValue::Kind::Auto);
        id = canvas.addNode("fugue/save-image", 0, 0);
        QVERIFY(!canvas.document().graph.nodes.value(id).inputs.contains("embed"));

        QCOMPARE(model.set("new_seed", "fixed"), QString());
        QCOMPARE(model.set("embed_workflow", "no-prompts"), QString());
        id = canvas.addNode("fugue/generate-image", 0, 0);
        QCOMPARE(canvas.document().graph.nodes.value(id).inputs.value("seed").kind(), InputValue::Kind::Literal);
        id = canvas.addNode("fugue/save-image", 0, 0);
        QCOMPARE(canvas.document().graph.nodes.value(id).inputs.value("embed").literal(), QJsonValue("no-prompts"));
    }

    // #239: the retired canvas_show_new_results key in an old file is read without a problem and left in the file.
    void aRetiredCanvasShowNewResultsKeyIsIgnoredWithoutAProblem() {
        writeFile(settingsFile(), "{\"canvas_show_new_results\": false, \"restore_tabs\": false}");
        SettingsModel model(settingsFile());
        QVERIFY2(model.problems().isEmpty(), qPrintable(model.problems().join('\n')));
        QVERIFY(!model.restoreTabs());
        QVERIFY(!model.set("canvas_show_new_results", true).isEmpty()); // no longer a setting
    }

    // The Sampler's step picture (#297): fast by default, a preference; the default leaves the file.
    void theStepPreviewIsFastAndPersists() {
        SettingsModel model(settingsFile());
        QCOMPARE(model.stepPreview(), QString("fast"));
        QCOMPARE(model.set("step_preview", "vae"), QString());
        QCOMPARE(SettingsModel(settingsFile()).stepPreview(), QString("vae"));
        QVERIFY(!model.set("step_preview", "sharp").isEmpty());
        QCOMPARE(model.set("step_preview", "fast"), QString());
        QVERIFY(!readJsonObject(settingsFile()).value().contains("step_preview"));
    }

    // Node pictures (#263): smart by default, a preference; the default leaves the file.
    void thePictureModeIsSmartAndPersists() {
        SettingsModel model(settingsFile());
        QCOMPARE(model.pictureMode(), QString("smart"));
        QCOMPARE(model.set("picture_mode", "all"), QString());
        QCOMPARE(model.pictureMode(), QString("all"));
        QCOMPARE(SettingsModel(settingsFile()).pictureMode(), QString("all"));
        QVERIFY(!model.set("picture_mode", "some").isEmpty());
        QCOMPARE(model.set("picture_mode", "smart"), QString());
        QVERIFY(!readJsonObject(settingsFile()).value().contains("picture_mode"));
    }

    // Reopen tabs at start (#212): on by default; off is written (the default is on), on again takes the key out.
    void restoreTabsIsOnByDefaultAndPersists() {
        SettingsModel model;
        QVERIFY(model.restoreTabs());
        QCOMPARE(model.set("restore_tabs", false), QString());
        QVERIFY(!model.restoreTabs());
        QVERIFY(!SettingsModel().restoreTabs());
        QVERIFY(!model.set("restore_tabs", "no").isEmpty());
        QCOMPARE(model.set("restore_tabs", true), QString());
        QVERIFY(!readJsonObject(settingsFile()).value().contains("restore_tabs"));
    }

    // The hover magnifier (#123): off by default, on when set, and back to off takes the key out.
    void theHoverMagnifierIsOffByDefaultAndPersists() {
        SettingsModel model;
        QVERIFY(!model.hoverMagnifier());
        QCOMPARE(model.set("hover_magnifier", true), QString());
        QVERIFY(model.hoverMagnifier());
        QVERIFY(SettingsModel().hoverMagnifier());
        QVERIFY(!model.set("hover_magnifier", "yes").isEmpty());
        QVERIFY(model.hoverMagnifier());
        QCOMPARE(model.set("hover_magnifier", false), QString());
        QVERIFY(!readJsonObject(settingsFile()).value().contains("hover_magnifier"));
    }

    // The Corners slider and "follow Omarchy" (#467, #63) reach the Theme at once: 8 px by default, the slider's factor, or
    // Omarchy's rounding as the base; Reset leaves the option.
    void theCornersApplyLive() {
        QTemporaryDir omarchy;
        Theme theme(omarchy.path());
        SettingsModel model;
        model.attach(nullptr, nullptr, nullptr, &theme);
        QVERIFY(!model.lookCornersOmarchy());
        QCOMPARE(theme.look()->property("corner").toInt(), 8);
        QCOMPARE(model.set("look_corners", 0.75), QString());
        QCOMPARE(theme.look()->property("corner").toInt(), 12); // 8 x 2 x 0.75 = 12
        QCOMPARE(model.set("look_corners_omarchy", true), QString());
        QVERIFY(model.lookCornersOmarchy());
        QCOMPARE(theme.look()->property("corner").toInt(), 0); // no Hyprland here: square
        QCOMPARE(model.lookPreset(), QString("custom"));
        QCOMPARE(model.applyLookPreset("calm"), QString());
        QVERIFY(!model.lookCornersOmarchy());
        QCOMPARE(theme.look()->property("corner").toInt(), 8);
        QVERIFY(!readSettings(settingsFile()).document.contains("look_corners_omarchy")); // the default leaves the file
        QVERIFY(!model.set("look_corners_omarchy", "yes").isEmpty());
    }

    // The six sliders reach the Theme's look live; the presets set all six, a slider move after one reads Custom, and the defaults are Calm.
    void theLookSlidersApplyLiveAndPresetsAreDerived() {
        QTemporaryDir omarchy;
        Theme theme(omarchy.path());
        SettingsModel model;
        model.attach(nullptr, nullptr, nullptr, &theme);
        CanvasLook *look = theme.look();
        QCOMPARE(model.lookPreset(), QString("calm"));
        QCOMPARE(look->params().value("haloOpacity").toDouble(), 0.18);
        QCOMPARE(look->params().value("titleFont").toString(), QString("serif"));
        QCOMPARE(model.lookPresets().size(), 2);
        QCOMPARE(model.applyLookPreset("flat"), QString());
        QCOMPARE(look->params().value("titleFont").toString(), QString("ui")); // a preset is parameter values, enumerated ones too
        QCOMPARE(look->params().value("areaLayout").toString(), QString("panes"));
        QCOMPARE(model.lookPreset(), QString("flat"));
        QCOMPARE(model.lookColour(), 0.0);
        QCOMPARE(model.lookGlow(), 0.0);
        QCOMPARE(model.frost(), 0.0);
        QCOMPARE(model.lookDepth(), 0.0);
        QCOMPARE(model.lookCorners(), 0.1);
        QCOMPARE(model.lookSpacing(), 0.5);
        QCOMPARE(look->params().value("haloOpacity").toDouble(), 0.0);
        QCOMPARE(look->params().value("panelTint").toDouble(), 0.0);
        QCOMPARE(look->property("frost").toDouble(), 0.0);
        QCOMPARE(model.set("look_glow", 0.3), QString());
        QCOMPARE(model.lookPreset(), QString("custom"));
        QVERIFY(!model.set("look_spacing", 2).isEmpty());
        QCOMPARE(model.applyLookPreset("calm"), QString());
        QCOMPARE(model.lookPreset(), QString("calm"));
        QVERIFY(!readSettings(settingsFile()).document.contains("look_glow")); // Calm is the defaults: nothing stays in the file
        QCOMPARE(look->params().value("titleFont").toString(), QString("serif"));
        QVERIFY(!model.applyLookPreset("loud").isEmpty());
    }

    // Frost: the glass of the kit's controls over a picture. 0.6 by default; the Theme's look follows it live; 0 is off (the look
    // reads 0, which the kit takes as plain chips); a value outside 0 to 1 is refused; the default leaves the file.
    void theFrostAppliesLiveAndZeroIsOff() {
        QTemporaryDir omarchy;
        Theme theme(omarchy.path());
        SettingsModel model;
        model.attach(nullptr, nullptr, nullptr, &theme);
        QCOMPARE(model.frost(), 0.6);
        QCOMPARE(theme.look()->property("frost").toDouble(), 0.6);
        QCOMPARE(model.set("frost", 0.3), QString());
        QCOMPARE(theme.look()->property("frost").toDouble(), 0.3);
        QCOMPARE(SettingsModel().frost(), 0.3);
        QVERIFY(!model.set("frost", 1.5).isEmpty());
        QVERIFY(!model.set("frost", "strong").isEmpty());
        QCOMPARE(theme.look()->property("frost").toDouble(), 0.3);
        QCOMPARE(model.set("frost", 0), QString());
        QCOMPARE(theme.look()->property("frost").toDouble(), 0.0);
        QCOMPARE(model.set("frost", 0.6), QString());
        QVERIFY(!readSettings(settingsFile()).document.contains("frost"));
    }

    // --- providers ---

    void withoutAProvidersFileTheDefaultsShowAsNotSaved() {
        SettingsModel model;
        const QVariantList providers = model.providers();
        QCOMPARE(providers.size(), defaultProviderInstances().size());
        const QVariantMap first = providers.first().toMap();
        QCOMPARE(first.value("name").toString(), QStringLiteral("This machine"));
        QCOMPARE(first.value("saved").toBool(), false);
        QCOMPARE(model.providersProblem(), QString());
        QVERIFY(!QFile::exists(providersFile())); // only a change writes it
    }

    void anHttpProviderCanBeAddedWithItsSettings() {
        SettingsModel model;
        QVERIFY(model.providerTypes().contains("http"));
        QCOMPARE(model.addProvider("http"), QString());
        const int added = model.providers().size() - 1;
        QStringList names;
        for (const QVariant &field : model.providers().at(added).toMap().value("fields").toList())
            names << field.toMap().value("name").toString();
        QCOMPARE(names, (QStringList{"api", "image_models", "text_models", "timeout_s"}));
        QCOMPARE(model.setProviderField(added, "image_models", "gpt-image-1, dall-e-3"), QString());
        const ProviderInstance saved = loadProviderInstances(providersFile()).value().last();
        QCOMPARE(saved.type, QStringLiteral("http"));
        QCOMPARE(saved.settings.value("image_models").toString(), QStringLiteral("gpt-image-1, dall-e-3"));
    }

    void aHelperProviderCanBeAddedWithItsIdleTimeout() {
        SettingsModel model;
        QVERIFY(model.providerTypes().contains("fugue-helper"));
        QCOMPARE(model.addProvider("fugue-helper"), QString());
        const int added = model.providers().size() - 1;
        const QVariantList fields = model.providers().at(added).toMap().value("fields").toList();
        QCOMPARE(fields.size(), 1);
        QCOMPARE(fields.first().toMap().value("name").toString(), QStringLiteral("idle_timeout_s"));
        QCOMPARE(fields.first().toMap().value("set").toBool(), false);
        QCOMPARE(model.setProviderField(added, "idle_timeout_s", "120"), QString());
        const ProviderInstance saved = loadProviderInstances(providersFile()).value().last();
        QCOMPARE(saved.type, QStringLiteral("fugue-helper"));
        QCOMPARE(saved.settings.value("idle_timeout_s").toString(), QStringLiteral("120"));
    }

    void aProviderChangeWritesEveryInstanceOwnerOnly() {
        SettingsModel model;
        QCOMPARE(model.addProvider("omarchy"), QString());
        const int added = model.providers().size() - 1;
        QCOMPARE(model.setProviderField(added, "name", "Studio box"), QString());
        QCOMPARE(model.setProviderField(added, "address", "https://studio.example.ts.net"), QString());
        QCOMPARE(model.setProviderField(added, "enabled", false), QString());
        QCOMPARE(QFile::permissions(providersFile()) & (QFile::ReadGroup | QFile::ReadOther | QFile::WriteGroup | QFile::WriteOther),
                 QFile::Permissions());
        const auto loaded = loadProviderInstances(providersFile());
        QVERIFY(loaded.ok());
        QCOMPARE(loaded.value().size(), defaultProviderInstances().size() + 1);
        QCOMPARE(loaded.value().first().name, QStringLiteral("This machine"));
        QCOMPARE(loaded.value().last().name, QStringLiteral("Studio box"));
        QCOMPARE(loaded.value().last().enabled, false);
        QCOMPARE(model.providers().first().toMap().value("saved").toBool(), true);
    }

    void aKeyIsWrittenButNeverShown() {
        SettingsModel model;
        QCOMPARE(model.setProviderField(0, "key", "sk-very-secret"), QString());
        const QVariantMap first = model.providers().first().toMap();
        QCOMPARE(first.value("keySet").toBool(), true);
        QVERIFY(!QString::fromUtf8(QJsonDocument::fromVariant(model.providers()).toJson()).contains("sk-very-secret"));
        QCOMPARE(loadProviderInstances(providersFile()).value().first().key, QStringLiteral("sk-very-secret"));
        QCOMPARE(model.setProviderField(0, "key", ""), QString()); // cleared
        QCOMPARE(loadProviderInstances(providersFile()).value().first().key, QString());
    }

    void secretModuleSettingsAreMaskedToo() {
        ProviderInstance instance = defaultProviderInstances().first();
        instance.settings.insert("api_token", "tok-123");
        instance.settings.insert("steps_limit", "40");
        QVERIFY(saveProviderInstances(providersFile(), {instance}).isEmpty());
        SettingsModel model;
        const QVariantList fields = model.providers().first().toMap().value("fields").toList();
        QCOMPARE(fields.size(), 2);
        for (const QVariant &field : fields) {
            const QVariantMap f = field.toMap();
            if (f.value("name") == "api_token") {
                QCOMPARE(f.value("secret").toBool(), true);
                QCOMPARE(f.value("value").toString(), QString());
                QCOMPARE(f.value("set").toBool(), true);
            } else {
                QCOMPARE(f.value("value").toString(), QStringLiteral("40"));
            }
        }
        QCOMPARE(model.setProviderField(0, "steps_limit", "50"), QString());
        QCOMPARE(loadProviderInstances(providersFile()).value().first().settings.value("steps_limit").toString(),
                 QStringLiteral("50"));
        QCOMPARE(loadProviderInstances(providersFile()).value().first().settings.value("api_token").toString(),
                 QStringLiteral("tok-123"));
    }

    void badProviderChangesAreRefused() {
        SettingsModel model;
        QCOMPARE(model.addProvider("omarchy"), QString());
        const int added = model.providers().size() - 1;
        QVERIFY(!model.setProviderField(added, "name", "This machine").isEmpty()); // taken
        QVERIFY(!model.setProviderField(added, "name", " ").isEmpty());
        QVERIFY(!model.setProviderField(added, "address", "not a url").isEmpty());
        QVERIFY(!model.setProviderField(added, "address", "ftp://host").isEmpty());
        QVERIFY(!model.setProviderField(99, "name", "X").isEmpty());
        QVERIFY(!model.addProvider("no-such-module").isEmpty());
    }

    void providersAreReorderedAndRemoved() {
        SettingsModel model;
        QCOMPARE(model.addProvider("omarchy"), QString());
        const int added = model.providers().size() - 1;
        const QString name = model.providers().at(added).toMap().value("name").toString();
        QCOMPARE(model.moveProvider(added, 0), QString());
        QCOMPARE(loadProviderInstances(providersFile()).value().first().name, name);
        QCOMPARE(model.removeProvider(0), QString());
        QCOMPARE(loadProviderInstances(providersFile()).value().first().name, QStringLiteral("This machine"));
    }

    void anUnreadableProvidersFileIsReadOnly() {
        writeFile(providersFile(), "{broken");
        SettingsModel model;
        QVERIFY(!model.providersProblem().isEmpty());
        QVERIFY(!model.setProviderField(0, "name", "X").isEmpty());
        QVERIFY(!model.addProvider("omarchy").isEmpty());
        QFile file(providersFile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("{broken"));
    }

    // providers.json edited by hand (or by another window) after the panel read it: a change
    // in the panel is refused rather than written over it, and the panel shows the file as it
    // is now.
    void aProvidersFileChangedOutsideThePanelIsNotOverwritten() {
        const auto list = [](const QStringList &names) {
            QJsonArray providers;
            for (const QString &name : names)
                providers.append(QJsonObject{{"name", name}, {"type", "omarchy"}});
            return QJsonDocument(QJsonObject{{"providers", providers}}).toJson();
        };
        writeFile(providersFile(), list({"This machine", "A"}));
        SettingsModel model;
        QCOMPARE(model.providers().size(), 2);
        writeFile(providersFile(), list({"This machine", "A", "B"}));
        QSignalSpy changed(&model, &SettingsModel::providersChanged);
        QVERIFY(!model.setProviderField(0, "enabled", false).isEmpty());
        const auto loaded = loadProviderInstances(providersFile());
        QVERIFY(loaded.ok());
        QCOMPARE(loaded.value().size(), 3);
        QVERIFY(loaded.value().first().enabled);
        QCOMPARE(model.providers().size(), 3);
        QVERIFY(changed.count() >= 1);
        // Now that the panel shows it, the change goes through.
        QCOMPARE(model.setProviderField(0, "enabled", false), QString());
        QCOMPARE(loadProviderInstances(providersFile()).value().size(), 3);
    }

    // Without providers.json the panel shows the defaults, paired hosts included. While
    // Omarchy's network.json does not read (omarchy-agent-local rewriting it), the hosts are
    // unknown, so a change is refused rather than written without them for good.
    void theDefaultsAreNotSavedWithoutThePairedHosts() {
        const QByteArray home = qgetenv("HOME");
        const auto restore = qScopeGuard([home] { qputenv("HOME", home); });
        qputenv("HOME", m_home.filePath("home").toUtf8());
        const QString network = m_home.filePath("home/.local/state/omarchy/agent-local/network.json");
        writeFile(network, "{\"hosts\": {\"studio\": ");
        SettingsModel model;
        const QString refused = model.setProviderField(0, "enabled", false);
        QVERIFY(!refused.isEmpty());
        QVERIFY2(refused.contains("network.json"), qPrintable(refused));
        QVERIFY(!QFile::exists(providersFile()));
        // Once it reads again, the hosts are in what is written.
        writeFile(network, QJsonDocument(QJsonObject{{"hosts", QJsonObject{{"studio", QJsonObject{
                               {"dns", "studio.tail1234.ts.net"}, {"paired", true}}}}}}).toJson());
        model.reload();
        QCOMPARE(model.setProviderField(0, "enabled", false), QString());
        const auto loaded = loadProviderInstances(providersFile());
        QVERIFY(loaded.ok());
        QCOMPARE(loaded.value().size(), 2);
        QCOMPARE(loaded.value().last().name, QStringLiteral("studio"));
    }

    // "Add a service…" (spec 8.2): the table is one place, its rows are well formed, and a cloud row can only be https.
    void providerPresetsAreWellFormed() {
        QSet<QString> ids;
        for (const ProviderPreset &p : knownProviderPresets()) {
            QVERIFY2(!ids.contains(p.id), qPrintable(p.id));
            ids.insert(p.id);
            QVERIFY(!p.name.isEmpty() && !p.description.isEmpty());
            QVERIFY(QStringList({"omarchy", "http", "fugue-helper", "fal"}).contains(p.type));
            QVERIFY(QStringList({"cloud", "own"}).contains(p.group));
            if (p.group == QLatin1String("cloud")) {
                QVERIFY2(p.address.isEmpty() || p.address.startsWith("https://"), qPrintable(p.id));
                QVERIFY(p.needsKey && p.keyUrl.startsWith("https://") && !p.privacy.isEmpty());
            } else {
                QVERIFY(p.privacy.isEmpty() && p.keyUrl.isEmpty());
            }
            QVERIFY(p.needsAddress == p.address.isEmpty() || p.type == "fal");
        }
        QVERIFY(ids.contains("fal"));
        QCOMPARE(providerPresetById("fal").address, QString()); // the fal type's own default
        QVERIFY(providerPresetById("nope").id.isEmpty());
    }

    void addingAPresetWritesTheInstanceAndNamesAreUnique() {
        SettingsModel model;
        QCOMPARE(model.addPreset("openai"), QString());
        QCOMPARE(model.addPreset("openai"), QString());
        QCOMPARE(model.addPreset("fal"), QString());
        QVERIFY(!model.addPreset("nope").isEmpty());
        const auto loaded = loadProviderInstances(providersFile());
        QVERIFY(loaded.ok());
        const QList<ProviderInstance> list = loaded.value();
        QVERIFY(providersFile().startsWith(m_home.path()));
        QCOMPARE(list.at(list.size() - 3).name, QStringLiteral("OpenAI"));
        QCOMPARE(list.at(list.size() - 3).type, QStringLiteral("http"));
        QCOMPARE(list.at(list.size() - 3).address, QStringLiteral("https://api.openai.com/v1"));
        QCOMPARE(list.at(list.size() - 3).key, QString());
        QCOMPARE(list.at(list.size() - 2).name, QStringLiteral("OpenAI 2"));
        QCOMPARE(list.last().name, QStringLiteral("fal.ai"));
        QCOMPARE(list.last().type, QStringLiteral("fal"));
        QCOMPARE(list.last().address, QString());
        const QVariantMap card = model.providers().last().toMap();
        // fal's own settings: labelled, the rare ones advanced, store_io a switch stored as a real true.
        QVariantMap seen;
        for (const QVariant &f : card.value("fields").toList())
            seen.insert(f.toMap().value("name").toString(), f);
        QVERIFY(seen.contains("store_io") && seen.value("store_io").toMap().value("isBool").toBool());
        QVERIFY(!seen.value("image_models").toMap().value("advanced").toBool());
        QVERIFY(seen.value("poll_ms").toMap().value("advanced").toBool() && seen.value("rest_address").toMap().value("advanced").toBool());
        QVERIFY(card.value("addressAdvanced").toBool());
        const int fal = model.providers().size() - 1;
        QCOMPARE(model.setProviderField(fal, "store_io", true), QString());
        QCOMPARE(loadProviderInstances(providersFile()).value().last().settings.value("store_io"), QJsonValue(true));
        QCOMPARE(model.setProviderField(fal, "store_io", false), QString());
        QVERIFY(!loadProviderInstances(providersFile()).value().last().settings.contains("store_io"));
        // The page learns where to get the key and what is sent, without the key.
        QVERIFY(card.value("keyUrl").toString().startsWith("https://fal.ai/"));
        QVERIFY(!card.value("privacy").toString().isEmpty());
    }
};

QTEST_MAIN(TestSettingsModel)
#include "test_settings_model.moc"
