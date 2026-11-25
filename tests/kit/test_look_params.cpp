// The look parameters (look_params.h, docs/canvas-look.md 20.3): the defaults are the spec's values, every value stays in its range,
// each slider moves its own group and nothing else, and look.json overrides by name and is read again when it changes.
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "look.h"
#include "look_params.h"

using namespace fugue::look;

class TestLookParams : public QObject {
    Q_OBJECT

    static double at(const QVariantMap &map, const char *name) { return map.value(name).toDouble(); }

private slots:
    // The approved numbers (the boards, docs/canvas-look.md 20.4 to 20.11), at the default sliders.
    void theDefaultsAreTheSpecsValues() {
        const QVariantMap p = resolveParams({}, {});
        QCOMPARE(at(p, "panelTint"), 0.06 + 0.16 * 0.4);
        QCOMPARE(at(p, "toggleOnTop"), 0.52);
        QCOMPARE(at(p, "toggleOnBottom"), 0.34);
        QCOMPARE(at(p, "segThumbTop"), 0.55);
        QCOMPARE(at(p, "haloStop"), 0.22);
        QCOMPARE(at(p, "haloOpacity"), 0.18);
        QCOMPARE(at(p, "haloWidth"), 65.0);
        QCOMPARE(at(p, "heroFadeStart"), 0.58);
        QCOMPARE(at(p, "wellShadowAlpha"), 0.22);
        QCOMPARE(at(p, "raiseEdge"), 0.07);
        QCOMPARE(at(p, "cornerCard"), 4.5);
        QCOMPARE(at(p, "areaGap"), 2.0);
        QCOMPARE(at(p, "durToggle"), 160.0);
        QCOMPARE(resolveParams({}, {{"motion", "reduced"}}).value("durToggle").toDouble(), 0.0); // reduced motion: no animation
        for (const ParamDef &def : paramDefs()) {
            QVERIFY2(def.min <= def.def && def.def <= def.max, qPrintable(def.name));
            QCOMPARE(at(p, def.name.toUtf8().constData()), def.def);
        }
    }

    // Each slider at 0 and 1 moves its own group, keeps every value in range, and leaves the other groups at their defaults.
    void eachSliderMovesItsGroupAndNothingElse() {
        const QVariantMap base = resolveParams({}, {});
        for (const char *slider : {"colour", "glow", "depth", "corners", "spacing"}) {
            for (double position : {0.0, 1.0}) {
                Sliders s;
                (std::string(slider) == "colour" ? s.colour : std::string(slider) == "glow" ? s.glow : std::string(slider) == "depth" ? s.depth
                 : std::string(slider) == "corners" ? s.corners : s.spacing) = position;
                const QVariantMap p = resolveParams(s, {});
                int moved = 0;
                for (const ParamDef &def : paramDefs()) {
                    const double v = at(p, def.name.toUtf8().constData());
                    QVERIFY2(v >= def.min && v <= def.max, qPrintable(def.name));
                    if (def.slider != QLatin1String(slider)) {
                        QCOMPARE(v, at(base, def.name.toUtf8().constData()));
                    } else if (!qFuzzyCompare(v, def.def)) {
                        ++moved;
                    }
                }
                QVERIFY2(moved > 0, slider);
            }
        }
        Sliders none;
        none.glow = 0;
        QCOMPARE(at(resolveParams(none, {}), "haloOpacity"), 0.0); // 0 is no halo
        Sliders flat;
        flat.depth = 0;
        const QVariantMap f = resolveParams(flat, {});
        QCOMPARE(at(f, "wellShadowAlpha"), 0.0);
        QCOMPARE(at(f, "raiseShadowAlpha"), 0.0);
        QVERIFY(at(f, "wellDarken") > 0); // fields still step darker than their group
        Sliders deep;
        deep.depth = 1;
        const QVariantMap d = resolveParams(deep, {});
        QVERIFY(at(d, "wellShadowAlpha") <= 0.4 && at(d, "wellShadowHeight") <= 3 && at(d, "raiseShadowOffset") <= 3);
        Sliders square, spacious;
        square.corners = 0;
        QVERIFY(at(resolveParams(square, {}), "cornerWell") >= 0.4); // never below about 1.6px
        spacious.spacing = 0;
        QVERIFY(at(resolveParams(spacious, {}), "rowMinHeight") >= 7); // the press target stays
    }

    // look.json: an override by name, then the slider on top; a bad entry is a problem line and is clamped or dropped.
    void theFileOverridesByNameAndReportsWhatIsWrong() {
        QStringList problems;
        const QVariantMap o = parseLookFile(R"({"cornerCard": 99, "nope": 1, "areaGap": "wide", "haloOpacity": 0.1})", &problems);
        QCOMPARE(problems.size(), 3);
        QCOMPARE(at(o, "cornerCard"), paramDef("cornerCard")->max);
        QVERIFY(!o.contains("nope") && !o.contains("areaGap"));
        QCOMPARE(at(resolveParams({}, o), "haloOpacity"), 0.1);
        Sliders s;
        s.glow = 1;
        QCOMPARE(at(resolveParams(s, o), "haloOpacity"), 0.2); // the slider applies on top of the file
        QStringList notObject;
        parseLookFile("[1]", &notObject);
        QCOMPARE(notObject.size(), 1);
        QVERIFY(parseLookFile(defaultLookFile(), &problems).size() == paramDefs().size()); // the listing is a valid file
    }

    // The structural choices are parameters too: an enumerated value from the file, a bad one reported and the first choice kept.
    // Every shipped preset parses cleanly, and a parameter a preset does not name keeps its default.
    void theShippedPresetsAreDataAndKeepDefaults() {
        QVERIFY(QFile::exists(":/fugue/looks/calm.json"));
        QStringList problems;
        QCOMPARE(resolveParams({}, {}).value("areaLayout").toString(), QString("cards"));
        const QVariantMap bad = parseLookFile(R"({"areaLayout": "grid", "motion": "reduced"})", &problems);
        QCOMPARE(problems.size(), 1);
        QCOMPARE(bad.value("motion").toString(), QString("reduced"));
        QCOMPARE(resolveParams({}, bad).value("areaLayout").toString(), QString("cards"));
        const QVariantMap calm = resolveParams({}, {});
        for (const QString &id : presetIds()) {
            QStringList own;
            const Preset preset = loadPreset(id, &own);
            QVERIFY2(own.isEmpty(), qPrintable(id + ": " + own.join(" ")));
            QCOMPARE(preset.id, id);
            const QVariantMap resolved = resolveParams({}, preset.params);
            for (const ParamDef &def : paramDefs())
                if (!preset.params.contains(def.name)) // not named: the default
                    QCOMPARE(resolved.value(def.name), calm.value(def.name));
        }
        QCOMPARE(loadPreset("nope").id, QString("calm")); // unknown: Calm
        QCOMPARE(loadPreset("flat").params.value("titleFont").toString(), QString("ui"));
        QCOMPARE(loadPreset("flat").sliders.value("frost").toDouble(), 0.0);
    }

    // The look reads the file at once and again when it changes, in a scratch folder.
    void theLookReloadsTheFileLive() {
        QTemporaryDir dir;
        const QString path = dir.filePath("look.json");
        CanvasLook look;
        QCOMPARE(look.params().value("areaGap").toDouble(), 2.0);
        look.setLookFile(path); // absent: the defaults
        QCOMPARE(look.params().value("areaGap").toDouble(), 2.0);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"areaGap": 4})");
        file.close();
        QTRY_COMPARE(look.params().value("areaGap").toDouble(), 4.0);
        QTest::ignoreMessage(QtWarningMsg, "look.json: bogus is not a look parameter.");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"areaGap": 5, "bogus": 1})");
        file.close();
        QTRY_COMPARE(look.params().value("areaGap").toDouble(), 5.0);
        QCOMPARE(look.lookProblems().size(), 1);
        QVERIFY(QFile::remove(path));
        QTRY_COMPARE(look.params().value("areaGap").toDouble(), 2.0);
    }
};

QTEST_MAIN(TestLookParams)
#include "test_look_params.moc"
