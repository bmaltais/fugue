// The Drift kit's controls (src/kit/, docs/canvas-look.md "Drift kit") in a window of their own: a slider's value, steps and keys,
// a switch's key and click, a row's click reaching its switch and leaving a chip in the row alone. With FUGUE_KIT_SHOTS set to a folder,
// the gallery (tests/kit/KitGallery.qml) is also saved as pictures, a dark and a light theme, narrow and wide, and the dusk header with no picture:
//
//   FUGUE_KIT_SHOTS=~/.cache/fugue-ui-kit QT_QUICK_BACKEND=rhi build/test_kit shots

#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRadialGradient>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/registry.h"
#include "qml_app.h"
#include "theme.h"
#include "translator.h"
#include "workflow/workflows.h"
#include "../window_grab.h"

using namespace fugue;

namespace {

const char *windowQml = R"(
import QtQuick
import Fugue
import "qrc:/kit"

Window {
    id: window
    width: 420
    height: 360
    visible: true
    property real level: 5
    property bool on: false
    property int mode: 0
    property int chips: 0
    property int steps: 0
    property int frosts: 0
    Column {
        width: parent.width
        Rectangle {
            id: sky
            width: 200
            height: 50
            color: "#c9a4c8"
            KitChip { objectName: "frostChip"; x: 10; y: 8; frosted: true; backdrop: sky; text: "Frost"; onTapped: window.frosts++ }
        }
        KitGroup {
            id: group
            objectName: "group"
            KitRow {
                objectName: "toggleRow"
                label: "Follow the day"
                hint: "Local light"
                target: toggle
                KitChip { objectName: "chip"; text: "Here"; onTapped: window.chips++ }
                KitToggle { id: toggle; objectName: "toggle"; checked: window.on; onToggled: v => { window.on = v; window.steps++ } }
            }
            KitRow {
                label: "Seed"
                KitSegmented {
                    objectName: "seg"
                    options: ["Fixed", "Random", "Increment"]
                    currentIndex: window.mode
                    onMoved: i => window.mode = i
                }
            }
            KitRow {
                label: "Blobs"
                KitSlider {
                    id: slider
                    objectName: "slider"
                    from: 2; to: 8; stepSize: 1; ticks: true; showValue: true; value: window.level
                    onMoved: v => window.level = v
                }
            }
        }
    }
}
)";

void writeScene(const QString &path) {
    QImage image(760, 280, QImage::Format_RGB32);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient sky(0, 0, 760, 280);
    sky.setColorAt(0, QColor("#7aa2c8"));
    sky.setColorAt(0.5, QColor("#c9a4c8"));
    sky.setColorAt(1, QColor("#f0c0a0"));
    p.fillRect(image.rect(), sky);
    QRadialGradient sun(560, 90, 160);
    sun.setColorAt(0, QColor(255, 240, 210, 230));
    sun.setColorAt(1, QColor(255, 240, 210, 0));
    p.fillRect(image.rect(), sun);
    p.end();
    image.save(path);
}

} // namespace

class TestKit : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    QString themeFolder; // the folder a Theme reads Omarchy's colors from
    QStringList warnings;

    // A theme as the window reads it: a shipped Omarchy theme's palette (empty: Theme's fallbacks).
    struct Shell {
        Translator translator;
        Theme theme;
        Registry registry;
        CanvasModel model{&registry};
        CommandRegistry commands;
        Workflows workflows;
        QQmlEngine engine;
        Shell(const QString &home, const QString &name)
            : translator(home + "/language"), theme(prepare(home, name)),
              workflows(&model, &registry, &commands, {home + "/recovery", home + "/trust.json"}) {
            exposeToQml(engine, {&translator, &theme, &commands, &model, &workflows});
        }
        static QString prepare(const QString &home, const QString &name) {
            if (!name.isEmpty()) {
                const QString from = QStringLiteral("/usr/share/omarchy/themes/") + name;
                QDir().mkpath(home + "/theme");
                QFile::remove(home + "/theme/colors.toml");
                QFile::remove(home + "/theme/light.mode");
                QFile::copy(from + "/colors.toml", home + "/theme/colors.toml");
                if (QFile::exists(from + "/light.mode"))
                    QFile::copy(from + "/light.mode", home + "/theme/light.mode");
            }
            return home;
        }
    };

    QQuickWindow *show(Shell &shell, const QByteArray &qml, const QUrl &url, const QVariantMap &initial = {}) {
        QObject::connect(&shell.engine, &QQmlEngine::warnings, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        QQmlComponent component(&shell.engine);
        component.setData(qml, url);
        QObject *made = component.createWithInitialProperties(initial);
        if (!made)
            qFatal("%s", qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(made);
        window->show();
        return window;
    }

    static QPointF centerOf(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)); }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // A test run by hand from a desktop shell still never opens a window on the owner's screen (tests/test_environment.cpp).
    void aTestBinaryRunsOffscreenWhateverTheShellSays() {
        if (qgetenv("FUGUE_TEST_ON_SCREEN") == "1")
            QSKIP("FUGUE_TEST_ON_SCREEN asks for a real window");
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
        QVERIFY(qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"));
    }

    void kitControlsKeepTheirContract() {
        const QString home = dir.filePath("controls");
        Shell shell(home, "tokyo-night");
        QQuickWindow *window = show(shell, windowQml, QUrl("qrc:/test_kit.qml"));
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *slider = window->findChild<QQuickItem *>("slider");
        auto *toggle = window->findChild<QQuickItem *>("toggle");
        auto *chip = window->findChild<QQuickItem *>("chip");
        auto *row = window->findChild<QQuickItem *>("toggleRow");
        QVERIFY(slider && toggle && chip && row);

        // The slider: keys move it by a step, the ends are Home and End, it never leaves its range, and it asks (`moved`)
        // rather than sets: the window's binding is what changes `value`.
        slider->forceActiveFocus();
        QTRY_VERIFY(slider->hasActiveFocus());
        QCOMPARE(slider->property("value").toDouble(), 5.0);
        QTest::keyClick(window, Qt::Key_Right);
        QTRY_COMPARE(window->property("level").toDouble(), 6.0);
        QTest::keyClick(window, Qt::Key_Left);
        QTest::keyClick(window, Qt::Key_Left);
        QTRY_COMPARE(window->property("level").toDouble(), 4.0);
        QTest::keyClick(window, Qt::Key_End);
        QTRY_COMPARE(window->property("level").toDouble(), 8.0);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(window->property("level").toDouble(), 8.0);
        QTest::keyClick(window, Qt::Key_Home);
        QTRY_COMPARE(window->property("level").toDouble(), 2.0);
        // A press on the rail picks the value under it, snapped to a step: the middle of the rail is the middle of 2 to 8.
        const QPointF mid = slider->mapToScene(QPointF(slider->property("railWidth").toDouble() / 2, slider->height() / 2));
        QTest::mouseClick(window, Qt::LeftButton, {}, mid.toPoint());
        QTRY_COMPARE(window->property("level").toDouble(), 5.0);

        // The chooser: one tab stop, the arrows move and choose (never past the ends), Home and End jump, a press on an option chooses it,
        // Space and Enter keep the choice, and it asks (`moved`) rather than sets.
        auto *seg = window->findChild<QQuickItem *>("seg");
        QVERIFY(seg);
        seg->forceActiveFocus();
        QTRY_VERIFY(seg->hasActiveFocus());
        QCOMPARE(window->property("mode").toInt(), 0);
        QTest::keyClick(window, Qt::Key_Left);
        QCOMPARE(window->property("mode").toInt(), 0);
        QTest::keyClick(window, Qt::Key_Right);
        QTRY_COMPARE(window->property("mode").toInt(), 1);
        QTest::keyClick(window, Qt::Key_Space);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(window->property("mode").toInt(), 1);
        QTest::keyClick(window, Qt::Key_End);
        QTRY_COMPARE(window->property("mode").toInt(), 2);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(window->property("mode").toInt(), 2);
        QTest::keyClick(window, Qt::Key_Home);
        QTRY_COMPARE(window->property("mode").toInt(), 0);
        QTest::mouseClick(window, Qt::LeftButton, {}, seg->mapToScene(QPointF(seg->width() - 10, seg->height() / 2)).toPoint());
        QTRY_COMPARE(window->property("mode").toInt(), 2);
        QCOMPARE(seg->property("currentIndex").toInt(), 2);

        // The switch: Space asks for the opposite, and the window's answer is what shows.
        toggle->forceActiveFocus();
        QTRY_VERIFY(toggle->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(window->property("on").toBool());
        QTRY_VERIFY(toggle->property("checked").toBool());
        QTest::mouseClick(window, Qt::LeftButton, {}, centerOf(toggle).toPoint());
        QTRY_VERIFY(!window->property("on").toBool());
        QCOMPARE(window->property("steps").toInt(), 2);

        // A click on the row, away from its controls, is the switch's click; on the chip it is only the chip's.
        QTest::mouseClick(window, Qt::LeftButton, {}, row->mapToScene(QPointF(30, row->height() / 2)).toPoint());
        QTRY_VERIFY(window->property("on").toBool());
        QTest::mouseClick(window, Qt::LeftButton, {}, centerOf(chip).toPoint());
        QTRY_COMPARE(window->property("chips").toInt(), 1);
        QCOMPARE(window->property("steps").toInt(), 3);
        QVERIFY(window->property("on").toBool());
        // A frosted chip, over a picture, presses by click and by Space as any chip does.
        auto *frost = window->findChild<QQuickItem *>("frostChip");
        QVERIFY(frost);
        QTest::mouseClick(window, Qt::LeftButton, {}, centerOf(frost).toPoint());
        QTRY_COMPARE(window->property("frosts").toInt(), 1);
        frost->forceActiveFocus();
        QTRY_VERIFY(frost->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(window->property("frosts").toInt(), 2);
        // The frost setting at 0 makes it the plain chip: no glass, still pressed the same.
        QVERIFY(frost->property("glass").toBool());
        shell.theme.look()->setFrost(0);
        QTRY_VERIFY(!frost->property("glass").toBool());
        QTest::mouseClick(window, Qt::LeftButton, {}, centerOf(frost).toPoint());
        QTRY_COMPARE(window->property("frosts").toInt(), 3);
        shell.theme.look()->setFrost(0.6);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join("\n")));
        delete window;
    }

    void shots_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("tag");
        QTest::addColumn<int>("cardWidth");
        QTest::addColumn<bool>("picture");
        QTest::addColumn<int>("heroMin");
        QTest::addColumn<QString>("tone");
        QTest::newRow("dark narrow") << "tokyo-night" << "dark-380" << 380 << true << 28 << "dusk";
        QTest::newRow("light narrow") << "catppuccin-latte" << "light-380" << 380 << true << 28 << "dusk";
        QTest::newRow("dusk dark") << "tokyo-night" << "dusk-dark-380" << 380 << false << 0 << "dusk";
        QTest::newRow("dusk light") << "catppuccin-latte" << "dusk-light-380" << 380 << false << 45 << "dusk";
        QTest::newRow("dark wide") << "tokyo-night" << "dark-720" << 720 << true << 28 << "dusk";
        for (const char *tone : {"sea", "ember", "moss", "slate"})
            QTest::newRow(tone) << "tokyo-night" << QString("tone-%1").arg(tone) << 380 << false << 0 << QString(tone);
        QTest::newRow("light wide") << "catppuccin-latte" << "light-720" << 720 << true << 28 << "dusk";
    }

    // Pictures of the gallery, only when asked for: the folder is the caller's, nothing else is written.
    void shots() {
        const QString folder = qEnvironmentVariable("FUGUE_KIT_SHOTS");
        if (folder.isEmpty())
            QSKIP("FUGUE_KIT_SHOTS is not set");
        QFETCH(QString, theme);
        QFETCH(QString, tag);
        QFETCH(int, cardWidth);
        QFETCH(bool, picture);
        QFETCH(int, heroMin);
        QFETCH(QString, tone);
        QDir().mkpath(folder);
        const QString scene = dir.filePath("scene.png");
        writeScene(scene);
        QFile gallery(QStringLiteral(FUGUE_SOURCE_DIR "/tests/kit/KitGallery.qml"));
        QVERIFY(gallery.open(QIODevice::ReadOnly));
        const QByteArray body = gallery.readAll();
        Shell shell(dir.filePath("shot-" + tag), theme);
        QQmlComponent component(&shell.engine);
        component.setData(body, QUrl::fromLocalFile(gallery.fileName()));
        QObject *made = component.createWithInitialProperties({{"heroSource", picture ? QUrl::fromLocalFile(scene) : QUrl()}, {"cardWidth", cardWidth}, {"heroMin", heroMin}, {"heroTone", tone}});
        QVERIFY2(made, qPrintable(component.errorString()));
        auto *item = qobject_cast<QQuickItem *>(made);
        QVERIFY(item);
        QQuickWindow window;
        item->setParentItem(window.contentItem());
        window.resize(int(item->width()), int(item->height()));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        // The hero's picture loads off the main thread: wait for it.
        auto *hero = item->findChild<QQuickItem *>("hero");
        QVERIFY(hero);
        QTest::qWait(300);
        window.resize(int(item->width()), int(item->height()));
        QTest::qWait(100);
        const QImage image = test_support::grab(&window);
        QVERIFY(!image.isNull());
        QVERIFY(image.save(folder + "/" + tag + ".png"));
        delete item;
    }
};

QTEST_MAIN(TestKit)
#include "test_kit.moc"
