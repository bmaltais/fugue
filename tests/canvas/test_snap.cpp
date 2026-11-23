// The one snap function of the Canvas view (#253): points and rectangles pulled to guides, the grid and the Region node's
// Multiple of, on plain structs.

#include <QtTest>

#include "canvas/snap.h"

using namespace fugue;
using crop::Rect;
using crop::Size;
using snap::Targets;

namespace {
// Targets built by moving the lines in (assigning an initializer list to a vector member trips a false -Wnonnull in GCC 16).
Targets lines(std::vector<int> xs, std::vector<int> ys, double threshold, int multiple = 0) {
    Targets t;
    t.xs = std::move(xs);
    t.ys = std::move(ys);
    t.threshold = threshold;
    t.multiple = multiple;
    return t;
}
} // namespace

class TestSnap : public QObject {
    Q_OBJECT

private slots:
    void aCoordinateGoesToTheNearestLineWithinTheThreshold() {
        bool hit = false;
        QCOMPARE(snap::snapCoordinate(103, std::vector<int>{100, 150}, 0, 8, &hit), 100.0);
        QVERIFY(hit);
        QCOMPARE(snap::snapCoordinate(147, {100, 150}, 0, 8, &hit), 150.0);
        QCOMPARE(snap::snapCoordinate(120, {100, 150}, 0, 8, &hit), 120.0); // too far from both
        QVERIFY(!hit);
        QCOMPARE(snap::snapCoordinate(103, {100}, 0, 0, &hit), 103.0);      // no threshold: lines are off
        QVERIFY(!hit);
    }

    void theGridIsLinesEverySpacingAndAGuideWinsATie() {
        QCOMPARE(snap::snapCoordinate(66, {}, 64, 8), 64.0);
        QCOMPARE(snap::snapCoordinate(200, {}, 64, 8), 192.0);
        QCOMPARE(snap::snapCoordinate(100, {}, 64, 8), 100.0);
        QCOMPARE(snap::snapCoordinate(68, {70}, 64, 8), 70.0); // a guide at 2 beats the grid line at 4
        QCOMPARE(snap::snapCoordinate(66, {70}, 64, 8), 64.0); // the grid is nearer here
        QCOMPARE(snap::snapCoordinate(67, {70}, 64, 8), 70.0); // equally near: the guide wins
    }

    void aPointSnapsEachAxisAgainstItsOwnLines() {
        const Targets t = lines({100}, {300}, 5);
        const crop::Point p = snap::snapPoint({103, 297}, t);
        QCOMPARE(p.x, 100.0);
        QCOMPARE(p.y, 300.0);
        const crop::Point far = snap::snapPoint({103, 280}, t);
        QCOMPARE(far.y, 280.0);
    }

    void aResizedRectangleHasEachEdgePulledOnItsOwn() {
        const Targets t = lines({100, 300}, {50}, 6);
        const Rect r = snap::snapRect({103, 54, 194, 200}, t, Size{1000, 800});
        QCOMPARE(r, (Rect{100, 50, 200, 204})); // left 103 -> 100, right 297 -> 300, top 54 -> 50, bottom (254) free
    }

    void aMovedRectangleKeepsItsSizeAndFollowsTheNearerEdge() {
        const Targets t = lines({100, 400}, {}, 8);
        const Rect moved = snap::snapRect({105, 10, 297, 40}, t, Size{1000, 800}, true); // right edge at 402 is 2 off 400, the left 5 off 100: the right wins, shift -2
        QCOMPARE(moved, (Rect{103, 10, 297, 40}));
        const Rect left = snap::snapRect({102, 10, 200, 40}, t, Size{1000, 800}, true); // left 2 off, right 302 far
        QCOMPARE(left, (Rect{100, 10, 200, 40}));
    }

    void nothingIsNearNothingMoves() {
        const Targets t = lines({500}, {}, 4);
        QCOMPARE(snap::snapRect({10, 10, 50, 50}, t, Size{1000, 800}), (Rect{10, 10, 50, 50}));
        QCOMPARE(snap::snapRect({10, 10, 50, 50}, Targets{}, Size{1000, 800}), (Rect{10, 10, 50, 50}));
    }

    void theMultipleIsTheRegionsRuleAndALineNearEnoughWinsOverIt() {
        Targets t;
        t.multiple = 16;
        // The Multiple alone is region::snapped's, as the Region frame always had it.
        QCOMPARE(snap::snapRect({107, 55, 203, 98}, t, Size{1000, 800}), (Rect{112, 48, 192, 112}));
        // A guide at 105 pulls the left edge off the multiple; the other edges keep the multiple's.
        t = lines({105}, {}, 6, 16);
        const Rect r = snap::snapRect({107, 55, 203, 98}, t, Size{1000, 800});
        QCOMPARE(r.x, 105);
        QCOMPARE(r.x + r.width, 304); // the right edge: 310 -> multiple 304
        QCOMPARE(r.y, 48);
    }

    void aPullNeverClosesTheRectangleOrLeavesTheCanvas() {
        const Targets t = lines({100}, {}, 20);
        const Rect thin = snap::snapRect({95, 0, 8, 10}, t, Size{1000, 800}); // both edges would land on 100
        QVERIFY(thin.width >= 1);
        const Rect out = snap::snapRect({990, 0, 10, 10}, lines({1005}, {}, 20), Size{1000, 800});
        QVERIFY(out.x + out.width <= 1000);
    }
};

QTEST_GUILESS_MAIN(TestSnap)
#include "test_snap.moc"
