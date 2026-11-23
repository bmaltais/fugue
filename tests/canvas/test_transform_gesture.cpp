// The Move and Free Transform gestures (#236, epic #216): canvas/transform_math.h (pure numbers) and canvas/transform_gesture.h
// (the QML object), apart from any window: a handle scales from the opposite side, Shift keeps the proportions, Alt works from the
// center, outside a corner turns (Shift by 15 degrees), the size never goes under one pixel, a turned box scales along its own
// edges, and a drag is computed from where it started so going out and back returns to where the pointer is.

#include <QTemporaryDir>
#include <QtTest>

#include "canvas/guides_model.h"
#include "canvas/transform_gesture.h"
#include "engine/canvas_store.h"

using namespace fugue;
using namespace fugue::xform;
using canvas::LayerTransform;

namespace {

LayerTransform box(double cx, double cy, double w, double h, double angle = 0) {
    LayerTransform t;
    t.cx = cx;
    t.cy = cy;
    t.width = w;
    t.height = h;
    t.angle = angle;
    return t;
}

crop::Point at(double x, double y) { return {x, y}; }

} // namespace

class TestTransformGesture : public QObject {
    Q_OBJECT

private slots:
    void aCornerScalesFromTheOppositeCorner() {
        const LayerTransform start = box(100, 100, 80, 40); // 60..140 x 80..120
        const LayerTransform t = dragged(start, Handle::BottomRight, at(140, 120), at(160, 140), {});
        QCOMPARE(t.width, 100.0);
        QCOMPARE(t.height, 60.0);
        QCOMPARE(t.cx - t.width / 2, 60.0); // the top left corner held
        QCOMPARE(t.cy - t.height / 2, 80.0);
        const LayerTransform tl = dragged(start, Handle::TopLeft, at(60, 80), at(50, 70), {});
        QCOMPARE(tl.cx + tl.width / 2, 140.0); // the bottom right held
        QCOMPARE(tl.cy + tl.height / 2, 120.0);
        QCOMPARE(tl.width, 90.0);
    }

    void anEdgeScalesOneSideOnly() {
        const LayerTransform start = box(100, 100, 80, 40);
        const LayerTransform t = dragged(start, Handle::Right, at(140, 100), at(180, 130), {});
        QCOMPARE(t.width, 120.0);
        QCOMPARE(t.height, 40.0); // the y of the drag is not used
        QCOMPARE(t.cx, 120.0);
        QCOMPARE(t.cy, 100.0);
        const LayerTransform top = dragged(start, Handle::Top, at(100, 80), at(100, 60), {});
        QCOMPARE(top.height, 60.0);
        QCOMPARE(top.cy + top.height / 2, 120.0); // the bottom held
    }

    void shiftKeepsTheProportionsAndAltWorksFromTheCenter() {
        const LayerTransform start = box(100, 100, 80, 40);
        const LayerTransform t = dragged(start, Handle::BottomRight, at(140, 120), at(180, 125), {true, false}); // the wider pull wins
        QCOMPARE(t.width / t.height, 2.0);
        QCOMPARE(t.width, 120.0);
        QCOMPARE(t.cx - t.width / 2, 60.0);
        const LayerTransform edge = dragged(start, Handle::Right, at(140, 100), at(180, 100), {true, false});
        QCOMPARE(edge.width, 120.0);
        QCOMPARE(edge.height, 60.0); // the other side follows
        QCOMPARE(edge.cy, 100.0);    // about the middle of the held side
        const LayerTransform centered = dragged(start, Handle::BottomRight, at(140, 120), at(150, 130), {false, true});
        QCOMPARE(centered.width, 100.0);
        QCOMPARE(centered.height, 60.0);
        QCOMPARE(centered.cx, 100.0);
        QCOMPARE(centered.cy, 100.0);
    }

    void theSizeStaysAtLeastOnePixelAndNeverFlips() {
        const LayerTransform start = box(100, 100, 80, 40);
        const LayerTransform t = dragged(start, Handle::Right, at(140, 100), at(0, 100), {});
        QCOMPARE(t.width, 1.0);
        QCOMPARE(t.cx - t.width / 2, 60.0);
        const LayerTransform huge = dragged(start, Handle::Right, at(140, 100), at(1e9, 100), {});
        QCOMPARE(huge.width, double(canvas::maxTransformSide));
    }

    void aTurnedBoxScalesAlongItsOwnEdges() {
        const LayerTransform start = box(100, 100, 80, 40, 90); // turned a quarter: its width runs down the screen
        // The right edge of the box is now at the bottom of the screen: dragging it 20 px down widens the box by 20.
        const std::vector<crop::Point> c = corners(start);
        QCOMPARE(std::round(c[1].x), 120.0); // top right corner of the box, turned: (120, 140)
        QCOMPARE(std::round(c[1].y), 140.0);
        const LayerTransform t = dragged(start, Handle::Right, at(100, 140), at(100, 160), {});
        QCOMPARE(t.width, 100.0);
        QCOMPARE(t.height, 40.0);
        QCOMPARE(t.angle, 90.0);
        QCOMPARE(std::round(t.cy), 110.0); // the held edge (the left, now the top) stayed at y 60
    }

    void aDragIsFromWhereItStartedSoGoingOutAndBackReturns() {
        const LayerTransform start = box(100, 100, 80, 40);
        const LayerTransform out = dragged(start, Handle::BottomRight, at(140, 120), at(500, 500), {});
        QVERIFY(out.width > 300);
        QVERIFY(dragged(start, Handle::BottomRight, at(140, 120), at(140, 120), {}) == start.normalized());
    }

    void outsideACornerTurnsAboutTheCenterAndShiftStepsBy15() {
        const LayerTransform start = box(100, 100, 80, 40);
        const LayerTransform quarter = dragged(start, Handle::Rotate, at(200, 100), at(100, 200), {}); // from east to south: clockwise on screen
        QCOMPARE(quarter.angle, 90.0);
        QCOMPARE(quarter.cx, 100.0);
        const LayerTransform stepped = dragged(start, Handle::Rotate, at(200, 100), at(200, 135), {true, false});
        QCOMPARE(std::fmod(stepped.angle, 15.0), 0.0);
        QVERIFY(std::abs(stepped.angle - 19.3) < 8);
        // The turn adds to the angle the box has.
        QCOMPARE(dragged(box(0, 0, 10, 10, 30), Handle::Rotate, at(10, 0), at(0, 10), {}).angle, 120.0);
        // Past 180 it comes round.
        QCOMPARE(dragged(box(0, 0, 10, 10, 170), Handle::Rotate, at(10, 0), at(0, 10), {}).angle, -100.0);
    }

    void aMoveShiftsAndShiftHoldsOneAxis() {
        const LayerTransform start = box(100, 100, 80, 40);
        const LayerTransform t = dragged(start, Handle::Move, at(90, 90), at(100, 95), {});
        QCOMPARE(t.cx, 110.0);
        QCOMPARE(t.cy, 105.0);
        const LayerTransform along = dragged(start, Handle::Move, at(90, 90), at(130, 95), {true, false});
        QCOMPARE(along.cx, 140.0);
        QCOMPARE(along.cy, 100.0);
    }

    void snappingPullsAMoveByTheNearestEdgeAndAHandleByItsOwnEdge() {
        snap::Targets targets;
        targets.xs = {200};
        targets.ys = {50};
        targets.threshold = 6;
        const LayerTransform start = box(100, 100, 80, 40); // 60..140 x 80..120
        // Moved so its right edge is at 197: pulled to the guide at 200.
        const LayerTransform moved = dragged(start, Handle::Move, at(0, 0), at(57, 0), {});
        const LayerTransform pulled = snapped(start, moved, Handle::Move, {}, targets);
        QCOMPARE(pulled.cx + pulled.width / 2, 200.0);
        QCOMPARE(pulled.cy, 100.0); // no horizontal guide near
        // A layer partly off the canvas is not clamped: its left edge at -3 is pulled to a guide at 0.
        snap::Targets zero;
        zero.xs = {0};
        zero.threshold = 6;
        const LayerTransform off = box(37, 100, 80, 40); // left edge at -3
        QCOMPARE(snapped(off, off, Handle::Move, {}, zero).cx, 40.0);
        // The right handle pulls the right edge only.
        const LayerTransform widened = dragged(start, Handle::Right, at(140, 100), at(198, 100), {});
        const LayerTransform edge = snapped(start, widened, Handle::Right, {}, targets);
        QCOMPARE(edge.cx + edge.width / 2, 200.0);
        QCOMPARE(edge.cx - edge.width / 2, 60.0);
        // With Shift, Alt or a turn it is not snapped.
        QCOMPARE(snapped(start, widened, Handle::Right, {true, false}, targets).width, widened.width);
        // Nothing is pulled when the lines are off (threshold 0).
        snap::Targets none;
        QVERIFY(snapped(start, moved, Handle::Move, {}, none) == moved);
    }

    void theQmlObjectUsesTheGuidesAndAnswersWithTheDrawnCorners() {
        QTemporaryDir dir;
        canvas::CanvasStore store(dir.path());
        mask::PaintDocument d(400, 300);
        d.loadLayer(mask::LayerKind::Paint, "A", true, 1, mask::MaskPaint(400, 300, 4));
        const canvas::CanvasInfo made = store.create("g", d, "test").value();
        GuidesModel guides;
        guides.setStoreRoot(dir.path());
        guides.setCanvasId(made.id);
        guides.setCanvasSize(QSize(400, 300));
        QVERIFY(guides.addGuide("x", 200));
        TransformGesture gesture;
        gesture.setGuides(&guides);
        gesture.setSnapOptions({{"guides", true}, {"zoom", 1.0}});
        const QVariantMap start = gesture.describe({{"cx", 100.0}, {"cy", 100.0}, {"width", 80.0}, {"height", 40.0}, {"angle", 0.0}});
        QCOMPARE(start.value("x").toDouble(), 60.0);
        QCOMPARE(start.value("corners").toList().size(), 4);
        const QVariantMap moved = gesture.dragged(start, "move", {{"x", 0}, {"y", 0}}, {{"x", 57}, {"y", 0}}, false, false);
        QCOMPARE(moved.value("cx").toDouble() + 40, 200.0); // snapped to the guide
        const QVariantMap free = gesture.dragged(start, "move", {{"x", 0}, {"y", 0}}, {{"x", 57}, {"y", 0}}, false, false, false);
        QCOMPARE(free.value("cx").toDouble(), 157.0);
        QCOMPARE(gesture.dragged(start, "nonsense", {}, {}, false, false).value("cx").toDouble(), 100.0);
        // The options bar's fields: a width with the link on scales the height along.
        const QVariantMap wider = gesture.changed(start, {{"width", 160.0}}, true);
        QCOMPARE(wider.value("height").toDouble(), 80.0);
        QCOMPARE(gesture.changed(start, {{"width", 160.0}}, false).value("height").toDouble(), 40.0);
        QCOMPARE(gesture.changed(start, {{"width", 0.0}}, false).value("width").toDouble(), 1.0);
        QCOMPARE(gesture.nudged(start, 1.4, -10).value("cx").toDouble(), 101.0);
    }
};

QTEST_GUILESS_MAIN(TestTransformGesture)
#include "test_transform_gesture.moc"
