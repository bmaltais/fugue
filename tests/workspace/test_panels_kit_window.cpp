// The side panels, the areas and the dialogs on the Drift kit (docs/canvas-look.md 20.21; #465), in the real window, offscreen:
// the pictures to look at (FUGUE_PANELS_SHOTS), the keyboard path of each restyled dialog, and the idle check (a settled window with
// its panels open swaps no frame).

#include <QDir>
#include <QElapsedTimer>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QQuickStyle>
#include <QtTest>

#include "../canvas/canvas_app.h"
#include "../window_grab.h"
#include "../test_shard.h"

using namespace canvas_test;

namespace {

const char *graph = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox in the snow"}},
    "other": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "watercolor, soft light"}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [40, 90]}, "other": {"pos": [380, 160]}}}
})";

QQuickItem *named(App &app, const QString &name) { return item(app.window->contentItem(), name); }
QRectF sceneRect(QQuickItem *item) { return QRectF(item->mapToScene(QPointF(0, 0)), QSizeF(item->width(), item->height())); }
// The Calm look: the window tests' fixture has no gap between cards, these tests measure it.
void calm(App &app) { QCOMPARE(app.settings.applyLookPreset("calm"), QString()); }


} // namespace

class TestPanelsKitWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // Calm: a gap of window background between cards (half of 2u on each side of a border), none in Flat's panes.
    void cardsKeepAGapAndPanesNone() {
        App app(load(graph));
        QVERIFY(app.open());
        const qreal half = 4; // 2u at the 12 px text size, halved
        QTRY_COMPARE(app.canvas->x(), 44.0); // the fixture's no-gap look: the canvas right after the strip
        calm(app);
        QTRY_COMPARE(app.canvas->x(), 44.0 + half);
        QCOMPARE(app.canvas->width(), app.window->width() - 44.0 - 2 * half);
        app.commands.trigger("panel.nodes");
        QTRY_VERIFY(named(app, "nodeLibrary"));
        QQuickItem *panel = named(app, "sidePanel");
        QTRY_COMPARE(app.canvas->x(), 44.0 + app.sidePanels.width() + half);
        // The panel's card (inset half) and the canvas's are a whole gap apart.
        QCOMPARE(app.canvas->x() - (panel->x() + panel->width() - half), 2 * half);
        app.commands.trigger("area.split-row");
        QTRY_VERIFY(named(app, "areaHost"));
        QTest::qWait(100);
        QQuickItem *other = named(app, "canvasArea");
        QVERIFY(other);
        QCOMPARE(sceneRect(other).left() - sceneRect(app.canvas).right(), 2 * half);
        QCOMPARE(app.settings.applyLookPreset("flat"), QString());
        QTRY_COMPARE(sceneRect(other).left(), sceneRect(app.canvas).right()); // panes: edge to edge
    }

    // The node library by keys alone: N opens it with the focus in the search, typing filters, Down goes to the list, Down and Up move,
    // Right opens a category and Left closes it, Escape closes the panel and the focus is back on the canvas.
    void theNodeLibraryIsKeyboardOnly() {
        App app(load(graph));
        QVERIFY(app.open());
        calm(app);
        app.canvas->forceActiveFocus();
        app.key(Qt::Key_N);
        QTRY_VERIFY(named(app, "nodeLibrarySearch"));
        QTRY_VERIFY(named(app, "nodeLibrarySearch")->hasActiveFocus());
        QQuickItem *list = named(app, "nodeLibraryList");
        QVERIFY(list);
        const int rows = app.nodeLibrary.rows().size();
        QVERIFY(rows > 2);
        app.key(Qt::Key_Down);
        QTRY_VERIFY(list->hasActiveFocus());
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        app.key(Qt::Key_Down);
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        app.key(Qt::Key_Up);
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        app.key(Qt::Key_Right); // opens the category
        QTRY_VERIFY(app.nodeLibrary.rows().size() > rows);
        app.key(Qt::Key_Left);
        QTRY_COMPARE(app.nodeLibrary.rows().size(), rows);
        app.key(Qt::Key_Up); // from the first row back to the field
        QTRY_VERIFY(named(app, "nodeLibrarySearch")->hasActiveFocus());
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!named(app, "sidePanel"));
        QTRY_VERIFY(app.canvas->hasActiveFocus());
    }

    // The performance budget (docs/canvas-look.md 20.17): a settled window with a side panel and an area split open swaps no frame.
    void anIdleWindowWithPanelsDoesNotRedraw_data() {
        QTest::addColumn<QString>("preset");
        QTest::newRow("calm") << "calm";
        QTest::newRow("flat") << "flat";
    }
    void anIdleWindowWithPanelsDoesNotRedraw() {
        QFETCH(QString, preset);
        App app(load(graph));
        QVERIFY(app.open());
        QCOMPARE(app.settings.applyLookPreset(preset), QString());
        app.commands.trigger("panel.workflows");
        QTRY_VERIFY(named(app, "workflowsPanel"));
        app.commands.trigger("area.split-row");
        QTRY_VERIFY(named(app, "areaHost"));
        app.canvas->forceActiveFocus(); // a field's blinking cursor is a frame of its own
        QTest::mouseMove(app.window, QPoint(500, 500));
        int frames = 0;
        const auto connection = connect(app.window, &QQuickWindow::frameSwapped, this, [&frames] { ++frames; });
        bool quiet = false;
        for (int i = 0; i < 20 && !quiet; ++i) {
            frames = 0;
            QTest::qWait(500);
            quiet = frames == 0;
        }
        QVERIFY2(quiet, "the window never stopped drawing");
        QTest::qWait(600);
        disconnect(connection);
        QCOMPARE(frames, 0);
    }

    // FUGUE_PANELS_SHOTS=<dir> [FUGUE_SHOT_THEME=<omarchy theme>] [FUGUE_SHOT_LOOK="preset=flat"] [QT_QUICK_BACKEND=rhi]: the main window with
    // its panels, an area split and a context menu, at the two sizes of the owner's checks (one theme and look per process).
    void screenshots() {
        const QString out = qEnvironmentVariable("FUGUE_PANELS_SHOTS");
        if (out.isEmpty())
            QSKIP("Set FUGUE_PANELS_SHOTS to a folder to write the pictures there.");
        QDir().mkpath(out);
        const QString tag = qEnvironmentVariable("FUGUE_SHOT_TAG", QStringLiteral("shot"));
        App app(load(graph), {}, Look{qEnvironmentVariable("FUGUE_SHOT_THEME", QStringLiteral("tokyo-night")), 12});
        QVERIFY(app.open());
        for (const QString &part : qEnvironmentVariable("FUGUE_SHOT_LOOK").split(';', Qt::SkipEmptyParts)) {
            const QStringList kv = part.split('=');
            QVERIFY(kv.size() == 2);
            QCOMPARE(kv[0] == "preset" ? app.settings.applyLookPreset(kv[1]) : app.settings.set(kv[0], kv[1].toDouble()), QString());
        }
        const auto save = [&](const QString &what, const QSize &size) {
            QTest::qWait(300);
            QVERIFY(test_support::grab(app.window).save(QStringLiteral("%1/%2-%3-%4x%5.png").arg(out, tag, what).arg(size.width()).arg(size.height())));
        };
        for (const QSize size : {QSize(1888, 971), QSize(900, 700)}) {
            app.window->resize(size);
            QTest::mouseMove(app.window, QPoint(size.width() / 2, size.height() / 2)); // no tooltip over the panels
            QTest::qWait(100);
            save("graph", size);
            app.commands.trigger("panel.nodes");
            QTRY_VERIFY(named(app, "nodeLibrary"));
            save("library", size);
            app.commands.trigger("panel.workflows");
            QTRY_VERIFY(named(app, "workflowsPanel"));
            save("workflows", size);
            app.commands.trigger("panel.workflows");
            QTRY_VERIFY(!named(app, "sidePanel"));
            app.commands.trigger("area.split-row");
            QTRY_VERIFY(named(app, "areaHost"));
            QTest::qWait(200);
            save("areas", size);
            app.commands.trigger("area.close");
            QTRY_VERIFY(!named(app, "areaHost"));
            app.window->requestActivate();
            QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(120, 110));
            QTest::qWait(300);
            save("menu", size);
            QTest::keyClick(app.window, Qt::Key_Escape);
        }
    }
};

FUGUE_TEST_MAIN(TestPanelsKitWindow, QGuiApplication)
#include "test_panels_kit_window.moc"
