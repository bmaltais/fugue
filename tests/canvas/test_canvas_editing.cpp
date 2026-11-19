// Editing in the real window (Main.qml, offscreen), driven with keys and clicks: selection,
// copy and paste, clone, delete, bypass, mute, collapse, disconnecting and moving links, the
// zoom and fit keys, and one undo stack under Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z (spec 12.3,
// 12.4, 12.11).

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QMimeData>
#include <QQuickStyle>
#include <QUndoStack>

#include "canvas_app.h"

using namespace fugue;
using canvas_test::App;
using canvas_test::load;

namespace {

// prompt (Text) -> gen (Generate Image) -> save (Save Image), and a second Text; the view at
// zoom 1 with the world's origin at the window's top left. At text size 12 a node is 240 wide,
// its title 28 high, then 4 of padding and 24 per row, outputs first (docs/canvas-look.md 4):
// Text's output is at (260, 144), Generate Image's prompt input (its sixth row, after its five
// outputs) at (340, 224), Save Image's image input (after its one output) at (660, 368), and
// other's output at (260, 444).
const char *linked = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": {"from": "prompt.text"}}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}}},
    "other": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "snow"}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [20, 100]}, "gen": {"pos": [340, 60]}, "save": {"pos": [660, 300]},
                       "other": {"pos": [20, 400]}},
             "view": {"center": [480, 340], "zoom": 1}}
})";

// Two Load Image nodes feeding a grid of `count` Blend nodes, each linked from its left and
// upper neighbours (or a Load Image at the edges): 2 * count wires.
Document grid(int count, int columns = 20) {
    QJsonObject nodes, positions;
    for (const char *source : {"src-a", "src-b"}) {
        nodes.insert(source, QJsonObject{{"type", "fugue/load-image"}, {"version", "1.0.0"}});
        positions.insert(source, QJsonObject{{"pos", QJsonArray{-300, source[4] == 'a' ? 0 : 200}}});
    }
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
    const Result<Document> made = Document::fromJson(
        QJsonObject{{"fugue", 1}, {"graph", QJsonObject{{"nodes", nodes}}}, {"layout", QJsonObject{{"nodes", positions}}}});
    if (!made.ok())
        qFatal("grid does not load: %s", qPrintable(made.error().message));
    return made.value();
}

QString from(const App &app, const QString &node, const QString &input) {
    const InputValue value = app.model.document().graph.nodes.value(node).inputs.value(input);
    return value.kind() == InputValue::Kind::Link ? value.from().toString() : QString();
}

} // namespace

class TestCanvasEditing : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void ctrlZUndoesAndCtrlYAndCtrlShiftZRedo() {
        App app(load(linked));
        QVERIFY(app.open());
        // A drag of Text, then a link from other into Generate Image's prompt.
        app.drag(Qt::LeftButton, app.screen(100, 110), app.screen(100, 160));
        QCOMPARE(app.model.position("prompt"), QPointF(20, 150));
        app.drag(Qt::LeftButton, app.screen(260, 444), app.screen(341, 224));
        QCOMPARE(from(app, "gen", "prompt"), QString("other.text"));
        QCOMPARE(app.model.undoStack()->count(), 2);

        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(from(app, "gen", "prompt"), QString("prompt.text"));
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(app.model.position("prompt"), QPointF(20, 100));
        QVERIFY(!app.workflows.unsaved()); // back to what was opened
        app.key(Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(app.model.position("prompt"), QPointF(20, 150));
        app.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(from(app, "gen", "prompt"), QString("other.text"));
        QVERIFY(app.workflows.unsaved());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void copyAndPasteLandAtThePointer() {
        App app(load(linked));
        QVERIFY(app.open());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(100, 110)); // prompt
        QTest::mouseClick(app.window, Qt::LeftButton, Qt::ShiftModifier, app.screen(400, 70)); // gen
        app.key(Qt::Key_C, Qt::ControlModifier);
        const QMimeData *copied = QGuiApplication::clipboard()->mimeData();
        QVERIFY(copied && copied->hasFormat(canvasClipboardType));
        QVERIFY(Document::load(copied->text().toUtf8()).ok());

        QTest::mouseMove(app.window, app.screen(500, 500));
        app.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(app.model.document().graph.nodes.size(), 6);
        const QStringList pasted = app.model.selectedIds();
        QCOMPARE(pasted.size(), 2);
        // The copy's top left (gen's top, prompt's left) is at the pointer.
        QRectF bounds;
        for (const QString &id : pasted)
            bounds = bounds.united(QRectF(app.model.position(id), QSizeF(1, 1)));
        QCOMPARE(bounds.topLeft(), QPointF(500, 500));
        // The link between them came along; the one into gen from outside did not exist.
        QCOMPARE(app.model.links()->rowCount(), 3);

        // Ctrl+Shift+V keeps the links from outside: Save Image's image from gen.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(700, 310)); // save
        app.key(Qt::Key_C, Qt::ControlModifier);
        app.key(Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        const QString save = app.model.selectedIds().value(0);
        QVERIFY(save != "save");
        QCOMPARE(from(app, save, "image"), QString("gen.image"));
        QCOMPARE(app.model.undoStack()->count(), 2);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void ctrlDClonesDeleteDeletesCtrlASelectsAll() {
        App app(load(linked));
        QVERIFY(app.open());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(100, 410)); // other
        app.key(Qt::Key_D, Qt::ControlModifier);
        const QString clone = app.model.selectedIds().value(0);
        QVERIFY(!clone.isEmpty() && clone != "other");
        QCOMPARE(app.model.position(clone), QPointF(40, 420));
        app.key(Qt::Key_Delete);
        QVERIFY(!app.model.document().graph.nodes.contains(clone));
        app.key(Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(app.model.selectedIds().size(), 4);
        app.key(Qt::Key_Backspace);
        QCOMPARE(app.model.nodes()->rowCount(), 0);
        QCOMPARE(app.model.links()->rowCount(), 0);
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(app.model.nodes()->rowCount(), 4);
        QCOMPARE(app.model.links()->rowCount(), 2);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void bypassMuteAndCollapseKeys() {
        App app(load(linked));
        QVERIFY(app.open());
        auto mode = [&](const QString &id) { return nodeModeName(app.model.document().graph.nodes.value(id).mode); };
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(400, 70)); // gen
        app.key(Qt::Key_B, Qt::ControlModifier);
        QCOMPARE(mode("gen"), QString("bypass"));
        app.key(Qt::Key_M, Qt::ControlModifier);
        QCOMPARE(mode("gen"), QString("mute"));
        app.key(Qt::Key_M, Qt::ControlModifier);
        QCOMPARE(mode("gen"), QString("normal"));
        app.key(Qt::Key_C, Qt::AltModifier);
        QVERIFY(app.model.document().layout.value("nodes").toObject().value("gen").toObject().value("collapsed").toBool());
        // A collapsed node is as tall as its title, so a click below it is on empty canvas.
        QCOMPARE(app.model.nodeAt(400, 120), QString());
        app.key(Qt::Key_Z, Qt::ControlModifier);
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(mode("gen"), QString("mute"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void altDragClonesAndAltClickDeselects() {
        App app(load(linked));
        QVERIFY(app.open());
        app.drag(Qt::LeftButton, app.screen(100, 410), app.screen(100, 510), Qt::AltModifier);
        QCOMPARE(app.model.position("other"), QPointF(20, 400));
        const QString clone = app.model.selectedIds().value(0);
        QVERIFY(clone != "other");
        QCOMPARE(app.model.position(clone), QPointF(20, 500));
        QCOMPARE(app.model.undoStack()->count(), 1);
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(app.model.nodes()->rowCount(), 4);

        app.key(Qt::Key_A, Qt::ControlModifier);
        QTest::mouseClick(app.window, Qt::LeftButton, Qt::AltModifier, app.screen(100, 410));
        QCOMPARE(app.model.selectedIds(), (QStringList{"gen", "prompt", "save"}));
        QCOMPARE(app.model.nodes()->rowCount(), 4);
    }

    void shiftOrCtrlClickTakesASelectedNodeOut() {
        App app(load(linked));
        QVERIFY(app.open());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(100, 110));
        QTest::mouseClick(app.window, Qt::LeftButton, Qt::ControlModifier, app.screen(400, 70));
        QCOMPARE(app.model.selectedIds(), (QStringList{"gen", "prompt"}));
        QTest::mouseClick(app.window, Qt::LeftButton, Qt::ShiftModifier, app.screen(100, 110));
        QCOMPARE(app.model.selectedIds(), QStringList{"gen"});
        // Shift+dragging a selected node still moves the selection.
        QTest::mouseClick(app.window, Qt::LeftButton, Qt::ShiftModifier, app.screen(100, 110));
        app.drag(Qt::LeftButton, app.screen(100, 110), app.screen(110, 110), Qt::ShiftModifier);
        QCOMPARE(app.model.position("gen"), QPointF(350, 60));
        QCOMPARE(app.model.selectedIds(), (QStringList{"gen", "prompt"}));
    }

    void ctrlAltClickOnAPortDisconnectsIt() {
        App app(load(linked));
        QVERIFY(app.open());
        QTest::mouseClick(app.window, Qt::LeftButton, Qt::ControlModifier | Qt::AltModifier, app.screen(660, 368));
        QCOMPARE(from(app, "save", "image"), QString());
        QCOMPARE(app.model.links()->rowCount(), 1);
        QTest::mouseClick(app.window, Qt::LeftButton, Qt::ControlModifier | Qt::AltModifier, app.screen(260, 144));
        QCOMPARE(app.model.links()->rowCount(), 0);
        app.key(Qt::Key_Z, Qt::ControlModifier);
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(app.model.links()->rowCount(), 2);
    }

    void shiftDragFromAnOutputMovesItsLinks() {
        App app(load(linked));
        QVERIFY(app.open());
        // From Text's output to other's: Generate Image now reads other.
        app.drag(Qt::LeftButton, app.screen(260, 144), app.screen(260, 444), Qt::ShiftModifier);
        QCOMPARE(from(app, "gen", "prompt"), QString("other.text"));
        QCOMPARE(app.model.links()->rowCount(), 2);
        // Let go on empty canvas: the links stay where they are, and search does not open.
        app.drag(Qt::LeftButton, app.screen(260, 444), app.screen(500, 600), Qt::ShiftModifier);
        QCOMPARE(from(app, "gen", "prompt"), QString("other.text"));
        QVERIFY(!app.model.search()->active());
        QCOMPARE(app.model.undoStack()->count(), 1);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void editsOnThreeHundredNodesStayQuick() {
        App app(grid(300));
        QVERIFY(app.open());
        QElapsedTimer timer;
        timer.start();
        app.key(Qt::Key_A, Qt::ControlModifier);
        app.key(Qt::Key_Delete);
        QCOMPARE(app.model.nodes()->rowCount(), 0);
        const qint64 deleted = timer.restart();
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(app.model.links()->rowCount(), 600);
        const qint64 undone = timer.restart();
        // Undoing a move in a full graph touches only what moved and its wires.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(100, 10));
        QCOMPARE(app.model.selectedIds(), QStringList{"n0"});
        app.drag(Qt::LeftButton, app.screen(100, 10), app.screen(100, 40));
        QCOMPARE(app.model.position("n0"), QPointF(0, 30));
        timer.restart();
        app.key(Qt::Key_Z, Qt::ControlModifier);
        const qint64 moved = timer.elapsed();
        qInfo("302 nodes: delete all %lld ms, undo it %lld ms, undo a move %lld ms", deleted, undone, moved);
        QCOMPARE(app.model.position("n0"), QPointF(0, 0));
        // Generous bounds for a slow CI machine; a whole-graph rebuild per edit would blow them.
        QVERIFY2(deleted < 1000 && undone < 1000, qPrintable(QStringLiteral("%1 %2").arg(deleted).arg(undone)));
        QVERIFY2(moved < 200, qPrintable(QString::number(moved)));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void eachTabHasItsOwnUndoStack() {
        App app(load(linked));
        QVERIFY(app.open());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(100, 410)); // other
        app.key(Qt::Key_Delete);
        QCOMPARE(app.model.nodes()->rowCount(), 3);
        app.key(Qt::Key_N, Qt::ControlModifier); // a new tab
        QCOMPARE(app.workflows.count(), 2);
        QVERIFY(!app.model.canUndo());
        app.model.addNode("fugue/number", 0, 0);
        app.workflows.setCurrent(0);
        QCOMPARE(app.model.nodes()->rowCount(), 3);
        app.key(Qt::Key_Z, Qt::ControlModifier); // the delete in this tab, not the add in the other
        QCOMPARE(app.model.nodes()->rowCount(), 4);
        QVERIFY(!app.workflows.unsaved(0));
        app.workflows.setCurrent(1);
        QCOMPARE(app.model.nodes()->rowCount(), 1);
        app.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(app.model.nodes()->rowCount(), 0);
        app.key(Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(app.model.nodes()->rowCount(), 1);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void zoomAndFitKeys() {
        App app(load(linked));
        QVERIFY(app.open());
        const qreal width = app.canvas->width(), height = app.canvas->height();
        const QPoint middle = app.canvas->mapToScene(QPointF(width / 2, height / 2)).toPoint();
        const QPointF centre((width / 2 - app.pan().x()) / app.zoom(), (height / 2 - app.pan().y()) / app.zoom());
        app.key(Qt::Key_Equal, Qt::ControlModifier);
        QVERIFY(qAbs(app.zoom() - 1.1) < 1e-9);
        // At the canvas's center, which stays where it was.
        QCOMPARE(app.screen(centre.x(), centre.y()), middle);
        app.key(Qt::Key_Minus, Qt::AltModifier);
        QVERIFY(qAbs(app.zoom() - 1.0) < 1e-9);
        // Fit: every node's box fills three quarters of the canvas, centered.
        QRectF bounds;
        for (int i = 0; i < app.model.nodes()->rowCount(); ++i) {
            auto role = [&](int r) { return app.model.nodes()->data(app.model.nodes()->index(i, 0), r).toDouble(); };
            bounds = bounds.united(QRectF(role(NodeList::X), role(NodeList::Y), role(NodeList::Width), role(NodeList::Height)));
        }
        QCOMPARE(bounds.left(), 20.0);
        QCOMPARE(bounds.right(), 900.0);
        app.key(Qt::Key_Period);
        QVERIFY(qAbs(app.zoom() - std::min(width / bounds.width(), height / bounds.height()) * 0.75) < 1e-9);
        QCOMPARE(app.screen(bounds.center().x(), bounds.center().y()), middle);
        // With a selection, Home fits just that: other's center comes to the canvas's center.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(100, 410));
        QCOMPARE(app.model.selectedIds(), QStringList{"other"});
        app.key(Qt::Key_Home);
        qreal otherHeight = 0;
        for (int i = 0; i < app.model.nodes()->rowCount(); ++i)
            if (app.model.nodes()->data(app.model.nodes()->index(i, 0), NodeList::NodeId) == "other")
                otherHeight = app.model.nodes()->data(app.model.nodes()->index(i, 0), NodeList::Height).toDouble();
        QCOMPARE(app.screen(140, 400 + otherHeight / 2), middle);
        // The view is not an edit.
        QVERIFY(!app.model.canUndo());
        QVERIFY(!app.workflows.unsaved());
    }
};

QTEST_MAIN(TestCanvasEditing)
#include "test_canvas_editing.moc"
