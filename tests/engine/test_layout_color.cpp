// A layout color that is not a theme role (#52, spec 4.5): a hex value, its hue-saturation-value
// form for a color wheel, and the list of recently used colors.
//
// Seam: engine/layout_color.h, on plain strings and a temporary file.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/layout_color.h"

using namespace fugue;

class TestLayoutColor : public QObject {
    Q_OBJECT

private slots:
    void normalizeAcceptsSixAndThreeDigitsWithOrWithoutHash_data() {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("six") << "#FF8800" << "#ff8800";
        QTest::newRow("no hash") << "ff8800" << "#ff8800";
        QTest::newRow("three") << "#f80" << "#ff8800";
        QTest::newRow("spaces") << "  #AbCdEf " << "#abcdef";
        QTest::newRow("alpha is refused") << "#ff880080" << "";
        QTest::newRow("not hex") << "#gg0000" << "";
        QTest::newRow("a role") << "blue" << "";
        QTest::newRow("empty") << "" << "";
        QTest::newRow("four") << "#ff80" << "";
    }
    void normalizeAcceptsSixAndThreeDigitsWithOrWithoutHash() {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(normalizeHex(input), expected);
    }

    void onlyAWholeHexValueIsAHexLayoutColor() {
        QVERIFY(isHexColor("#ff8800"));
        QVERIFY(isHexColor("#FF8800"));
        QVERIFY(!isHexColor("ff8800"));
        QVERIFY(!isHexColor("#f80"));
        QVERIFY(!isHexColor("blue"));
        QVERIFY(!isHexColor(""));
    }

    void hsvOfThePrimariesAndGrays() {
        const Hsv red = hsvFromHex("#ff0000");
        QCOMPARE(red.h, 0.0);
        QCOMPARE(red.s, 1.0);
        QCOMPARE(red.v, 1.0);
        QCOMPARE(hsvFromHex("#00ff00").h, 120.0);
        QCOMPARE(hsvFromHex("#0000ff").h, 240.0);
        const Hsv gray = hsvFromHex("#808080");
        QCOMPARE(gray.s, 0.0);
        QVERIFY(qAbs(gray.v - 128 / 255.0) < 1e-9);
        const Hsv black = hsvFromHex("#000000");
        QCOMPARE(black.v, 0.0);
        // Not a color: black, so a wheel has somewhere to start.
        QCOMPARE(hsvFromHex("nonsense").v, 0.0);
    }

    void hexFromHsvRoundTrips_data() {
        QTest::addColumn<QString>("hex");
        for (const char *hex : {"#ff0000", "#00ff00", "#0000ff", "#ffffff", "#000000", "#808080", "#ff8800", "#12ab9f",
                                "#fedcba", "#010203"})
            QTest::newRow(hex) << QString::fromLatin1(hex);
    }
    void hexFromHsvRoundTrips() {
        QFETCH(QString, hex);
        QCOMPARE(hexFromHsv(hsvFromHex(hex)), hex);
    }

    void hexFromHsvClampsAndWrapsItsInput() {
        QCOMPARE(hexFromHsv({360, 1, 1}), QString("#ff0000"));  // the hue wraps
        QCOMPARE(hexFromHsv({-120, 1, 1}), QString("#0000ff")); // also below zero
        QCOMPARE(hexFromHsv({0, 2, 2}), QString("#ff0000"));    // s and v stop at 1
        QCOMPARE(hexFromHsv({0, -1, -1}), QString("#000000"));  // and at 0
    }

    void rgbOfAHexColorAndBack() {
        const Rgb c = rgbFromHex("#12ab9f");
        QCOMPARE(c.r, 0x12);
        QCOMPARE(c.g, 0xab);
        QCOMPARE(c.b, 0x9f);
        QCOMPARE(hexFromRgb(c.r, c.g, c.b), QString("#12ab9f"));
        QCOMPARE(rgbFromHex("#f80").g, 0x88); // the short form is widened
        const Rgb none = rgbFromHex("nonsense"); // not a color: black
        QCOMPARE(none.r + none.g + none.b, 0);
    }

    void hexFromRgbStopsEachChannelAtItsEnds() {
        QCOMPARE(hexFromRgb(300, -5, 255), QString("#ff00ff"));
        QCOMPARE(hexFromRgb(0, 0, 0), QString("#000000"));
        QCOMPARE(hexFromRgb(1, 2, 3), QString("#010203")); // two digits each
    }

    void everyRgbColorSurvivesAHexToHsvAndBackTrip() {
        // A number field edit goes RGB -> HSV (stored) -> RGB (shown): no channel may drift by one.
        for (int r = 0; r < 256; r += 5)
            for (int g = 0; g < 256; g += 5)
                for (int b = 0; b < 256; b += 5) {
                    const QString hex = hexFromRgb(r, g, b);
                    QCOMPARE(hexFromHsv(hsvFromHex(hex)), hex);
                }
    }

    void hsbShowsWholeDegreesAndPercent_data() {
        QTest::addColumn<double>("h");
        QTest::addColumn<double>("s");
        QTest::addColumn<double>("v");
        QTest::addColumn<int>("shownH");
        QTest::addColumn<int>("shownS");
        QTest::addColumn<int>("shownB");
        QTest::newRow("red") << 0.0 << 1.0 << 1.0 << 0 << 100 << 100;
        QTest::newRow("rounds up") << 120.6 << 0.505 << 0.994 << 121 << 51 << 99;
        QTest::newRow("rounds down") << 120.4 << 0.504 << 0.996 << 120 << 50 << 100;
        QTest::newRow("359.6 is 0 not 360") << 359.6 << 0.5 << 0.5 << 0 << 50 << 50;
        QTest::newRow("359.4 stays") << 359.4 << 0.5 << 0.5 << 359 << 50 << 50;
        QTest::newRow("wraps below zero") << -90.0 << 0.5 << 0.5 << 270 << 50 << 50;
        QTest::newRow("clamps s and v") << 10.0 << 1.5 << -0.5 << 10 << 100 << 0;
    }
    void hsbShowsWholeDegreesAndPercent() {
        QFETCH(double, h);
        QFETCH(double, s);
        QFETCH(double, v);
        QFETCH(int, shownH);
        QFETCH(int, shownS);
        QFETCH(int, shownB);
        const Hsb shown = hsbRounded({h, s, v});
        QCOMPARE(shown.h, shownH);
        QCOMPARE(shown.s, shownS);
        QCOMPARE(shown.b, shownB);
    }

    void recentColorsKeepTheNewestFirstWithoutRepeats() {
        QTemporaryDir dir;
        RecentColors recent(dir.filePath("recent.json"), 3);
        QVERIFY(recent.list().isEmpty());
        recent.use("#111111");
        recent.use("#222222");
        recent.use("#333333");
        QCOMPARE(recent.list(), (QStringList{"#333333", "#222222", "#111111"}));
        recent.use("#111111"); // used again: moves to the front
        QCOMPARE(recent.list(), (QStringList{"#111111", "#333333", "#222222"}));
        recent.use("#444444"); // over the limit: the oldest goes
        QCOMPARE(recent.list(), (QStringList{"#444444", "#111111", "#333333"}));
    }

    void recentColorsTakeOnlyHexAndNormalizeIt() {
        QTemporaryDir dir;
        RecentColors recent(dir.filePath("recent.json"));
        recent.use("blue");
        recent.use("");
        QVERIFY(recent.list().isEmpty());
        recent.use("F80");
        QCOMPARE(recent.list(), (QStringList{"#ff8800"}));
    }

    void recentColorsSurviveARestart() {
        QTemporaryDir dir;
        const QString path = dir.filePath("state/recent.json"); // the folder does not exist yet
        {
            RecentColors recent(path);
            recent.use("#aa0000");
            recent.use("#00bb00");
        }
        RecentColors again(path);
        QCOMPARE(again.list(), (QStringList{"#00bb00", "#aa0000"}));
    }

    void aDamagedRecentFileStartsEmptyAndDropsBadEntries() {
        QTemporaryDir dir;
        const QString path = dir.filePath("recent.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{not json");
        file.close();
        QVERIFY(RecentColors(path).list().isEmpty());

        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(QJsonArray{"#aa0000", "blue", 5, "#AA0000", "#00bb00"}).toJson());
        file.close();
        QCOMPARE(RecentColors(path).list(), (QStringList{"#aa0000", "#00bb00"}));
    }
};

QTEST_GUILESS_MAIN(TestLayoutColor)
#include "test_layout_color.moc"
