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
        QCOMPARE(checkSetting("frost", 0.0), QString());
        QCOMPARE(checkSetting("frost", 1), QString());
        QVERIFY(!checkSetting("frost", -0.1).isEmpty());
        QVERIFY(!checkSetting("frost", "on").isEmpty());
    }

    // The radius is the Look's Corners slider, with "follow Omarchy" as an option beside it (#467).
    void theCornerOptionIsABoolean() {
        QTemporaryDir dir;
        QCOMPARE(readSettings(dir.filePath("settings.json")).settings.lookCornersOmarchy, false);
        QCOMPARE(settingDefault("look_corners_omarchy"), QJsonValue(false));
        QCOMPARE(checkSetting("look_corners_omarchy", true), QString());
        QVERIFY(!checkSetting("look_corners_omarchy", "omarchy").isEmpty());
        QVERIFY(!settingKeys().contains("corner_radius"));
        QVERIFY(!changeSetting(dir.filePath("settings.json"), "corner_radius", 8).isEmpty());
    }

    // The old corner_radius migrates when read: px to the slider position that gives the same radius, "omarchy" to the option.
    void anOldCornerRadiusMigratesToTheLookSettings() {
        QTemporaryDir dir;
        const QString path = dir.filePath("settings.json");
        const auto read = [&](const char *json) {
            write(path, json);
            return readSettings(path);
        };
        SettingsFile file = read(R"({"corner_radius": 12})");
        QVERIFY(file.problems.isEmpty());
        QVERIFY(!file.document.contains("corner_radius"));
        QCOMPARE(file.settings.lookCorners, 0.75); // 8 px x 2 x 0.75 = 12
        QCOMPARE(std::lround(8 * 2 * file.settings.lookCorners), 12);
        QCOMPARE(file.settings.lookCornersOmarchy, false);
        file = read(R"({"corner_radius": 8})"); // the old default is the new one
        QCOMPARE(file.settings.lookCorners, 0.5);
        QVERIFY(!file.document.contains("look_corners"));
        file = read(R"({"corner_radius": 0})");
        QCOMPARE(file.settings.lookCorners, 0.0);
        file = read(R"({"corner_radius": 16})");
        QCOMPARE(file.settings.lookCorners, 1.0);
        file = read(R"({"corner_radius": 40})"); // out of the old range: held to it
        QCOMPARE(file.settings.lookCorners, 1.0);
        file = read(R"({"corner_radius": "omarchy"})");
        QCOMPARE(file.settings.lookCornersOmarchy, true);
        QCOMPARE(file.settings.lookCorners, 0.5);
        // What the file already says about the new settings wins.
        file = read(R"({"corner_radius": 12, "look_corners": 0.2})");
        QCOMPARE(file.settings.lookCorners, 0.2);
        file = read(R"({"corner_radius": "omarchy", "look_corners_omarchy": false})");
        QCOMPARE(file.settings.lookCornersOmarchy, false);
    }

    // The next write leaves the old key out and keeps the migrated value.
    void aWriteDropsTheOldCornerRadius() {
        QTemporaryDir dir;
        const QString path = dir.filePath("settings.json");
        write(path, R"({"corner_radius": "omarchy", "navigation": "standard"})");
        QVERIFY(changeSetting(path, "frost", 0.3).isEmpty());
        const QJsonObject document = readSettings(path).document;
        QVERIFY(!document.contains("corner_radius"));
        QCOMPARE(document.value("look_corners_omarchy"), QJsonValue(true));
        QCOMPARE(document.value("navigation"), QJsonValue("standard"));
        QCOMPARE(document.value("frost"), QJsonValue(0.3));
    }

    // The picture mode (#263): smart unless set; smart, sinks or all, nothing else.
    void thePictureModeIsSmartUnlessSet() {
        QTemporaryDir dir;
        QCOMPARE(settingDefault("picture_mode"), QJsonValue("smart"));
        QCOMPARE(readSettings(dir.filePath("settings.json")).settings.pictureMode, QString("smart"));
        for (const char *good : {"smart", "sinks", "all"})
            QCOMPARE(checkSetting("picture_mode", good), QString());
        for (const QJsonValue &bad : {QJsonValue("some"), QJsonValue(true), QJsonValue("All")})
            QVERIFY(!checkSetting("picture_mode", bad).isEmpty());
        write(dir.filePath("settings.json"), R"({"picture_mode": "sinks"})");
        QCOMPARE(readSettings(dir.filePath("settings.json")).settings.pictureMode, QString("sinks"));
        QCOMPARE(settingValue(readSettings(dir.filePath("settings.json")).settings, "picture_mode"), QJsonValue("sinks"));
        write(dir.filePath("settings.json"), R"({"picture_mode": "none"})");
        const SettingsFile bad = readSettings(dir.filePath("settings.json"));
        QCOMPARE(bad.problems.size(), 1);
        QCOMPARE(bad.settings.pictureMode, QString("smart"));
        // A project inherits it with the other preferences.
        QCOMPARE(settingClass("picture_mode"), SettingClass::Preferences);
    }

    // The look factors: 0.4, 0.5 and 0.5 unless set; a number from 0 to 1, nothing else.
    void theLookFactorsDefaultAndAreRanged() {
        QTemporaryDir dir;
        const QStringList keys{"look_colour", "look_glow", "look_depth", "look_corners", "look_spacing"};
        QCOMPARE(settingDefault("look_preset"), QJsonValue("calm"));
        QCOMPARE(checkSetting("look_preset", "flat"), QString());
        QVERIFY(!checkSetting("look_preset", "Not A Preset").isEmpty());
        // The node card's face (#466): compact or flat, compact unless set.
        QCOMPARE(settingDefault("node_style"), QJsonValue("compact"));
        QCOMPARE(checkSetting("node_style", "flat"), QString());
        QVERIFY(!checkSetting("node_style", "classic").isEmpty());
        QCOMPARE(settingClass("node_style"), SettingClass::Preferences);
        QCOMPARE(settingDefault("look_colour"), QJsonValue(0.4));
        QCOMPARE(settingDefault("look_corners"), QJsonValue(0.5));
        QCOMPARE(settingDefault("look_spacing"), QJsonValue(0.5));
        QCOMPARE(settingDefault("look_glow"), QJsonValue(0.5));
        QCOMPARE(settingDefault("look_depth"), QJsonValue(0.5));
        const Settings defaults = readSettings(dir.filePath("settings.json")).settings;
        QCOMPARE(defaults.lookColour, 0.4);
        QCOMPARE(defaults.lookCorners, 0.5);
        QCOMPARE(defaults.lookSpacing, 0.5);
        for (const QString &key : keys) {
            QCOMPARE(checkSetting(key, 0.0), QString());
            QCOMPARE(checkSetting(key, 1), QString());
            QVERIFY(!checkSetting(key, -0.1).isEmpty());
            QVERIFY(!checkSetting(key, 1.1).isEmpty());
            QVERIFY(!checkSetting(key, "high").isEmpty());
            QCOMPARE(settingClass(key), SettingClass::Preferences);
        }
        write(dir.filePath("settings.json"), R"({"look_colour": 0, "look_corners": 1, "look_spacing": 0.25})");
        const Settings set = readSettings(dir.filePath("settings.json")).settings;
        QCOMPARE(set.lookColour, 0.0);
        QCOMPARE(set.lookCorners, 1.0);
        QCOMPARE(set.lookSpacing, 0.25);
        write(dir.filePath("settings.json"), R"({"look_colour": 7})");
        const SettingsFile bad = readSettings(dir.filePath("settings.json"));
        QCOMPARE(bad.problems.size(), 1);
        QCOMPARE(bad.settings.lookColour, 0.4);
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

    // The cache limits (#141): 10 GB of results, 1 GB of previews, 20 runs kept; 0 is no limit.
    void theCacheLimitsHaveDefaultsAndBounds() {
        QTemporaryDir dir;
        const Settings defaults = readSettings(dir.filePath("settings.json")).settings;
        QCOMPARE(defaults.cacheResultsGb, 10.0);
        QCOMPARE(defaults.cacheThumbnailsGb, 1.0);
        QCOMPARE(defaults.runsKept, 20);
        QCOMPARE(checkSetting("cache_results_gb", 0), QString());
        QCOMPARE(checkSetting("cache_thumbnails_gb", 0.5), QString());
        QCOMPARE(checkSetting("runs_kept", 5), QString());
        for (const QString &key : {QStringLiteral("cache_results_gb"), QStringLiteral("cache_thumbnails_gb"), QStringLiteral("runs_kept")}) {
            QVERIFY(!checkSetting(key, -1).isEmpty());
            QVERIFY(!checkSetting(key, "big").isEmpty());
        }
        QVERIFY(!checkSetting("runs_kept", 2.5).isEmpty());
        write(dir.filePath("settings.json"), R"({"cache_results_gb": 2.5, "runs_kept": 0})");
        const Settings set = readSettings(dir.filePath("settings.json")).settings;
        QCOMPARE(set.cacheResultsGb, 2.5);
        QCOMPARE(set.runsKept, 0);
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
