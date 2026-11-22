// The canvas gesture module (canvas/canvas_gesture.h) with a CanvasModel and no window: what a
// press, a drag and a release do by the rules of spec 12.3 and 12.4, and what a double click
// opens. The window tests (test_canvas_window and the others that use the mouse) keep what needs
// the real items: the cursor, the cut handle, the magnifier.
//
// The view is at zoom 1 with no pan, so a screen point and a world point are the same here.

#include <QJsonArray>
#include <QSignalSpy>
#include <QStyleHints>
#include <QtTest>
#include <QUndoStack>

#include "canvas/canvas_gesture.h"
#include "canvas/canvas_model.h"
#include "engine/document.h"
#include "engine/registry.h"
#include "nodes/builtin.h"

using namespace fugue;

namespace {

// Three Text nodes (a feeds gen), a Generate Image, a Save Image and an empty Frame, well apart.
const char *graph = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "a": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a red fox"}},
    "b": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "snow"}},
    "c": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "night"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": {"from": "a.text"}}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}}}
  }},
  "layout": {"nodes": {"a": {"pos": [0, 100]}, "b": {"pos": [0, 400]}, "c": {"pos": [0, 700]},
                       "gen": {"pos": [500, 80]}, "save": {"pos": [1000, 100]}},
             "frames": [{"id": "f1", "title": "Inputs", "rect": [0, 1000, 300, 200], "nodes": []}]}
})";

constexpr int none = 0;
constexpr int shift = Qt::ShiftModifier;
constexpr int ctrl = Qt::ControlModifier;
constexpr int alt = Qt::AltModifier;
constexpr qreal tolerance = 10; // the window's: 10 px at zoom 1

QPointF at(qreal x, qreal y) { return {x, y}; }

struct Fixture {
    Registry registry;
    CanvasModel canvas{&registry};
    CanvasGesture &g;
    QStringList kinds; // what `finished` said
    QSignalSpy cut, search, context, drop, released, viewer, finished, panStarted, panFinished, pressStarted;

    Fixture()
        : g(*canvas.gesture()), cut(&g, &CanvasGesture::cutHandlePressed), search(&g, &CanvasGesture::searchRequested),
          context(&g, &CanvasGesture::contextMenuRequested), drop(&g, &CanvasGesture::dropRequested),
          released(&g, &CanvasGesture::linkReleased), viewer(&g, &CanvasGesture::viewerRequested),
          finished(&g, &CanvasGesture::finished), panStarted(&g, &CanvasGesture::panStarted),
          panFinished(&g, &CanvasGesture::panFinished), pressStarted(&g, &CanvasGesture::pressStarted) {
        nodes::addBuiltinNodeTypes(registry);
        const Result<Document> loaded = Document::load(QByteArray(graph));
        if (!loaded.ok())
            qFatal("fixture does not load: %s", qPrintable(loaded.error().message));
        canvas.setDocument(loaded.value());
        g.setNavigation("legacy");
        g.setZoom(1);
        g.setTier(0);
        g.setFrameBand(canvas.headerHeight());
    }

    // The middle of a node's box, away from its ports and corners.
    QPointF body(const QString &node) const { return canvas.nodeRect(node).center(); }
    QPointF title(const QString &node) const {
        const QRectF r = canvas.nodeRect(node);
        return {r.center().x(), r.top() + canvas.headerHeight() / 2};
    }
    QPointF corner(const QString &node) const { return canvas.nodeRect(node).bottomRight() - QPointF(3, 3); }
    QPointF port(const QString &node, bool output, const QString &name) const {
        const QString side = output ? "outputs" : "inputs";
        for (int i = 0; i < canvas.nodes()->rowCount(); ++i) {
            const QModelIndex index = canvas.nodes()->index(i, 0);
            const QHash<int, QByteArray> roles = canvas.nodes()->roleNames();
            QVariantMap values;
            for (auto it = roles.begin(); it != roles.end(); ++it)
                values.insert(QString::fromUtf8(it.value()), canvas.nodes()->data(index, it.key()));
            if (values.value("nodeId") != node)
                continue;
            for (const QVariant &p : values.value(side).toList()) {
                if (p.toMap().value("name") != name)
                    continue;
                // The row's y is in the node; its port sits on the right edge (an output) or the left (an input).
                const QRectF box = canvas.nodeRect(node);
                return {output ? box.right() : box.left(), box.top() + p.toMap().value("y").toDouble()};
            }
        }
        qFatal("no port %s.%s", qPrintable(node), qPrintable(name));
    }
    QPointF empty() const { return {400, 900}; }

    void press(const QPointF &p, int modifiers = none, Qt::MouseButton button = Qt::LeftButton, int buttons = -1,
               qreal tol = tolerance, bool onCut = false) {
        if (g.admit(button, buttons < 0 ? int(button) : buttons))
            g.press(p, p, button, modifiers, tol, onCut);
    }
    void move(const QPointF &p, qreal tol = tolerance) { g.move(p, p, tol); }
    void release(const QPointF &p, int modifiers = none, Qt::MouseButton button = Qt::LeftButton, qreal tol = tolerance) {
        g.release(p, p, button, modifiers, tol);
    }
    void drag(const QPointF &from, const QPointF &to, int modifiers = none, int releaseModifiers = -1) {
        press(from, modifiers);
        move((from + to) / 2);
        move(to);
        release(to, releaseModifiers < 0 ? modifiers : releaseModifiers);
    }
    void click(const QPointF &p, int modifiers = none) {
        press(p, modifiers);
        release(p, modifiers);
    }
    bool linked(const QString &to) const {
        for (int i = 0; i < canvas.links()->rowCount(); ++i) {
            const QHash<int, QByteArray> roles = canvas.links()->roleNames();
            QVariantMap values;
            for (auto it = roles.begin(); it != roles.end(); ++it)
                values.insert(QString::fromUtf8(it.value()), canvas.links()->data(canvas.links()->index(i, 0), it.key()));
            if (values.value("toNode").toString() + "." + values.value("toPort").toString() == to)
                return true;
        }
        return false;
    }
    int steps() const { return canvas.undoStack()->count(); }
    QStringList selection() const {
        QStringList ids = canvas.selectedIds();
        ids.sort();
        return ids;
    }
};

} // namespace

class TestCanvasGesture : public QObject {
    Q_OBJECT

private slots:
    // The order of a press: what it lands on first decides the gesture.
    void aPressGoesToTheFirstThingUnderIt_data() {
        QTest::addColumn<QString>("target");
        QTest::addColumn<int>("modifiers");
        QTest::addColumn<QString>("navigation");
        QTest::addColumn<QString>("gesture");
        QTest::newRow("a port over its node") << "outputPort" << none << "legacy" << "link";
        QTest::newRow("an input port") << "inputPort" << none << "legacy" << "link";
        QTest::newRow("a corner over its node") << "corner" << none << "legacy" << "resize";
        QTest::newRow("a node's body") << "body" << none << "legacy" << "move";
        QTest::newRow("a node's body with Shift") << "body" << int(shift) << "legacy" << "move";
        QTest::newRow("a node's body with Alt waits for a drag") << "body" << int(alt) << "legacy" << "alt";
        QTest::newRow("a node's body with Ctrl+Alt moves") << "body" << int(ctrl | alt) << "legacy" << "move";
        QTest::newRow("a frame's title") << "frameTitle" << none << "legacy" << "move";
        QTest::newRow("empty canvas, legacy, pans") << "empty" << none << "legacy" << "pan";
        QTest::newRow("empty canvas, legacy, Ctrl is the marquee") << "empty" << int(ctrl) << "legacy" << "marquee";
        QTest::newRow("empty canvas, standard, is the marquee") << "empty" << none << "standard" << "marquee";
        QTest::newRow("a node, standard, still moves") << "body" << none << "standard" << "move";
    }
    void aPressGoesToTheFirstThingUnderIt() {
        QFETCH(QString, target);
        QFETCH(int, modifiers);
        QFETCH(QString, navigation);
        QFETCH(QString, gesture);
        Fixture f;
        f.g.setNavigation(navigation);
        const QHash<QString, QPointF> points{{"outputPort", f.port("a", true, "text")},
                                             {"inputPort", f.port("gen", false, "prompt")},
                                             {"corner", f.corner("b")},
                                             {"body", f.body("b")},
                                             {"frameTitle", at(50, 1010)},
                                             {"empty", f.empty()}};
        f.press(points.value(target), modifiers);
        QCOMPARE(f.g.current(), gesture);
        QCOMPARE(f.pressStarted.count(), 1); // the window forgets what it hovered
        f.release(points.value(target), modifiers);
        QCOMPARE(f.g.current(), QString());
        QCOMPARE(f.finished.last().first().toString(), gesture);
    }

    void aResizeCornerRecordsItsNodeAndCorner() {
        Fixture f;
        f.press(f.corner("b"));
        QCOMPARE(f.g.current(), QString("resize"));
        QCOMPARE(f.g.pressedNode(), QString("b"));
        QCOMPARE(f.g.resizeCorner(), QString("se"));
        const qreal width = f.canvas.nodeRect("b").width();
        f.move(f.corner("b") + QPointF(60, 40));
        QCOMPARE(f.canvas.nodeRect("b").width(), width + 60);
        f.release(f.corner("b") + QPointF(60, 40));
        QCOMPARE(f.g.resizeCorner(), QString());
    }

    void aPortWinsOverTheCornerBeneathIt() {
        // The corner is skipped where a port also is: here the node's own output port.
        Fixture f;
        f.press(f.port("a", true, "text"));
        QCOMPARE(f.g.current(), QString("link"));
        QCOMPARE(f.canvas.linkDrag().value("node").toString(), QString("a"));
    }

    void withoutAPortToleranceTheOverviewPressIsOnTheNode() {
        // In the overview (tier 2) ports cannot be hit: the window passes a tolerance of zero, and
        // corners are off too.
        Fixture f;
        f.g.setTier(2);
        f.press(f.port("a", true, "text"), none, Qt::LeftButton, -1, 0);
        QCOMPARE(f.g.current(), QString("move"));
        f.release(f.port("a", true, "text"), none, Qt::LeftButton, 0);
        f.press(f.corner("b"), none, Qt::LeftButton, -1, 0);
        QCOMPARE(f.g.current(), QString("move"));
    }

    void theMiddleButtonAndSpacePanWhateverIsUnderThem() {
        Fixture f;
        f.press(f.body("b"), none, Qt::MiddleButton);
        QCOMPARE(f.g.current(), QString("pan"));
        QCOMPARE(f.panStarted.count(), 1);
        QCOMPARE(f.pressStarted.count(), 0);
        f.release(f.body("b"), none, Qt::MiddleButton);
        QCOMPARE(f.panFinished.count(), 1);
        f.g.setSpaceHeld(true);
        f.press(f.port("a", true, "text"));
        QCOMPARE(f.g.current(), QString("pan"));
        QCOMPARE(f.canvas.linkDrag().value("node"), QVariant()); // no wire started
    }

    void theCutHandleComesBeforeEverythingButAPan() {
        Fixture f;
        f.press(f.body("b"), none, Qt::LeftButton, -1, tolerance, true);
        QCOMPARE(f.cut.count(), 1);
        QCOMPARE(f.g.current(), QString());
        QCOMPARE(f.pressStarted.count(), 0);
        QVERIFY(f.canvas.selectedIds().isEmpty()); // the node under it was not taken
        f.release(f.body("b"));
        f.g.setSpaceHeld(true);
        f.press(f.body("b"), none, Qt::LeftButton, -1, tolerance, true);
        QCOMPARE(f.cut.count(), 1); // a pan wins
        QCOMPARE(f.g.current(), QString("pan"));
    }

    void ctrlAltOnAPortDisconnectsItAndStartsNoGesture() {
        Fixture f;
        QVERIFY(f.linked("gen.prompt"));
        const int steps = f.steps();
        f.press(f.port("gen", false, "prompt"), ctrl | alt);
        QCOMPARE(f.g.current(), QString());
        QVERIFY(!f.linked("gen.prompt"));
        QCOMPARE(f.steps(), steps + 1);
        f.release(f.port("gen", false, "prompt"), ctrl | alt);
        QCOMPARE(f.steps(), steps + 1);
    }

    // A press that does not travel is a click; one that travels at least the drag distance moves.
    void aDragStartsAtTheTravelThreshold() {
        Fixture f;
        const QPointF from = f.body("b");
        const QPointF origin = f.canvas.position("b");
        const qreal threshold = QGuiApplication::styleHints()->startDragDistance();
        f.press(from);
        f.move(from + QPointF(threshold - 1, 0));
        QVERIFY(!f.g.travelled());
        f.move(from + QPointF(threshold, 0));
        QVERIFY(f.g.travelled());
        f.move(from + QPointF(50, 20));
        QCOMPARE(f.canvas.position("b"), origin + QPointF(50, 20));
        f.release(from + QPointF(50, 20));
        QCOMPARE(f.g.current(), QString());
        QCOMPARE(f.canvas.position("b"), origin + QPointF(50, 20));
    }

    void aClickMovesNothingAndAddsNoUndoStep() {
        Fixture f;
        const int steps = f.steps();
        const QPointF origin = f.canvas.position("b");
        f.click(f.body("b"));
        QCOMPARE(f.canvas.position("b"), origin);
        QCOMPARE(f.steps(), steps);
        QCOMPARE(f.selection(), QStringList{"b"});
    }

    // Selection by click.
    void aClickOnANodeSelectsOnlyIt() {
        Fixture f;
        f.canvas.select({"a", "b"});
        f.click(f.body("b")); // a selected node: the click narrows the selection to it
        QCOMPARE(f.selection(), QStringList{"b"});
        f.click(f.body("c")); // an unselected one replaces it
        QCOMPARE(f.selection(), QStringList{"c"});
    }

    void aDragOfASelectedNodeKeepsTheSelectionAndMovesItAll() {
        Fixture f;
        f.canvas.select({"a", "b"});
        const QPointF a = f.canvas.position("a"), b = f.canvas.position("b");
        f.drag(f.body("b"), f.body("b") + QPointF(30, 0));
        QCOMPARE(f.selection(), (QStringList{"a", "b"}));
        QCOMPARE(f.canvas.position("a"), a + QPointF(30, 0));
        QCOMPARE(f.canvas.position("b"), b + QPointF(30, 0));
    }

    void shiftAndCtrlAddToTheSelection_data() {
        QTest::addColumn<int>("modifiers");
        QTest::newRow("Shift") << int(shift);
        QTest::newRow("Ctrl") << int(ctrl);
    }
    void shiftAndCtrlAddToTheSelection() {
        QFETCH(int, modifiers);
        Fixture f;
        f.canvas.select({"a"});
        f.click(f.body("b"), modifiers);
        QCOMPARE(f.selection(), (QStringList{"a", "b"})); // added, and a click adds no more
        f.click(f.body("b"), modifiers);                  // a click on one already in it takes it out
        QCOMPARE(f.selection(), QStringList{"a"});
        // A drag adds and keeps it.
        f.drag(f.body("c"), f.body("c") + QPointF(20, 0), modifiers);
        QCOMPARE(f.selection(), (QStringList{"a", "c"}));
    }

    void altClickTakesTheNodeOutOfTheSelection() {
        Fixture f;
        f.canvas.select({"a", "b"});
        const int nodes = f.canvas.nodes()->rowCount();
        f.press(f.body("b"), alt);
        QCOMPARE(f.g.current(), QString("alt"));
        f.release(f.body("b"), alt);
        QCOMPARE(f.selection(), QStringList{"a"});
        QCOMPARE(f.canvas.nodes()->rowCount(), nodes);
    }

    void altDragClonesWhatItDrags() {
        Fixture f;
        const int nodes = f.canvas.nodes()->rowCount();
        const QPointF origin = f.canvas.position("b");
        f.press(f.body("b"), alt);
        f.move(f.body("b") + QPointF(1, 0)); // below the threshold: still not a clone
        QCOMPARE(f.g.current(), QString("alt"));
        QCOMPARE(f.canvas.nodes()->rowCount(), nodes);
        f.move(f.body("b") + QPointF(80, 60));
        QCOMPARE(f.g.current(), QString("move"));
        QCOMPARE(f.canvas.nodes()->rowCount(), nodes + 1);
        f.release(f.body("b") + QPointF(80, 60), alt);
        QCOMPARE(f.canvas.position("b"), origin); // the original stays
        QCOMPARE(f.canvas.nodes()->rowCount(), nodes + 1);
    }

    // Links.
    void aLinkLetGoOnAPortConnects() {
        Fixture f;
        QVERIFY(!f.linked("gen.negative_prompt"));
        f.drag(f.port("b", true, "text"), f.port("gen", false, "negative_prompt"));
        QVERIFY(f.linked("gen.negative_prompt"));
        QVERIFY(f.canvas.linkDrag().isEmpty());
        QCOMPARE(f.drop.count(), 0);
        QCOMPARE(f.released.count(), 0);
    }

    void aLinkLetGoOnANodeAsksForTheDropChoice() {
        Fixture f;
        const QPointF to = f.body("gen");
        f.drag(f.port("b", true, "text"), to);
        QCOMPARE(f.drop.count(), 1);
        QCOMPARE(f.drop.first().at(0).toString(), QString("gen"));
        QCOMPARE(f.drop.first().at(1).toPointF(), to);
        QVERIFY(!f.canvas.linkDrag().isEmpty()); // the window's list ends the drag
        QCOMPARE(f.released.count(), 0);
        f.canvas.endLinkDrag();
    }

    void aLinkLetGoOnNothingAsksForTheSearch() {
        Fixture f;
        f.drag(f.port("b", true, "text"), f.empty());
        QCOMPARE(f.released.count(), 1);
        const QVariantMap from = f.released.first().at(0).toMap();
        QCOMPARE(from.value("node").toString(), QString("b"));
        QCOMPARE(from.value("port").toString(), QString("text"));
        QCOMPARE(from.value("output").toBool(), true);
        QCOMPARE(f.released.first().at(1).toPointF(), f.empty());
        QVERIFY(f.canvas.linkDrag().isEmpty());
    }

    void aLinkThatNeverTravelledAsksForNothing() {
        Fixture f;
        const QPointF p = f.port("b", true, "text");
        f.press(p);
        f.release(p + QPointF(0, 0), none, Qt::LeftButton, 0); // let go, no port reachable: a click
        QCOMPARE(f.released.count(), 0);
        QCOMPARE(f.drop.count(), 0);
        QVERIFY(f.canvas.linkDrag().isEmpty());
    }

    void aLinkWhoseDragEndedByItselfLetsGoOfNothing() {
        Fixture f;
        f.press(f.port("b", true, "text"));
        f.canvas.endLinkDrag(); // an undo or a key ended it
        f.move(f.empty());
        f.release(f.empty());
        QCOMPARE(f.released.count(), 0);
        QCOMPARE(f.g.current(), QString());
    }

    void shiftDragOfAnOutputMovesItsLinksAndLetGoOnNothingKeepsThem() {
        Fixture f;
        f.press(f.port("a", true, "text"), shift);
        QCOMPARE(f.g.current(), QString("link"));
        QVERIFY(f.canvas.linkDrag().contains("targets"));
        f.move(f.empty());
        f.release(f.empty(), shift);
        QVERIFY(f.linked("gen.prompt")); // still wired to a
        QCOMPARE(f.released.count(), 0);
        QCOMPARE(f.drop.count(), 0);
        QVERIFY(f.canvas.linkDrag().isEmpty());
    }

    void shiftDragOfAnOutputMovesItsLinksToAnotherPort() {
        Fixture f;
        f.press(f.port("a", true, "text"), shift);
        f.move(f.port("b", true, "text") + QPointF(0, 0));
        f.release(f.port("b", true, "text"), shift);
        QVERIFY(f.linked("gen.prompt")); // now from b
        const Document doc = f.canvas.document();
        QVERIFY(doc.save().contains("b.text"));
    }

    // The marquee.
    void theMarqueeSelectsByModifier_data() {
        QTest::addColumn<int>("modifiers");
        QTest::addColumn<QStringList>("before");
        QTest::addColumn<QStringList>("after");
        QTest::newRow("replace") << none << QStringList{"a", "c"} << QStringList{"b"};
        QTest::newRow("Shift adds") << int(shift) << QStringList{"a"} << QStringList{"a", "b"};
        QTest::newRow("Alt subtracts") << int(alt) << QStringList{"a", "b"} << QStringList{"a"};
    }
    void theMarqueeSelectsByModifier() {
        QFETCH(int, modifiers);
        QFETCH(QStringList, before);
        QFETCH(QStringList, after);
        Fixture f;
        f.g.setNavigation("standard");
        f.canvas.select(before);
        // A box around b only: from empty canvas to the right of b's column, over its box.
        const QRectF b = f.canvas.nodeRect("b");
        const QPointF from(b.left() - 20, b.top() - 20), to(b.right() + 20, b.bottom() + 20);
        f.press(from);
        QCOMPARE(f.g.current(), QString("marquee"));
        f.move(to);
        QVERIFY(f.g.travelled());
        f.release(to, modifiers); // the modifiers at the release choose the mode
        QCOMPARE(f.selection(), after);
    }

    void theMarqueeMapsItsCornersThroughThePan() {
        Fixture f;
        f.g.setNavigation("standard");
        f.g.setZoom(0.5);
        f.g.setPanX(100);
        f.g.setPanY(-50);
        const QRectF b = f.canvas.nodeRect("b");
        auto screen = [](const QPointF &w) { return QPointF(w.x() * 0.5 + 100, w.y() * 0.5 - 50); };
        const QPointF from = screen(b.topLeft() - QPointF(20, 20)), to = screen(b.bottomRight() + QPointF(20, 20));
        f.g.admit(Qt::LeftButton, Qt::LeftButton);
        f.g.press(from, QPointF(), Qt::LeftButton, none, tolerance, false);
        f.g.move(to, QPointF(), tolerance);
        f.g.release(to, QPointF(), Qt::LeftButton, none, tolerance);
        QCOMPARE(f.selection(), QStringList{"b"});
    }

    void aMarqueeThatDidNotTravelClearsTheSelection() {
        Fixture f;
        f.g.setNavigation("standard");
        f.canvas.select({"a"});
        f.click(f.empty());
        QVERIFY(f.canvas.selectedIds().isEmpty());
    }

    void aMarqueeThatDidNotTravelWithShiftKeepsTheSelection() {
        Fixture f;
        f.g.setNavigation("standard");
        f.canvas.select({"a"});
        f.press(f.empty());
        f.release(f.empty(), shift);
        QCOMPARE(f.selection(), QStringList{"a"});
    }

    // The pan.
    void aLegacyPanClickClearsTheSelectionAndADragDoesNot() {
        Fixture f;
        f.canvas.select({"a"});
        f.drag(f.empty(), f.empty() + QPointF(80, 0));
        QCOMPARE(f.selection(), QStringList{"a"});
        QCOMPARE(f.panStarted.count(), 1);
        QCOMPARE(f.panFinished.count(), 1);
        f.click(f.empty());
        QVERIFY(f.canvas.selectedIds().isEmpty());
        QCOMPARE(f.panFinished.count(), 2);
    }

    void aPanClickKeepsTheSelectionWithSpaceHeldOrInStandardNavigation() {
        Fixture f;
        f.canvas.select({"a"});
        f.g.setSpaceHeld(true);
        f.click(f.empty());
        QCOMPARE(f.selection(), QStringList{"a"});
        f.g.setSpaceHeld(false);
        f.press(f.empty(), none, Qt::MiddleButton); // a middle click is a pan click too, in legacy navigation
        f.release(f.empty(), none, Qt::MiddleButton);
        QVERIFY(f.canvas.selectedIds().isEmpty());
        f.g.setNavigation("standard");
        f.canvas.select({"a"});
        f.press(f.empty(), none, Qt::MiddleButton);
        f.release(f.empty(), none, Qt::MiddleButton);
        QCOMPARE(f.selection(), QStringList{"a"});
    }

    // Buttons.
    void aSecondButtonDuringAGestureIsLeftOut() {
        Fixture f;
        const QPointF origin = f.canvas.position("b");
        f.press(f.body("b"));
        QCOMPARE(f.g.current(), QString("move"));
        f.press(f.empty(), none, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
        QCOMPARE(f.g.current(), QString("move")); // the gesture goes on
        f.move(f.body("b") + QPointF(40, 0));
        QCOMPARE(f.canvas.position("b"), origin + QPointF(40, 0));
        f.release(f.empty(), none, Qt::RightButton); // its release does nothing: no menu
        QCOMPARE(f.context.count(), 0);
        QCOMPARE(f.g.current(), QString("move"));
        f.release(f.body("b") + QPointF(40, 0));
        QCOMPARE(f.g.current(), QString());
        QCOMPARE(f.context.count(), 0);
    }

    void aRightClickAsksForTheContextMenuAndARightDragDoesNot() {
        Fixture f;
        f.press(f.empty(), none, Qt::RightButton);
        f.release(f.empty(), none, Qt::RightButton);
        QCOMPARE(f.context.count(), 1);
        QCOMPARE(f.context.first().at(0).toPointF(), f.empty());
        f.press(f.empty(), none, Qt::RightButton);
        f.release(f.empty() + QPointF(60, 0), none, Qt::RightButton);
        QCOMPARE(f.context.count(), 1);
        QCOMPARE(f.g.current(), QString()); // a right press begins no gesture
    }

    void aPressWhoseReleaseNeverCameFinishesTheOldGestureFirst() {
        Fixture f;
        f.press(f.body("b"), none, Qt::MiddleButton); // a pan whose release was lost
        QCOMPARE(f.g.current(), QString("pan"));
        f.press(f.body("c")); // the left button is the only one down now
        QCOMPARE(f.panFinished.count(), 1);
        QCOMPARE(f.g.current(), QString("move"));
        QCOMPARE(f.g.pressedNode(), QString("c"));
        f.release(f.body("c"));
    }

    void aFinishedMoveIsItsOwnUndoStep() {
        Fixture f;
        const int steps = f.steps();
        f.drag(f.body("b"), f.body("b") + QPointF(30, 0));
        f.drag(f.body("c"), f.body("c") + QPointF(30, 0));
        QCOMPARE(f.steps(), steps + 2);
    }

    void cancelFinishesTheGestureWhereItWas() {
        Fixture f;
        const QPointF origin = f.canvas.position("b");
        const int steps = f.steps();
        f.press(f.body("b"));
        f.move(f.body("b") + QPointF(25, 0));
        f.press(f.empty(), none, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
        f.g.cancel();
        QCOMPARE(f.g.current(), QString());
        QCOMPARE(f.canvas.position("b"), origin + QPointF(25, 0)); // kept, as a release would
        QCOMPARE(f.steps(), steps + 1);
        QCOMPARE(f.finished.last().first().toString(), QString("move"));
        // The right button's release after a cancel is no longer ignored, but a lone one is a click.
        f.release(f.empty(), none, Qt::RightButton);
        QCOMPARE(f.g.current(), QString());
    }

    void cancelOfALinkLetsGoOnTheSpot() {
        Fixture f;
        f.press(f.port("b", true, "text"));
        f.move(f.empty());
        f.g.cancel();
        QVERIFY(f.canvas.linkDrag().isEmpty());
        QCOMPARE(f.g.current(), QString());
    }

    // The double click.
    void aDoubleClickOnATitleRenamesTheNode() {
        Fixture f;
        QSignalSpy rename(&f.canvas, &CanvasModel::nodeRenameRequested);
        f.g.doubleClick(f.title("b"), f.title("b"), Qt::LeftButton, tolerance);
        QCOMPARE(rename.count(), 1);
        QCOMPARE(rename.first().first().toString(), QString("b"));
        QCOMPARE(f.search.count(), 0);
    }

    void aDoubleClickOnAPlainBodyDoesNothing() {
        Fixture f;
        QSignalSpy rename(&f.canvas, &CanvasModel::nodeRenameRequested);
        f.g.doubleClick(f.body("b"), f.body("b"), Qt::LeftButton, tolerance);
        QCOMPARE(rename.count(), 0);
        QCOMPARE(f.search.count(), 0);
        QCOMPARE(f.viewer.count(), 0);
    }

    void aDoubleClickOnEmptyCanvasOpensTheSearchAtThePoint() {
        Fixture f;
        f.g.doubleClick(f.empty(), f.empty(), Qt::LeftButton, tolerance);
        QCOMPARE(f.search.count(), 1);
        QCOMPARE(f.search.first().first().toPointF(), f.empty());
        f.g.doubleClick(f.empty(), f.empty(), Qt::RightButton, tolerance); // only the left button
        QCOMPARE(f.search.count(), 1);
    }

    void aDoubleClickOnAFrameTitleRenamesTheFrame() {
        Fixture f;
        QSignalSpy rename(&f.canvas, &CanvasModel::frameRenameRequested);
        f.g.doubleClick(at(50, 1010), at(50, 1010), Qt::LeftButton, tolerance);
        QCOMPARE(rename.count(), 1);
        QCOMPARE(rename.first().first().toString(), QString("f1"));
        QCOMPARE(f.search.count(), 0);
        f.g.doubleClick(at(50, 1150), at(50, 1150), Qt::LeftButton, tolerance); // the frame's body: the canvas
        QCOMPARE(rename.count(), 1);
        QCOMPARE(f.search.count(), 1);
    }

    void aDoubleClickOnACornerReturnsTheNodeToAutoSize() {
        Fixture f;
        const qreal width = f.canvas.nodeRect("b").width();
        f.canvas.beginResize("b", "se");
        f.canvas.resizeBy(70, 0);
        f.canvas.endResize();
        QCOMPARE(f.canvas.nodeRect("b").width(), width + 70);
        f.g.doubleClick(f.corner("b"), f.corner("b"), Qt::LeftButton, tolerance);
        QCOMPARE(f.canvas.nodeRect("b").width(), width);
    }

    void aDoubleClickOnAGroupsBodyEntersItAndItsTitleRenamesIt() {
        Fixture f;
        f.canvas.select({"gen"});
        const QString group = f.canvas.groups()->makeGroup();
        QVERIFY(!group.isEmpty());
        QSignalSpy rename(&f.canvas, &CanvasModel::nodeRenameRequested);
        f.g.doubleClick(f.title(group), f.title(group), Qt::LeftButton, tolerance);
        QCOMPARE(rename.count(), 1);
        QVERIFY(!f.canvas.inGroup());
        f.g.doubleClick(f.body(group), f.body(group), Qt::LeftButton, tolerance);
        QVERIFY(f.canvas.inGroup());
        QCOMPARE(rename.count(), 1);
    }

    void aDoubleClickOnAGroupPortAsksToRenameIt() {
        Fixture f;
        f.canvas.select({"gen"});
        const QString group = f.canvas.groups()->makeGroup();
        QVERIFY(!group.isEmpty());
        QSignalSpy rename(&f.canvas, &CanvasModel::portRenameRequested);
        // The group card's first input port.
        QVariantList ports;
        for (int i = 0; i < f.canvas.nodes()->rowCount(); ++i) {
            const QModelIndex index = f.canvas.nodes()->index(i, 0);
            const QHash<int, QByteArray> roles = f.canvas.nodes()->roleNames();
            if (f.canvas.nodes()->data(index, roles.key("nodeId")).toString() == group)
                ports = f.canvas.nodes()->data(index, roles.key("inputs")).toList();
        }
        QVERIFY(!ports.isEmpty());
        const QPointF p = f.port(group, false, ports.first().toMap().value("name").toString());
        f.g.doubleClick(p, p, Qt::LeftButton, tolerance);
        QCOMPARE(rename.count(), 1);
        QCOMPARE(f.search.count(), 0);
    }

    void aDoubleClickOnAPlainNodesPortDoesNothing() {
        Fixture f;
        QSignalSpy rename(&f.canvas, &CanvasModel::portRenameRequested);
        QSignalSpy nodeRename(&f.canvas, &CanvasModel::nodeRenameRequested);
        const QPointF p = f.port("gen", false, "prompt");
        f.g.doubleClick(p, p, Qt::LeftButton, tolerance);
        QCOMPARE(rename.count(), 0);
        QCOMPARE(nodeRename.count(), 0);
        QCOMPARE(f.search.count(), 0);
    }

    void aDoubleClickOnATitleInTheOverviewStillRenames() {
        // The overview: no ports (tolerance zero) and no corners; the title is still the node's.
        Fixture f;
        f.g.setTier(2);
        QSignalSpy rename(&f.canvas, &CanvasModel::nodeRenameRequested);
        f.g.doubleClick(f.title("a"), f.title("a"), Qt::LeftButton, 0);
        QCOMPARE(rename.count(), 1);
    }
};

QTEST_MAIN(TestCanvasGesture)
#include "test_canvas_gesture.moc"
