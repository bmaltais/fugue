// settings.json (spec 1.4): what it holds, what a bad value does, and what writing keeps.

#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/json.h"
#include "engine/paths.h"
#include "engine/settings.h"

using namespace fugue;

namespace {

void write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

} // namespace

class TestSettings : public QObject {
    Q_OBJECT

private slots:
    void withoutAFileEverySettingIsItsDefault() {
        QTemporaryDir dir;
        const SettingsFile file = readSettings(dir.filePath("settings.json"));
        QVERIFY(file.problems.isEmpty());
        QVERIFY(file.writable);
        QCOMPARE(file.settings.navigation, QStringLiteral("legacy"));
        QCOMPARE(file.settings.language, QString());
        QCOMPARE(file.settings.outputFolder, QString());
        QCOMPARE(file.settings.newSeed, QStringLiteral("random"));
        QCOMPARE(file.settings.embedWorkflow, QStringLiteral("full"));
        QVERIFY(!file.settings.hoverMagnifier); // off by default (#123)
        QVERIFY(file.settings.modelPaths.isEmpty());
    }

    void valuesInTheFileAreRead() {
        QTemporaryDir dir;
        write(dir.filePath("settings.json"),
              R"({"navigation": "standard", "language": "da", "output_folder": "~/Art",
                  "inputs_folder": "/data/in", "model_paths": ["~/ComfyUI/models"],
                  "pack_paths": ["/opt/packs"], "new_seed": "fixed", "embed_workflow": "none"})");
        const SettingsFile file = readSettings(dir.filePath("settings.json"));
        QVERIFY2(file.problems.isEmpty(), qPrintable(file.problems.join('\n')));
        QCOMPARE(file.settings.navigation, QStringLiteral("standard"));
        QCOMPARE(file.settings.language, QStringLiteral("da"));
        QCOMPARE(file.settings.outputFolder, QStringLiteral("~/Art"));
        QCOMPARE(file.settings.inputsFolder, QStringLiteral("/data/in"));
        QCOMPARE(file.settings.modelPaths, QStringList{"~/ComfyUI/models"});
        QCOMPARE(file.settings.packPaths, QStringList{"/opt/packs"});
        QCOMPARE(file.settings.newSeed, QStringLiteral("fixed"));
        QCOMPARE(file.settings.embedWorkflow, QStringLiteral("none"));
    }

    void aBadValueIsAProblemAndItsDefaultIsUsed() {
        QTemporaryDir dir;
        write(dir.filePath("settings.json"),
              R"({"navigation": "sideways", "output_folder": "relative/path", "model_paths": [3]})");
        const SettingsFile file = readSettings(dir.filePath("settings.json"));
        QCOMPARE(file.problems.size(), 3);
        QVERIFY(file.problems.first().contains("navigation"));
        QCOMPARE(file.settings.navigation, QStringLiteral("legacy"));
        QCOMPARE(file.settings.outputFolder, QString());
        QVERIFY(file.settings.modelPaths.isEmpty());
        QVERIFY(file.writable);
    }

    void aFileThatIsNotJsonIsNotWritable() {
        QTemporaryDir dir;
        write(dir.filePath("settings.json"), "{nope");
        const SettingsFile file = readSettings(dir.filePath("settings.json"));
        QVERIFY(!file.writable);
        QCOMPARE(file.problems.size(), 1);
        QCOMPARE(file.settings.navigation, QStringLiteral("legacy"));
    }

    void checkingNamesWhatIsWrong() {
        QCOMPARE(checkSetting("navigation", "standard"), QString());
        QVERIFY(!checkSetting("navigation", "fast").isEmpty());
        QVERIFY(!checkSetting("output_folder", "").isEmpty());
        QVERIFY(!checkSetting("output_folder", "pictures").isEmpty());
        QCOMPARE(checkSetting("output_folder", "~/Pictures/Mine"), QString());
        QCOMPARE(checkSetting("model_paths", QJsonArray{"/a", "~/b"}), QString());
        QVERIFY(!checkSetting("model_paths", QJsonArray{"/a", "/a"}).isEmpty());
        QVERIFY(!checkSetting("model_paths", "/a").isEmpty());
        QVERIFY(!checkSetting("language", "Danish").isEmpty());
        QCOMPARE(checkSetting("language", "pt-BR"), QString());
        QVERIFY(!checkSetting("embed_workflow", "some").isEmpty());
        QVERIFY(!checkSetting("new_seed", "increment").isEmpty());
        QVERIFY(!checkSetting("no_such_setting", 1).isEmpty());
        QCOMPARE(checkSetting("hover_magnifier", true), QString());
        QVERIFY(!checkSetting("hover_magnifier", "yes").isEmpty());
    }

    // The canvas corners (#63): 8 px unless set, any whole number of px from 0 to 16, or
    // "omarchy" to follow Hyprland's rounding as Fugue did before.
    void theCornerRadiusIsFixedOrFollowsOmarchy() {
        QTemporaryDir dir;
        QCOMPARE(readSettings(dir.filePath("settings.json")).settings.cornerRadius, 8);
        QCOMPARE(settingDefault("corner_radius"), QJsonValue(8));
        for (const QJsonValue &good : {QJsonValue(0), QJsonValue(12), QJsonValue(16), QJsonValue("omarchy")})
            QCOMPARE(checkSetting("corner_radius", good), QString());
        for (const QJsonValue &bad : {QJsonValue(-1), QJsonValue(17), QJsonValue(8.5), QJsonValue("round"), QJsonValue(true)})
            QVERIFY(!checkSetting("corner_radius", bad).isEmpty());

        write(dir.filePath("settings.json"), R"({"corner_radius": 12})");
        QCOMPARE(readSettings(dir.filePath("settings.json")).settings.cornerRadius, 12);
        write(dir.filePath("settings.json"), R"({"corner_radius": "omarchy"})");
        QCOMPARE(readSettings(dir.filePath("settings.json")).settings.cornerRadius, Settings::followOmarchy);
        write(dir.filePath("settings.json"), R"({"corner_radius": 40})");
        const SettingsFile bad = readSettings(dir.filePath("settings.json"));
        QCOMPARE(bad.problems.size(), 1);
        QCOMPARE(bad.settings.cornerRadius, 8);
    }

    // The hover magnifier (#123): off unless the file says true; a bad value is a problem.
    void theHoverMagnifierIsOffUnlessSet() {
        QTemporaryDir dir;
        QCOMPARE(settingDefault("hover_magnifier"), QJsonValue(false));
        write(dir.filePath("settings.json"), R"({"navigation": "standard"})"); // an older file: no key
        QVERIFY(!readSettings(dir.filePath("settings.json")).settings.hoverMagnifier);
        write(dir.filePath("settings.json"), R"({"hover_magnifier": true})");
        QVERIFY(readSettings(dir.filePath("settings.json")).settings.hoverMagnifier);
        write(dir.filePath("settings.json"), R"({"hover_magnifier": "yes"})");
        const SettingsFile bad = readSettings(dir.filePath("settings.json"));
        QCOMPARE(bad.problems.size(), 1);
        QVERIFY(!bad.settings.hoverMagnifier);
    }

    void changingOneKeepsTheKeysItDoesNotKnow() {
        QTemporaryDir dir;
        const QString path = dir.filePath("fugue/settings.json");
        QVERIFY(QDir().mkpath(dir.filePath("fugue")));
        write(path, R"({"someone_elses": {"x": 1}, "navigation": "standard"})");
        QVERIFY(changeSetting(path, "embed_workflow", "no-prompts").isEmpty());
        const auto document = readJsonObject(path);
        QVERIFY(document.ok());
        QCOMPARE(document.value().value("someone_elses").toObject().value("x").toInt(), 1);
        QCOMPARE(document.value().value("navigation").toString(), QStringLiteral("standard"));
        QCOMPARE(document.value().value("embed_workflow").toString(), QStringLiteral("no-prompts"));
    }

    void changingToTheDefaultRemovesTheKey() {
        QTemporaryDir dir;
        const QString path = dir.filePath("settings.json");
        write(path, R"({"navigation": "standard", "other": true})");
        QVERIFY(changeSetting(path, "navigation", "legacy").isEmpty());
        const auto document = readJsonObject(path);
        QVERIFY(!document.value().contains("navigation"));
        QVERIFY(document.value().value("other").toBool());
    }

    void changingMakesTheFolderAndFile() {
        QTemporaryDir dir;
        const QString path = dir.filePath("config/fugue/settings.json");
        QVERIFY(changeSetting(path, "navigation", "standard").isEmpty());
        QCOMPARE(readSettings(path).settings.navigation, QStringLiteral("standard"));
    }

    void aBadValueIsNotWritten() {
        QTemporaryDir dir;
        const QString path = dir.filePath("settings.json");
        const Errors errors = changeSetting(path, "navigation", "sideways");
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().code, QStringLiteral("usage"));
        QVERIFY(!QFile::exists(path));
    }

    void aFileThatIsNotJsonIsNeverOverwritten() {
        QTemporaryDir dir;
        const QString path = dir.filePath("settings.json");
        write(path, "{nope");
        QCOMPARE(changeSetting(path, "navigation", "standard").size(), 1);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("{nope"));
    }

    void theOutputAndInputsFoldersFollowTheSettings() {
        QTemporaryDir dir;
        qputenv("XDG_CONFIG_HOME", dir.filePath("config").toUtf8());
        qputenv("XDG_PICTURES_DIR", dir.filePath("pictures").toUtf8());
        QCOMPARE(outputDir(), dir.filePath("pictures/Fugue"));
        QCOMPARE(inputsDir(), dir.filePath("pictures/Fugue/inputs"));
        QCOMPARE(settingsPath(), dir.filePath("config/fugue/settings.json"));
        QVERIFY(changeSetting(settingsPath(), "output_folder", "~/Art").isEmpty());
        QCOMPARE(outputDir(), QDir::homePath() + "/Art");
        QCOMPARE(inputsDir(), QDir::homePath() + "/Art/inputs");
        QVERIFY(changeSetting(settingsPath(), "inputs_folder", dir.filePath("in")).isEmpty());
        QCOMPARE(inputsDir(), dir.filePath("in"));
        qunsetenv("XDG_PICTURES_DIR");
    }
};

QTEST_GUILESS_MAIN(TestSettings)
#include "test_settings.moc"
