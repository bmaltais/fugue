// The canvas in the real window (Main.qml, offscreen): how fast it opens with a large graph,
// and what the mouse, wheel, trackpad and keyboard do on it (spec 12.1 to 12.3, 12.6).

#include <atomic>
#include <ctime>
#include <QElapsedTimer>
#include <QVariantAnimation>
#include <QJsonArray>
#include <QJsonObject>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtTest>

#include "backend.h"
#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"
#include "theme.h"
#include "workflow/workflows.h"
#include "translator.h"

using namespace fugue;

namespace {

QQuickItem *item(QQuickItem *from, const QString &name) {
    for (auto *child : from->childItems()) {
        if (child->objectName() == name)
            return child;
        if (auto *found = item(child, name))
            return found;
    }
    return nullptr;
}

// Every item under `from`, itself included: what the scene graph has to walk.
int countItems(QQuickItem *from) {
    int count = 1;
    for (auto *child : from->childItems())
        count += countItems(child);
    return count;
}

Document load(const QByteArray &json) {
    const Result<Document> loaded = Document::load(json);
    if (!loaded.ok())
        qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
    return loaded.value();
}

// Two Load Image nodes feeding a grid of `count` Blend nodes, each linked from its left and
// upper neighbours (or a Load Image at the edges): 2 * count wires.
Document grid(int count, int columns = 20) {
    QJsonObject nodes, positions;
    nodes.insert("src-a", QJsonObject{{"type", "fugue/load-image"}, {"version", "1.0.0"}, {"inputs", QJsonObject{}}});
    nodes.insert("src-b", QJsonObject{{"type", "fugue/load-image"}, {"version", "1.0.0"}, {"inputs", QJsonObject{}}});
    positions.insert("src-a", QJsonObject{{"pos", QJsonArray{-300, 0}}});
    positions.insert("src-b", QJsonObject{{"pos", QJsonArray{-300, 200}}});
    auto id = [](int i) { return QStringLiteral("n%1").arg(i); };
    for (int i = 0; i < count; ++i) {
        const int row = i / columns, column = i % columns;
        const QString base = column > 0 ? id(i - 1) : QStringLiteral("src-a");
        const QString top = row > 0 ? id(i - columns) : QStringLiteral("src-b");
        nodes.insert(id(i), QJsonObject{{"type", "fugue/blend"},
                                        {"version", "1.0.0"},
                                        {"inputs", QJsonObject{{"base", QJsonObject{{"from", base + ".image"}}},
                                                               {"top", QJsonObject{{"from", top + ".image"}}}}}});
        positions.insert(id(i), QJsonObject{{"pos", QJsonArray{column * 260, row * 160}}});
    }
    Document doc = load(R"({"fugue": 1, "graph": {"nodes": {}}})");
    const QJsonObject json{{"fugue", 1},
                           {"graph", QJsonObject{{"nodes", nodes}}},
                           {"layout", QJsonObject{{"nodes", positions}}}};
    const Result<Document> made = Document::fromJson(json);
    if (!made.ok())
        qFatal("grid does not load: %s", qPrintable(made.error().message));
    return made.value();
}

// prompt (Text) and gen (Generate Image), unlinked, and a Save Image; the view at zoom 1
// with the world's origin at the window's top left.
const char *small = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [20, 100]}, "gen": {"pos": [340, 60]}, "save": {"pos": [660, 300]}},
             "view": {"center": [480, 340], "zoom": 1}}
})";

// A theme folder with this desktop text size.
QString themeFolder(const QTemporaryDir &dir, int textSize) {
    QFile shell(dir.filePath("shell.toml"));
    if (shell.open(QIODevice::WriteOnly))
        shell.write(QStringLiteral("[font]\nbase-size = %1\n").arg(textSize).toUtf8());
    return dir.path();
}

// The app's window around a canvas model, as main() makes it.
struct App {
    QTemporaryDir dir;
    Translator translator{dir.path() + "/language"};
    Theme theme;
    Backend backend;
    Registry registry;
    CanvasModel model{&registry};
    CommandRegistry commands;
    Workflows workflows{&model, &registry, &commands, {dir.path() + "/recovery", dir.path() + "/trust.json"}};
    QQmlApplicationEngine engine;
    QStringList warnings;
    QQuickWindow *window = nullptr;
    QQuickItem *canvas = nullptr;
    QQuickItem *world = nullptr;
    qint64 openedMs = 0;
    qint64 createdMs = 0; // the QML objects made, before the first frame
    // The CPU time the process spent to the first frame: what opening costs, unlike the wall
    // clock not stretched when other programs (ctest -j8) take the CPU meanwhile.
    qint64 openedCpuMs = 0;

    explicit App(const Document &document, int textSize = 12) : theme(themeFolder(dir, textSize)) {
        nodes::addBuiltinNodeTypes(registry);
        registerDefaultCommands(commands);
        model.setDocument(document);
        QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("canvasModel", &model);
        engine.rootContext()->setContextProperty("commands", &commands);
        engine.rootContext()->setContextProperty("workflows", &workflows);
    }

    static qint64 cpuMs() {
        timespec now{};
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
        return qint64(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
    }

    bool open() {
        QElapsedTimer timer;
        timer.start();
        const qint64 cpuStart = cpuMs();
        engine.load(QUrl("qrc:/Main.qml"));
        if (engine.rootObjects().isEmpty())
            return false;
        createdMs = timer.elapsed();
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        bool swapped = false;
        const QMetaObject::Connection first =
            QObject::connect(window, &QQuickWindow::frameSwapped, [&swapped] { swapped = true; });
        // Up means a first frame on screen.
        const bool up = QTest::qWaitForWindowExposed(window) && QTest::qWaitFor([&] { return swapped; }, 5000);
        QObject::disconnect(first);
        if (!up)
            return false;
        openedMs = timer.elapsed();
        openedCpuMs = cpuMs() - cpuStart;
        window->requestActivate();
        if (!QTest::qWaitForWindowActive(window))
            return false;
        canvas = item(window->contentItem(), "canvas");
        world = item(window->contentItem(), "world");
        return canvas && world;
    }

    qreal zoom() const { return world->scale(); }
    QPointF pan() const { return world->position(); }
    // A world point in window coordinates.
    QPoint screen(qreal x, qreal y) const {
        return canvas->mapToScene(QPointF(world->x() + x * zoom(), world->y() + y * zoom())).toPoint();
    }

    void drag(Qt::MouseButton button, QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = {}) {
        QTest::mousePress(window, button, modifiers, from);
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(window, from + (to - from) * i / 8);
        QTest::mouseRelease(window, button, modifiers, to);
    }

    void wheel(QPoint at, QPoint angle, QPoint pixels = {}, Qt::KeyboardModifiers modifiers = {}) {
        QWheelEvent event(at, window->mapToGlobal(at), pixels, angle, Qt::NoButton, modifiers, Qt::NoScrollPhase,
                          false);
        QCoreApplication::sendEvent(window, &event);
    }
};

} // namespace

class TestCanvasWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void opensQuicklyWithThreeHundredNodesAndSixHundredWires() {
        App app(grid(300));
        QCOMPARE(app.model.links()->rowCount(), 600);
        QVERIFY(app.open());
        // 12.1: up in well under half a second. Offscreen this is the QML and scene graph
        // work without a GPU; the budget leaves room for a slow CI machine.
        const int items = countItems(app.window->contentItem());
        qInfo("window up with 302 nodes and 600 wires in %lld ms, %lld ms of CPU (objects made in %lld ms); "
              "%d items", app.openedMs, app.openedCpuMs, app.createdMs, items);
        // 12.1's target is the window itself (opensQuicklyEmpty); a 300-node file gets a
        // looser bound, so a slow CI machine does not fail it. It is on CPU time: the wall
        // clock also counts the time other tests take the CPU under ctest -j8, which made it
        // flaky. The wall clock only catches a hang.
        QVERIFY2(app.openedCpuMs < 500, qPrintable(QString::number(app.openedCpuMs)));
        QVERIFY2(app.openedMs < 3000, qPrintable(QString::number(app.openedMs)));
        // What the time grows with, counted, so it can be checked without a clock: 3534 items
        // on 2026-11-19 (the faces and rows of the 44 nodes in view, two items for each other
        // node, a shadow each, a shape per wire). One more item on every node is 302 more.
        QVERIFY2(items < 3800, qPrintable(QString::number(items)));
        QCOMPARE(item(app.window->contentItem(), "nodes")->property("count").toInt(), 302);
        QCOMPARE(item(app.window->contentItem(), "wires")->property("count").toInt(), 600);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void opensQuicklyEmpty() {
        App app(load(R"({"fugue": 1, "graph": {"nodes": {}}})"));
        QVERIFY(app.open());
        qInfo("empty window up in %lld ms", app.openedMs);
        QVERIFY2(app.openedMs < 100, qPrintable(QString::number(app.openedMs)));
    }

    void nodesDrawTheirPortsOnceInViewAndNotWhenZoomedOut() {
        App app(grid(300));
        QVERIFY(app.open());
        QObject *nodes = item(app.window->contentItem(), "nodes");
        auto node = [&](const QString &id) -> QQuickItem * {
            for (int i = 0; i < nodes->property("count").toInt(); ++i) {
                QQuickItem *found = nullptr;
                QMetaObject::invokeMethod(nodes, "itemAt", Q_RETURN_ARG(QQuickItem *, found), Q_ARG(int, i));
                if (found && found->property("nodeId").toString() == id)
                    return found;
            }
            return nullptr;
        };
        // n0 is on screen; n19, at x = 4940, is far to the right.
        QVERIFY(node("n0")->property("rowsMade").toBool());
        QVERIFY(!node("n19")->property("rowsMade").toBool());
        app.wheel({500, 300}, {0, 0}, {-4600, 0}); // a trackpad swipe to the right end
        QTRY_VERIFY(node("n19")->property("rowsMade").toBool());
        QCOMPARE(node("n19")->property("tier").toInt(), 0);
        // Zoomed out the nodes shed detail by tier (docs/canvas-look.md 14): the map below
        // 0.5, the overview below 0.25.
        for (int i = 0; i < 10; ++i)
            app.wheel({500, 300}, {0, -120});
        QVERIFY(app.zoom() < 0.5 && app.zoom() >= 0.25);
        QCOMPARE(node("n19")->property("tier").toInt(), 1);
        for (int i = 0; i < 8; ++i)
            app.wheel({500, 300}, {0, -120});
        QVERIFY(app.zoom() < 0.25);
        QCOMPARE(node("n19")->property("tier").toInt(), 2);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // 12.1's frame rate needs a real display: offscreen frames say nothing. Opt in with
    //   FUGUE_CANVAS_BENCH=1 QSG_RENDER_TIMING=1 QT_QPA_PLATFORM=xcb build/test_canvas_window benchmark
    // It pans and drags a node for 3 s each at a zoom that shows the grid's text, with the node
    // shadows and without, and pans zoomed out, printing frames per second, the longest gap
    // between two frames and the render thread's time a frame.
    // Measured 2026-11-19 (Qt 6.12, RTX 3090, 60 Hz display through XWayland, 302 nodes and
    // 600 wires, every node's rows made), before the canvas look (#47): pan 60.5 fps, drag
    // 60.1, pan zoomed out 60.5; worst gap 21 to 22 ms (one late frame at most); render thread
    // 2.1 ms a frame on average while dragging, under 0.5 ms while panning.
    // After it, two runs: pan 60.1 to 60.3 fps, drag 59.9 to 60.2, zoomed out 60.5 to 60.8;
    // worst gap 19 to 27 ms; render 0.3 to 1.7 ms panning, 2.1 to 2.5 ms dragging, 0.15 ms
    // zoomed out. The shadows cost 0.45 ms at most (drag 2.54 on, 2.08 off; the other run
    // 2.31 on, 2.41 off), inside docs/canvas-look.md 7's 1 ms, so nodes at rest keep theirs.
    void benchmark() {
        if (!qEnvironmentVariableIsSet("FUGUE_CANVAS_BENCH"))
            QSKIP("set FUGUE_CANVAS_BENCH=1 and run on a real display");
        App app(grid(300));
        QVERIFY(app.open());
        app.window->resize(1600, 1000);
        QElapsedTimer clock;
        qint64 last = 0, worst = 0;
        int frames = 0;
        connect(app.window, &QQuickWindow::frameSwapped, this, [&] {
            const qint64 now = clock.elapsed();
            worst = std::max(worst, now - last);
            last = now;
            ++frames;
        });
        // The render thread's time per frame, in microseconds: scene graph rendering from
        // beforeRendering to afterRendering (QSG_RENDER_TIMING only prints whole milliseconds).
        std::atomic<qint64> renderNs = 0, renderStart = 0;
        std::atomic<int> rendered = 0;
        QElapsedTimer renderClock;
        renderClock.start();
        connect(app.window, &QQuickWindow::beforeRendering, this, [&] { renderStart = renderClock.nsecsElapsed(); },
                Qt::DirectConnection);
        connect(app.window, &QQuickWindow::afterRendering, this, [&] {
            renderNs += renderClock.nsecsElapsed() - renderStart;
            ++rendered;
        }, Qt::DirectConnection);
        auto measure = [&](const QByteArray &name, const std::function<void(qreal)> &step) {
            QVariantAnimation animation;
            animation.setStartValue(0.0);
            animation.setEndValue(1.0);
            animation.setDuration(3000);
            connect(&animation, &QVariantAnimation::valueChanged, this, [&](const QVariant &t) { step(t.toReal()); });
            frames = 0;
            worst = 0;
            renderNs = 0;
            rendered = 0;
            clock.restart();
            last = 0;
            animation.start();
            QTRY_VERIFY_WITH_TIMEOUT(animation.state() == QAbstractAnimation::Stopped, 10000);
            qInfo("RESULT %s: %.1f fps, worst gap %lld ms, render %.0f us a frame", name.constData(),
                  frames * 1000.0 / clock.elapsed(), worst, rendered ? renderNs / 1000.0 / rendered : 0.0);
        };
        // Make every node's rows first (a swipe across the whole grid), the worst case.
        app.canvas->setProperty("navigation", "legacy");
        for (int i = 0; i < 6; ++i)
            app.wheel({0, 0}, {0, -120}); // zoom 0.56
        for (int x = 0; x < 6000; x += 400)
            for (int y = 0; y < 2600; y += 400) {
                app.world->setPosition(QPointF(-x * app.zoom(), -y * app.zoom()));
                QCoreApplication::processEvents();
            }
        app.world->setPosition({0, 0});
        QTest::qWait(500);
        const QPointF origin = app.world->position();
        // With the node shadows and without them, for docs/canvas-look.md 7's budget (at most
        // 1 ms of render time at 300 nodes).
        QQuickItem *shadows = item(app.window->contentItem(), "shadows");
        QVERIFY(shadows);
        for (const bool on : {true, false}) {
            shadows->setOpacity(on ? 1 : 0);
            const QByteArray which = on ? "shadows on" : "shadows off";
            measure("pan at zoom 0.56, " + which, [&](qreal t) {
                app.world->setX(origin.x() - 1500 * std::sin(t * M_PI));
            });
            app.model.beginMove("n45");
            measure("drag a node at zoom 0.56, " + which, [&](qreal t) {
                app.model.moveBy(200 * std::cos(t * 4 * M_PI) - 200, 200 * std::sin(t * 4 * M_PI));
            });
            app.model.endMove();
        }
        shadows->setOpacity(1);
        for (int i = 0; i < 14; ++i)
            app.wheel({0, 0}, {0, -120}); // zoom 0.15: the whole grid, without text
        measure("pan zoomed out to 0.15", [&](qreal t) {
            app.world->setX(-300 * std::sin(t * M_PI));
        });
    }

    void wiresMeetTheirPortsAtAnyTextSize() {
        App app(load(small), 15);
        QVERIFY(app.open());
        QCOMPARE(app.theme.textSize(), 15);
        QCOMPARE(app.model.rowHeight(), 30.0);
        QVERIFY(app.model.connectPorts("prompt", "text", "gen", "prompt"));
        QQuickItem *wires = item(app.window->contentItem(), "wires");
        QQuickItem *wire = nullptr;
        QMetaObject::invokeMethod(wires, "itemAt", Q_RETURN_ARG(QQuickItem *, wire), Q_ARG(int, 0));
        QVERIFY(wire);
        const QString path = app.model.links()->data(app.model.links()->index(0, 0), LinkList::Path).toString();
        QCOMPARE(wire->property("path").toString(), path);
        // Text's output, at the end of its 300-wide node (60u) on its first row: 100 + 35 + 5 + 15.
        QVERIFY2(path.startsWith("M 320.0 155.0 "), qPrintable(path));
    }

    void zoomedOutAPressOnANodeMovesIt() {
        App app(load(small));
        QVERIFY(app.open());
        for (int i = 0; i < 20; ++i)
            app.wheel({0, 0}, {0, -120}); // zoom 0.15: ports are not drawn
        QVERIFY(app.zoom() < 0.2);
        // 40 world units from Text's input port, inside the node.
        app.drag(Qt::LeftButton, app.screen(60, 140), app.screen(60, 140) + QPoint(30, 0));
        QCOMPARE(app.model.links()->rowCount(), 0);
        QVERIFY2(app.model.position("prompt").x() > 100, qPrintable(QString::number(app.model.position("prompt").x())));
    }

    void theLayoutsViewIsWhereItOpens() {
        App app(load(small));
        QVERIFY(app.open());
        QCOMPARE(app.zoom(), 1.0);
        // The window is 960 x 680, so the center (480, 340) is the world's origin plus itself.
        QCOMPARE(app.pan(), QPointF(0, 0));
    }

    void theWheelZoomsAtThePointer() {
        App app(load(small));
        QVERIFY(app.open());
        const QPoint at(500, 300);
        app.wheel(at, {0, 120});
        QVERIFY(qAbs(app.zoom() - 1.1) < 1e-9);
        // The world point under the pointer stays under it.
        QCOMPARE(app.screen(500, 300), at);
        for (int i = 0; i < 100; ++i)
            app.wheel(at, {0, 120});
        QCOMPARE(app.zoom(), 4.0);
        for (int i = 0; i < 100; ++i)
            app.wheel(at, {0, -120});
        QCOMPARE(app.zoom(), 0.1);
        // The model keeps the view for the file.
        QCOMPARE(app.model.document().layout.value("view").toObject().value("zoom").toDouble(), 0.1);
    }

    void aLeftDragOnEmptyCanvasPans() {
        App app(load(small));
        QVERIFY(app.open());
        app.drag(Qt::LeftButton, {500, 600}, {560, 640});
        QCOMPARE(app.pan(), QPointF(60, 40));
        QVERIFY(app.model.selectedIds().isEmpty());
    }

    void middleDragAndSpaceDragPanRightDragDoesNot() {
        App app(load(small));
        QVERIFY(app.open());
        app.drag(Qt::MiddleButton, {100, 130}, {130, 130}); // over prompt: still a pan
        QCOMPARE(app.pan(), QPointF(30, 0));
        app.drag(Qt::RightButton, {500, 600}, {600, 650});
        QCOMPARE(app.pan(), QPointF(30, 0));
        QTest::keyPress(app.window, Qt::Key_Space);
        QVERIFY(app.canvas->property("spaceHeld").toBool());
        app.drag(Qt::LeftButton, app.screen(100, 130), app.screen(100, 150)); // over a node, Space held
        QTest::keyRelease(app.window, Qt::Key_Space);
        QCOMPARE(app.pan(), QPointF(30, 20));
        QCOMPARE(app.model.position("prompt"), QPointF(20, 100));
    }

    void aTrackpadPansAndPinchZooms() {
        App app(load(small));
        QVERIFY(app.open());
        app.wheel({500, 300}, {0, 24}, {-12, 18});
        QCOMPARE(app.pan(), QPointF(-12, 18));
        QCOMPARE(app.zoom(), 1.0);
        QNativeGestureEvent pinch(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2, QPointF(500, 300),
                                  QPointF(500, 300), app.window->mapToGlobal(QPointF(500, 300)), 0.25, QPointF());
        QNativeGestureEvent begin(Qt::BeginNativeGesture, QPointingDevice::primaryPointingDevice(), 2, QPointF(500, 300),
                                  QPointF(500, 300), app.window->mapToGlobal(QPointF(500, 300)), 0, QPointF());
        QNativeGestureEvent end(Qt::EndNativeGesture, QPointingDevice::primaryPointingDevice(), 2, QPointF(500, 300),
                                QPointF(500, 300), app.window->mapToGlobal(QPointF(500, 300)), 0, QPointF());
        QCoreApplication::sendEvent(app.window, &begin);
        QCoreApplication::sendEvent(app.window, &pinch);
        QCoreApplication::sendEvent(app.window, &end);
        QTRY_VERIFY2(app.zoom() > 1.1, qPrintable(QString::number(app.zoom())));
    }

    void theStandardPresetSelectsWithADragAndPansWithTheWheel() {
        App app(load(small));
        QVERIFY(app.open());
        app.canvas->setProperty("navigation", "standard");
        app.drag(Qt::LeftButton, app.screen(0, 40), app.screen(600, 500));
        QCOMPARE(app.pan(), QPointF(0, 0));
        QCOMPARE(app.model.selectedIds(), (QStringList{"gen", "prompt"}));
        app.wheel({500, 300}, {0, -120});
        QCOMPARE(app.pan(), QPointF(0, -60));
        QCOMPARE(app.zoom(), 1.0);
        app.wheel({500, 300}, {0, 120}, {}, Qt::ControlModifier);
        QVERIFY(qAbs(app.zoom() - 1.1) < 1e-9);
    }

    void ctrlDragSelectsShiftAddsAltSubtracts() {
        App app(load(small));
        QVERIFY(app.open());
        app.drag(Qt::LeftButton, app.screen(0, 80), app.screen(300, 300), Qt::ControlModifier);
        QCOMPARE(app.pan(), QPointF(0, 0));
        QCOMPARE(app.model.selectedIds(), QStringList{"prompt"});
        app.drag(Qt::LeftButton, app.screen(640, 280), app.screen(950, 600), Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(app.model.selectedIds(), (QStringList{"prompt", "save"}));
        app.drag(Qt::LeftButton, app.screen(0, 80), app.screen(300, 300), Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(app.model.selectedIds(), QStringList{"save"});
    }

    void clickingANodeSelectsItAndShiftClickAdds() {
        App app(load(small));
        QVERIFY(app.open());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(100, 110));
        QCOMPARE(app.model.selectedIds(), QStringList{"prompt"});
        QTest::mouseClick(app.window, Qt::LeftButton, Qt::ShiftModifier, app.screen(400, 70));
        QCOMPARE(app.model.selectedIds(), (QStringList{"gen", "prompt"}));
        // A click on empty canvas clears it.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, {500, 640});
        QVERIFY(app.model.selectedIds().isEmpty());
    }

    void draggingANodeMovesItAtAnyZoom() {
        App app(load(small));
        QVERIFY(app.open());
        app.wheel({0, 0}, {0, 120 * 7}); // zoom 1.1^7 at the window's corner
        app.drag(Qt::LeftButton, app.screen(100, 110), app.screen(150, 130));
        const QPointF moved = app.model.position("prompt");
        QVERIFY2(qAbs(moved.x() - 70) < 1 && qAbs(moved.y() - 120) < 1,
                 qPrintable(QStringLiteral("%1, %2").arg(moved.x()).arg(moved.y())));
    }

    void draggingFromAnOutputToAnInputConnectsThem() {
        App app(load(small));
        QVERIFY(app.open());
        // Text's output sits at its right edge, on the first row: (20 + 240, 100 + 28 + 4 + 12).
        // Generate Image's prompt is its first input, after its five outputs: (340, 60 + 164).
        app.drag(Qt::LeftButton, app.screen(260, 144), app.screen(341, 224));
        QCOMPARE(app.model.links()->rowCount(), 1);
        QCOMPARE(app.model.document().graph.nodes.value("gen").inputs.value("prompt").from().toString(),
                 QString("prompt.text"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void anInvalidTargetSaysWhyAndTakesNothing() {
        App app(load(small));
        QVERIFY(app.open());
        auto *problem = item(app.window->contentItem(), "linkProblem");
        QVERIFY(problem);
        // Text into Save Image's image input (its first input, after one output: 660, 300 + 68).
        const QPoint from = app.screen(260, 144), to = app.screen(660, 368);
        QTest::mousePress(app.window, Qt::LeftButton, {}, from);
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(app.window, from + (to - from) * i / 8);
        QTRY_VERIFY(problem->isVisible());
        const QString why = app.canvas->property("hoverProblem").toString();
        // The two type names bold in the chip (docs/canvas-look.md 8.2).
        QVERIFY2(why.contains("<b>text</b>") && why.contains("<b>image</b>"), qPrintable(why));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, to);
        QVERIFY(!problem->isVisible());
        QCOMPARE(app.model.links()->rowCount(), 0);
    }

    void aLinkLetGoOnEmptyCanvasAsksForSearch() {
        App app(load(small));
        QVERIFY(app.open());
        QSignalSpy released(app.canvas, SIGNAL(linkReleased(QVariant, double, double)));
        app.drag(Qt::LeftButton, app.screen(260, 144), app.screen(300, 600));
        QCOMPARE(released.count(), 1);
        QCOMPARE(released.first().at(0).toMap().value("node").toString(), QString("prompt"));
        QCOMPARE(app.model.links()->rowCount(), 0);
    }

    void aDoubleClickOnEmptyCanvasAsksForSearch() {
        App app(load(small));
        QVERIFY(app.open());
        QSignalSpy search(app.canvas, SIGNAL(searchRequested(double, double)));
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, app.screen(300, 600));
        QCOMPARE(search.count(), 1);
        QCOMPARE(search.first().at(1).toDouble(), 600.0);
    }
};

QTEST_MAIN(TestCanvasWindow)
#include "test_canvas_window.moc"
