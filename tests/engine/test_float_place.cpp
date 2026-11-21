// The place of a floating bar inside an area (#120): clamped to the area, remembered as a fraction of
// the room it has, snapped lightly to the edges and the middle, and the side a panel hung from it
// opens on. Plain numbers, no window.
//
// Seam: engine/float_place.h.

#include <QtTest>

#include "engine/float_place.h"

using namespace fugue::place;

namespace {
// A 1000 x 600 area, a 200 x 40 box, 16 from the edges: x runs 16 to 784, y 16 to 544.
const Bounds area{1000, 600, 200, 40, 16};
}

class TestFloatPlace : public QObject {
    Q_OBJECT

private slots:
    void clampedKeepsTheBoxInsideTheMargins() {
        QCOMPARE(clamped({-50, -50}, area), (Point{16, 16}));
        QCOMPARE(clamped({5000, 5000}, area), (Point{784, 544}));
        QCOMPARE(clamped({300, 200}, area), (Point{300, 200}));
    }

    void aBoxLargerThanTheAreaIsCentered() {
        const Bounds tight{100, 100, 200, 40, 16};
        QCOMPARE(clamped({0, 0}, tight).x, -50.0);
        QCOMPARE(clamped({90, 0}, tight).x, -50.0);
        QCOMPARE(clamped({0, 0}, tight).y, 16.0); // the y axis still has room
    }

    void fractionsMapTheFreeRoomOntoZeroToOne() {
        QCOMPARE(fromFraction({0, 0}, area), (Point{16, 16}));
        QCOMPARE(fromFraction({1, 1}, area), (Point{784, 544}));
        QCOMPARE(fromFraction({0.5, 0.5}, area), (Point{400, 280}));
        QCOMPARE(toFraction({400, 280}, area), (Frac{0.5, 0.5}));
        // Outside the room is the nearest end.
        QCOMPARE(toFraction({-10, 9999}, area), (Frac{0, 1}));
        // A fraction outside 0..1 is held to it.
        QCOMPARE(fromFraction({-3, 7}, area), (Point{16, 544}));
    }

    void aResizedAreaKeepsTheRelativePlace() {
        const Frac f = toFraction({784, 16}, area); // top right
        const Bounds smaller{500, 300, 200, 40, 16};
        QCOMPARE(fromFraction(f, smaller), (Point{284, 16}));
        // No room at all: the fraction is meaningless, the box is centered.
        const Bounds none{100, 40, 200, 40, 16};
        QCOMPARE(toFraction({0, 0}, none), (Frac{0.5, 0.5}));
    }

    void snapPullsToTheEdgesAndTheMiddleWithinTheThreshold() {
        // Near the left edge, 5 off: pulled; 20 off: left alone.
        QCOMPARE(snapped({21, 100}, area, 12).x, 16.0);
        QCOMPARE(snapped({36, 100}, area, 12).x, 36.0);
        // The middle of the x range is 400.
        QCOMPARE(snapped({407, 100}, area, 12).x, 400.0);
        // The right and bottom edges.
        QCOMPARE(snapped({780, 540}, area, 12), (Point{784, 544}));
        // The middle of the y range is 280.
        QCOMPARE(snapped({300, 275}, area, 12).y, 280.0);
        // Each axis on its own.
        QCOMPARE(snapped({401, 100}, area, 12), (Point{400, 100}));
        // Threshold 0 is no snapping.
        QCOMPARE(snapped({401, 17}, area, 0), (Point{401, 17}));
    }

    void movedStepsAndStopsAtTheEdge() {
        QCOMPARE(moved({100, 100}, 8, -8, area), (Point{108, 92}));
        QCOMPARE(moved({20, 100}, -80, 0, area), (Point{16, 100}));
        QCOMPARE(moved({780, 540}, 80, 80, area), (Point{784, 544}));
    }

    void aPanelOpensBelowWhenThereIsRoom() {
        const Rect bar{400, 16, 200, 40};
        const Point p = panelPlace(bar, {300, 200}, {1000, 600}, 8);
        QCOMPARE(p, (Point{350, 64})); // centered under the bar, 8 below
    }

    void aPanelOpensAboveWhenTheBarIsLow() {
        const Rect bar{400, 544, 200, 40};
        const Point p = panelPlace(bar, {300, 200}, {1000, 600}, 8);
        QCOMPARE(p, (Point{350, 336})); // 544 - 8 - 200
    }

    void aPanelPicksTheSideWithMoreRoomWhenNeitherFits() {
        // Room: 282 below, 302 above; a panel of 400 tall fits neither.
        const Rect bar{400, 310, 200, 40};
        QVERIFY(!opensBelow(bar, 400, {1000, 640}, 8));
        // It still stays inside the area: the caller limits its height to roomAbove.
        QCOMPARE(panelPlace(bar, {300, 400}, {1000, 640}, 8).y, 0.0);
        // A panel that fits below goes below even when above has more room.
        QVERIFY(opensBelow(bar, 250, {1000, 640}, 8));
    }

    void aPanelStaysInsideTheAreaSideways() {
        const Rect left{4, 16, 200, 40};
        QCOMPARE(panelPlace(left, {300, 100}, {1000, 600}, 8).x, 0.0);
        const Rect right{796, 16, 200, 40};
        QCOMPARE(panelPlace(right, {300, 100}, {1000, 600}, 8).x, 700.0);
    }

    void roomAroundReportsTheSpaceOnEachSide() {
        const Rect bar{400, 100, 200, 40};
        QCOMPARE(roomBelow(bar, {1000, 600}, 8), 600 - 140 - 8.0);
        QCOMPARE(roomAbove(bar, 8), 100 - 8.0);
    }
};

QTEST_GUILESS_MAIN(TestFloatPlace)
#include "test_float_place.moc"
