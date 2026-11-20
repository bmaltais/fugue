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
#include "engine/json.h"
#include "engine/paths.h"
#include "engine/providers.h"
#include "engine/providers_file.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "settings/settings_model.h"
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
};

QTEST_MAIN(TestSettingsModel)
#include "test_settings_model.moc"
