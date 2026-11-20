// The canvas look's colors (docs/canvas-look.md section 6): the tokens and port colors that
// src/look.h derives from a theme's palette, checked against docs/canvas-look/derive.py, the
// reference the spec's numbers and mockups come from. The expected values below are what
// `derive.py` gives for three shipped Omarchy themes (a dark one, a light one and a monochrome
// one); the palettes are copied here because CI has no Omarchy. When Omarchy's themes are
// installed, every one of them is also checked for the contrast the spec promises.

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/settings.h"
#include "look.h"
#include "theme.h"

using namespace fugue;

namespace {

// The keys Theme reads, with Theme's fallbacks for the ones a palette lacks.
QHash<QString, QColor> palette(std::initializer_list<std::pair<const char *, const char *>> entries) {
    QHash<QString, QColor> colors{{"accent", QColor("#FFD60A")}, {"background", QColor("#0e0e10")},
                                  {"foreground", QColor("#e6e6e9")}, {"red", QColor("#f7768e")},
                                  {"green", QColor("#9ece6a")}, {"yellow", QColor("#e0af68")},
                                  {"orange", QColor("#ff9e64")}, {"blue", QColor("#7aa2f7")},
                                  {"magenta", QColor("#bb9af7")}, {"cyan", QColor("#7dcfff")}};
    for (const auto &[key, value] : entries)
        colors.insert(key, QColor(value));
    return colors;
}

const QHash<QString, QColor> ethereal = palette({{"accent", "#7d82d9"}, {"background", "#060B1E"},
                                                 {"foreground", "#ffcead"}, {"red", "#ED5B5A"},
                                                 {"yellow", "#E9BB4F"}, {"orange", "#eb8b54"},
                                                 {"green", "#92a593"}, {"cyan", "#a3bfd1"},
                                                 {"blue", "#7d82d9"}, {"magenta", "#c89dc1"}});
const QHash<QString, QColor> flexokiLight = palette({{"accent", "#205EA6"}, {"background", "#FFFCF0"},
                                                     {"foreground", "#100F0F"}, {"red", "#D14D41"},
                                                     {"yellow", "#D0A215"}, {"orange", "#d0772b"},
                                                     {"green", "#879A39"}, {"cyan", "#3AA99F"},
                                                     {"blue", "#205EA6"}, {"magenta", "#CE5D97"}});

// Within one step of 255 per channel: derive.py and the C++ round at different points.
bool near(const QColor &a, const QColor &b) {
    return std::abs(a.red() - b.red()) <= 1 && std::abs(a.green() - b.green()) <= 1
        && std::abs(a.blue() - b.blue()) <= 1;
}

#define COMPARE_COLOR(actual, expected)                                                                    \
    QVERIFY2(near(actual, QColor(expected)),                                                               \
             qPrintable(QStringLiteral("%1 is %2, expected %3").arg(#actual, (actual).name(), expected)))

QString shippedThemes() { return QStringLiteral("/usr/share/omarchy/themes"); }

} // namespace

class TestLookTokens : public QObject {
    Q_OBJECT

private slots:
    void colorMathMatchesTheReference() {
        QCOMPARE(look::contrast(Qt::black, Qt::white), 21.0);
        QCOMPARE(look::contrast(QColor("#777777"), QColor("#777777")), 1.0);
        const look::Oklch white = look::oklch(Qt::white);
        QVERIFY(std::abs(white.L - 1.0) < 1e-6 && white.C < 1e-6);
        // A round trip through OKLCH keeps the color.
        COMPARE_COLOR(look::fromOklch(look::oklch(QColor("#c89dc1"))), "#c89dc1");
        QVERIFY(look::deltaE(QColor("#7d82d9"), QColor("#7d82d9")) < 1e-9);
    }

    void darkThemeTokens() {
        const look::Tokens t = look::tokens(ethereal, false);
        COMPARE_COLOR(t.canvas, "#060b1e");
        COMPARE_COLOR(t.dot, "#262431");
        COMPARE_COLOR(t.dotMajor, "#3d363d");
        COMPARE_COLOR(t.body, "#191a29");
        COMPARE_COLOR(t.header, "#262431");
        COMPARE_COLOR(t.hairline, "#302c36");
        COMPARE_COLOR(t.hairlineStrong, "#514649");
        COMPARE_COLOR(t.well, "#0f1223");
        COMPARE_COLOR(t.label, "#d1aa93");
        COMPARE_COLOR(t.popup, "#1c1d2b");
        COMPARE_COLOR(t.errorHeader, "#4e2f39");
        COMPARE_COLOR(t.errorChip, "#362330");
        COMPARE_COLOR(t.selectedRow, "#292a45");
        QCOMPARE(t.labelShare, 0.80);
        // A 1 px highlight of the text color along the header's top, on dark themes only.
        QVERIFY(std::abs(t.bevel.alphaF() - 0.10) < 0.01);
        QVERIFY(std::abs(t.scrim.alphaF() - 0.45) < 0.01);

        const look::RoleTokens blue = look::roleTokens(t, ethereal.value("blue"), ethereal.value("foreground"), false);
        COMPARE_COLOR(blue.frame, "#121731");
        COMPARE_COLOR(blue.frameEdge, "#30355f");
        COMPARE_COLOR(blue.frameTitle, "#bea8c3");
        COMPARE_COLOR(blue.tint, "#36354f");
        // The neutral role takes the label color.
        const look::RoleTokens muted = look::roleTokens(t, t.label, ethereal.value("foreground"), false);
        COMPARE_COLOR(muted.frame, "#1a1b2a");
        COMPARE_COLOR(muted.frameTitle, "#e8bca0");
        COMPARE_COLOR(muted.tint, "#453c42");
    }

    void aBypassedNodeIsTintedWithTheMagentaAndKeepsItsTextReadable() {
        for (const bool light : {false, true}) {
            const QHash<QString, QColor> &colors = light ? flexokiLight : ethereal;
            const look::Tokens t = look::tokens(colors, light);
            const QColor fg = colors.value("foreground");
            // Visibly not the plain node, toward the magenta.
            QVERIFY(look::deltaE(t.bypassBody, t.body) > 0.02);
            QVERIFY(look::deltaE(t.bypassHeader, t.header) > 0.03);
            QVERIFY(look::deltaE(t.bypassBody, colors.value("magenta"))
                    < look::deltaE(t.body, colors.value("magenta")));
            // The title, the labels and the edge read.
            QVERIFY(look::contrast(fg, t.bypassBody) >= 4.5);
            QVERIFY(look::contrast(fg, t.bypassHeader) >= 4.5);
            QVERIFY(look::contrast(t.label, t.bypassBody) >= 4.5);
            QVERIFY(look::contrast(t.bypassEdge, t.canvas) >= 3.0);
        }
    }

    void lightThemeTokens() {
        const look::Tokens t = look::tokens(flexokiLight, true);
        COMPARE_COLOR(t.canvas, "#f4f1e6");
        COMPARE_COLOR(t.dot, "#d2cfc6");
        COMPARE_COLOR(t.dotMajor, "#bdbbb2");
        COMPARE_COLOR(t.body, "#fffef9");
        COMPARE_COLOR(t.header, "#f3f2ed");
        COMPARE_COLOR(t.hairline, "#e0dfdb");
        COMPARE_COLOR(t.hairlineStrong, "#c1c0bc");
        COMPARE_COLOR(t.well, "#f3f2ed");
        COMPARE_COLOR(t.label, "#2f2e2d");
        COMPARE_COLOR(t.popup, "#fffef9");
        COMPARE_COLOR(t.errorHeader, "#eedbd5");
        COMPARE_COLOR(t.errorChip, "#faece7");
        COMPARE_COLOR(t.selectedRow, "#e4ebef");
        QCOMPARE(t.labelShare, 0.87);
        QCOMPARE(t.bevel.alpha(), 0);
        QVERIFY(std::abs(t.scrim.alphaF() - 0.22) < 0.01);

        const look::RoleTokens blue = look::roleTokens(t, flexokiLight.value("blue"), flexokiLight.value("foreground"), true);
        COMPARE_COLOR(blue.frame, "#dbe0de");
        COMPARE_COLOR(blue.frameEdge, "#aabed0");
        COMPARE_COLOR(blue.frameTitle, "#18375a");
        COMPARE_COLOR(blue.tint, "#d6dde3");
    }

    // Owner's decision (docs/canvas-look.md 18.1): only colliding port colors change.
    void portColorsRepairOnlyWhatCollides() {
        const look::Tokens t = look::tokens(ethereal, false);
        bool mono = true;
        const QHash<QString, QColor> ports = look::portColors(ethereal, false, t, &mono);
        QVERIFY(!mono);
        COMPARE_COLOR(ports.value("image"), "#c89dc1");  // kept
        COMPARE_COLOR(ports.value("text"), "#70bc73");   // green was too gray: its fallback hue
        COMPARE_COLOR(ports.value("number"), "#7d82d9"); // kept
        COMPARE_COLOR(ports.value("video"), "#00bdcc");  // cyan was too gray
        COMPARE_COLOR(ports.value("audio"), "#e9bb4f");
        COMPARE_COLOR(ports.value("boolean"), "#eb8b54");
        COMPARE_COLOR(ports.value("declared"), "#a698f2"); // the accent equals number's blue

        const look::Tokens l = look::tokens(flexokiLight, true);
        const QHash<QString, QColor> lightPorts = look::portColors(flexokiLight, true, l, &mono);
        COMPARE_COLOR(lightPorts.value("image"), "#ce5d97");
        COMPARE_COLOR(lightPorts.value("text"), "#819432"); // lifted to 3:1 against the canvas
        COMPARE_COLOR(lightPorts.value("number"), "#205ea6");
        COMPARE_COLOR(lightPorts.value("video"), "#279b91");
        COMPARE_COLOR(lightPorts.value("audio"), "#ac8613");
        COMPARE_COLOR(lightPorts.value("boolean"), "#cd7427");
        COMPARE_COLOR(lightPorts.value("declared"), "#8e7ed8");
    }

    // Owner's decision (18.2): a monochrome theme keeps its grays.
    void monochromeThemesKeepTheirGrays() {
        const QHash<QString, QColor> white = palette({{"accent", "#6e6e6e"}, {"background", "#ffffff"},
                                                      {"foreground", "#000000"}, {"red", "#2a2a2a"},
                                                      {"green", "#3a3a3a"}, {"yellow", "#4a4a4a"},
                                                      {"blue", "#1a1a1a"}, {"magenta", "#2e2e2e"},
                                                      {"cyan", "#3e3e3e"}, {"orange", "#4a4a4a"}});
        const look::Tokens t = look::tokens(white, true);
        COMPARE_COLOR(t.canvas, "#f4f4f4");
        COMPARE_COLOR(t.body, "#ffffff");
        COMPARE_COLOR(t.label, "#212121");
        bool mono = false;
        const QHash<QString, QColor> ports = look::portColors(white, true, t, &mono);
        QVERIFY(mono);
        COMPARE_COLOR(ports.value("text"), "#3a3a3a");
        COMPARE_COLOR(ports.value("number"), "#1a1a1a");
        COMPARE_COLOR(ports.value("image"), "#2e2e2e");
        COMPARE_COLOR(ports.value("declared"), "#6e6e6e");
    }

    // The Theme carries the tokens for QML (`theme.look`) and resolves port colors with them.
    void themeExposesTheLook() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("theme"));
        QFile file(dir.path() + "/theme/colors.toml");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("mode = \"dark\"\naccent = \"#7d82d9\"\nbackground = \"#060B1E\"\nforeground = \"#ffcead\"\n"
                   "red = \"#ED5B5A\"\nyellow = \"#E9BB4F\"\norange = \"#eb8b54\"\ngreen = \"#92a593\"\n"
                   "cyan = \"#a3bfd1\"\nblue = \"#7d82d9\"\nmagenta = \"#c89dc1\"\n");
        file.close();
        Theme theme(dir.path());
        auto *look = theme.look();
        QVERIFY(look);
        COMPARE_COLOR(look->property("body").value<QColor>(), "#191a29");
        COMPARE_COLOR(look->property("label").value<QColor>(), "#d1aa93");
        COMPARE_COLOR(look->property("canvas").value<QColor>(), "#060b1e");
        COMPARE_COLOR(theme.portColor("text"), "#70bc73");
        COMPARE_COLOR(theme.portColor("acme/model"), "#a698f2");
        // A layout color is a theme role (spec 4.5): a palette name or a semantic one.
        QVariantMap blue;
        QMetaObject::invokeMethod(look, "role", Q_RETURN_ARG(QVariantMap, blue), Q_ARG(QString, "blue"));
        COMPARE_COLOR(blue.value("frame").value<QColor>(), "#121731");
        COMPARE_COLOR(blue.value("tint").value<QColor>(), "#36354f");
        QVariantMap unknown;
        QMetaObject::invokeMethod(look, "role", Q_RETURN_ARG(QVariantMap, unknown), Q_ARG(QString, "no-such-role"));
        COMPARE_COLOR(unknown.value("frame").value<QColor>(), "#1a1b2a"); // muted
        // Theme::muted is unchanged for now (owner's decision 18.3).
        QCOMPARE(theme.muted(), Theme::mix(theme.background(), theme.foreground(), .65));
        // One shared shadow texture per elevation level, as a data URL.
        QVERIFY(look->property("shadow1").toString().startsWith("data:image/png;base64,"));
        QVERIFY(look->property("shadow1").toString() != look->property("shadow2").toString());
        QVERIFY(look->property("shadowMargins").toList().size() == 4);
        // Glyphs come from the bundled Lucide subset (16).
        QCOMPARE(look->property("iconFont").toString(), QString("lucide"));
        QCOMPARE(look->glyph("check"), QString(QChar(0xe070)));
        QCOMPARE(look->glyph("triangle-alert"), QString(QChar(0xe193)));
        QCOMPARE(look->glyph("no-such-glyph"), QString());
        // The grid and thumbnail shaders are compiled in (qt_add_shaders on fugue_core).
        QVERIFY(QFile::exists(":/canvas/shaders/grid.frag.qsb"));
        QVERIFY(QFile::exists(":/canvas/shaders/thumbnail.frag.qsb"));
    }

    // On every shipped Omarchy theme: labels read at 4.5:1 on all their surfaces, port marks at
    // 3:1 against the node and the canvas, titles at 4.5:1 on tinted headers and frames.
    void everyShippedThemeReads_data() {
        QTest::addColumn<QString>("path");
        const QDir themes(shippedThemes());
        for (const QString &name : themes.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (QFile::exists(themes.filePath(name + "/colors.toml")))
                QTest::newRow(qPrintable(name)) << themes.filePath(name + "/colors.toml");
        if (themes.isEmpty() || !themes.exists())
            QTest::newRow("none") << QString();
    }

    void everyShippedThemeReads() {
        QFETCH(QString, path);
        if (path.isEmpty())
            QSKIP("Omarchy's themes are not installed here");
        const Theme::Palette read = Theme::readThemeColors(path);
        QHash<QString, QColor> colors = palette({});
        for (auto it = read.colors.begin(); it != read.colors.end(); ++it)
            colors.insert(it.key(), it.value());
        const look::Tokens t = look::tokens(colors, read.light);
        for (const QColor &surface : {t.body, t.header, t.well, t.popup, t.selectedRow, t.menuRow, t.canvas})
            QVERIFY2(look::contrast(t.label, surface) >= 4.5, qPrintable(surface.name()));
        QVERIFY(look::contrast(colors.value("foreground"), t.menuRow) >= 4.5);
        for (const QColor &port : look::portColors(colors, read.light, t)) {
            QVERIFY2(look::contrast(port, t.body) >= 2.99, qPrintable(port.name()));
            QVERIFY2(look::contrast(port, t.canvas) >= 2.99, qPrintable(port.name()));
        }
        const QColor text = colors.value("foreground");
        QVERIFY(look::contrast(text, t.bypassBody) >= 4.5);
        QVERIFY(look::contrast(text, t.bypassHeader) >= 4.5);
        QVERIFY(look::contrast(t.label, t.bypassBody) >= 4.5);
        for (const char *role : {"blue", "green", "magenta", "orange"}) {
            const look::RoleTokens r = look::roleTokens(t, colors.value(role), text, read.light);
            QVERIFY2(look::contrast(text, r.tint) >= 4.5, qPrintable(role));
            QVERIFY2(look::contrast(r.frameTitle, r.frame) >= 4.5, qPrintable(role));
        }
    }

    // The corner radius setting (#63): a fixed radius for nodes and popups alike, or Omarchy's
    // (Hyprland's rounding) as before, the node corner then held to 8.
    void cornersComeFromTheSettingOrFromOmarchy() {
        const auto is = [](look::Corners c, int node, int popup) { return c.node == node && c.popup == popup; };
        QVERIFY(is(look::corners(8, 0), 8, 8));
        QVERIFY(is(look::corners(16, 3), 16, 16));
        QVERIFY(is(look::corners(0, 10), 0, 0));
        QVERIFY(is(look::corners(Settings::followOmarchy, 10), 8, 10));
        QVERIFY(is(look::corners(Settings::followOmarchy, 4), 4, 4));
        QVERIFY(is(look::corners(Settings::followOmarchy, 0), 0, 0));
    }

    // Every corner derives from the node corner r, inner ones concentric (docs/canvas-look.md 3),
    // and the shadows are painted for it, at 0, 8 and 16 on a dark and a light theme.
    void theCornersAndShadowsFollowTheRadius_data() {
        QTest::addColumn<int>("radius");
        QTest::addColumn<bool>("light");
        for (const int radius : {0, 8, 16})
            for (const bool light : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("r%1-%2").arg(radius).arg(light ? "light" : "dark")))
                    << radius << light;
    }
    void theCornersAndShadowsFollowTheRadius() {
        QFETCH(int, radius);
        QFETCH(bool, light);
        CanvasLook canvasLook;
        canvasLook.update(light ? flexokiLight : ethereal, light, look::corners(radius, 0), 12);
        QCOMPARE(canvasLook.property("corner").toInt(), radius);
        QCOMPARE(canvasLook.property("popupCorner").toInt(), radius);
        QCOMPARE(canvasLook.property("smallCorner").toInt(), std::min(radius, 4));
        QCOMPARE(canvasLook.property("thumbCorner").toInt(), std::max(0, radius - 6));
        const QVariantList margins = canvasLook.property("shadowMargins").toList();
        for (int level = 1; level <= 4; ++level) {
            int margin = 0;
            const QImage shadow = look::shadowImage(level, radius, light, &margin);
            QCOMPARE(margin, margins.at(level - 1).toInt());
            // A 9-slice: the margin, the corner, a 1 px middle that stretches, and back.
            QCOMPARE(shadow.width(), 2 * margin + 2 * radius + 3);
            QCOMPARE(shadow.height(), shadow.width());
        }
    }

    // The shadows (docs/canvas-look.md 7): a tight contact shadow and a soft ambient one, cast
    // downward; a selected node (level 2) sits higher, so its shadow is darker and reaches
    // further than a node at rest, and a node being dragged (level 4) higher still. On a dark theme the shadow is strong enough to read
    // over a near-black canvas.
    void shadowsFallDownwardAndRiseWithElevation_data() {
        QTest::addColumn<bool>("light");
        QTest::newRow("dark") << false;
        QTest::newRow("light") << true;
    }
    void shadowsFallDownwardAndRiseWithElevation() {
        QFETCH(bool, light);
        const int corner = 8;
        int rest = 0, raised = 0, lifted = 0;
        const QImage one = look::shadowImage(1, corner, light, &rest);
        const QImage two = look::shadowImage(2, corner, light, &raised);
        const QImage four = look::shadowImage(4, corner, light, &lifted);
        QVERIFY(raised > rest);
        QVERIFY(lifted > raised);
        // Alpha `d` px outside the box's bottom and top, along its middle.
        const auto below = [&](const QImage &image, int margin, int d) {
            return qAlpha(image.pixel(image.width() / 2, margin + 2 * corner + 3 + d - 1));
        };
        const auto above = [&](const QImage &image, int margin, int d) {
            return qAlpha(image.pixel(image.width() / 2, margin - d));
        };
        for (const int d : {2, 6, 12}) {
            QVERIFY2(below(one, rest, d) > above(one, rest, d), qPrintable(QString::number(d)));
            QVERIFY2(below(two, raised, d) > below(one, rest, d), qPrintable(QString::number(d)));
        }
        QVERIFY(below(four, lifted, 24) > below(two, raised, 24));
        if (!light) {
            QVERIFY2(below(one, rest, 2) >= 150, qPrintable(QString::number(below(one, rest, 2))));
            QVERIFY2(below(one, rest, 12) >= 60, qPrintable(QString::number(below(one, rest, 12))));
            QVERIFY(rest >= 24);
        }
    }

    // The Theme draws with the setting: 8 px by default, even where Hyprland's rounding is 0;
    // following Omarchy takes Hyprland's (none here, so square).
    void theThemeDrawsWithTheCornerSetting() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.look()->property("corner").toInt(), 8);
        QSignalSpy changed(&theme, &Theme::changed);
        theme.setCornerRadius(Settings::followOmarchy);
        QCOMPARE(theme.look()->property("corner").toInt(), 0);
        QCOMPARE(changed.count(), 1);
        theme.setCornerRadius(16);
        QCOMPARE(theme.look()->property("corner").toInt(), 16);
        QCOMPARE(theme.look()->property("popupCorner").toInt(), 16);
        theme.setCornerRadius(16);
        QCOMPARE(changed.count(), 2);
    }
};

QTEST_MAIN(TestLookTokens)
#include "test_look_tokens.moc"
