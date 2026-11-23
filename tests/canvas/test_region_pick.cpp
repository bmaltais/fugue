// The numbers behind the region picker (#213, epic #206, spec 14.5): snapping to the node's multiple, the
// context box and the model size. The context and the size must be what Canvas Region computes, so the
// last tests run the node's own functions beside them.

#include <QtTest>

#include "canvas/region_pick.h"
#include "nodes/canvas_region_ops.h"

using namespace fugue;
using crop::Rect;
using crop::Size;

class TestRegionPick : public QObject {
    Q_OBJECT

private slots:
    void theStartingRectangleIsTheNodesAndZeroRunsToTheEdge() {
        const Size canvas{1000, 800};
        QCOMPARE(region::initial(100, 50, 200, 100, canvas), (Rect{100, 50, 200, 100}));
        QCOMPARE(region::initial(900, 700, 0, 0, canvas), (Rect{900, 700, 100, 100}));
        QCOMPARE(region::initial(0, 0, 5000, 5000, canvas), (Rect{0, 0, 1000, 800})); // cut to the canvas
        QCOMPARE(region::initial(2000, 0, 10, 10, canvas), (Rect{0, 0, 1000, 800}));   // nothing inside: the whole canvas
    }

    void snapRoundsEveryEdgeToTheNearestMultiple() {
        const Size canvas{1000, 800};
        QCOMPARE(region::snapped({107, 55, 203, 98}, 16, canvas), (Rect{112, 48, 192, 112}));
        QCOMPARE(region::snapped({112, 48, 192, 112}, 16, canvas), (Rect{112, 48, 192, 112})); // idempotent
        QCOMPARE(region::snapped({107, 55, 203, 98}, 1, canvas), (Rect{107, 55, 203, 98}));    // 1 is no snap
    }

    void snapKeepsAtLeastOneStepAndStaysInsideTheCanvas() {
        const Size canvas{1000, 800};
        QCOMPARE(region::snapped({100, 100, 3, 3}, 16, canvas), (Rect{96, 96, 16, 16}));
        // The far edge of a canvas that is not a multiple is reachable, and a rectangle ending near it ends on it.
        QCOMPARE(region::snapped({900, 700, 100, 100}, 64, canvas), (Rect{896, 704, 104, 96}));
        QCOMPARE(region::snapped({990, 790, 10, 10}, 16, canvas), (Rect{992, 784, 8, 16}));
        const Rect inside = region::snapped({990, 795, 10, 5}, 64, canvas);
        QVERIFY(inside.x >= 0 && inside.x + inside.width <= 1000 && inside.y >= 0 && inside.y + inside.height <= 800);
        QVERIFY(inside.width > 0 && inside.height > 0);
        // A canvas smaller than the step still gives a rectangle.
        QCOMPARE(region::snapped({0, 0, 5, 5}, 16, {10, 10}), (Rect{0, 0, 10, 10}));
    }

    void snapWithKeepSizeOnlyMovesTheTopLeft() {
        const Size canvas{1000, 800};
        QCOMPARE(region::snapped({107, 55, 203, 98}, 16, canvas, true), (Rect{112, 48, 203, 98}));
        // Held inside the canvas by its size.
        QCOMPARE(region::snapped({990, 700, 100, 100}, 16, canvas, true), (Rect{900, 700, 100, 100}));
    }

    void theContextIsTheRectangleGrownByThePaddingAndKeptInsideTheCanvas() {
        const Size canvas{1000, 800};
        QCOMPARE(region::context({100, 100, 200, 200}, 64, canvas), (Rect{36, 36, 328, 328}));
        QCOMPARE(region::context({0, 0, 200, 200}, 64, canvas), (Rect{0, 0, 328, 328}));            // slid in, not cut short
        QCOMPARE(region::context({0, 0, 1000, 800}, 64, canvas), (Rect{0, 0, 1000, 800}));          // the whole canvas is all there is
        QCOMPARE(region::context({100, 100, 200, 200}, 0, canvas), (Rect{84, 84, 232, 232})); // the least padding: room for the blend
        QCOMPARE(region::context({100, 100, 200, 200}, 0, canvas, 40), (Rect{44, 44, 312, 312})); // feather + 16
    }

    void theModelSizeFollowsScaleCapAndMultiple() {
        const Size canvas{4000, 4000};
        region::Settings settings;
        // 200 + 2 x 64 = 328, at scale 1 and multiple 16: 328 -> 336 (nearest) per side.
        QCOMPARE(region::modelSize({1000, 1000, 200, 200}, settings, canvas), (Size{336, 336}));
        settings.scale = 2;
        QCOMPARE(region::modelSize({1000, 1000, 200, 200}, settings, canvas), (Size{656, 656}));
        QVERIFY(!region::capped({1000, 1000, 200, 200}, settings, canvas));
        // 2000 x 1000 + padding, 1.5 MP cap: shape kept, sides rounded down so the cap is not passed.
        settings = {};
        const Size big = region::modelSize({100, 100, 2000, 1000}, settings, canvas);
        QVERIFY(big.width * big.height <= 1.5e6);
        QVERIFY(big.width % 16 == 0 && big.height % 16 == 0);
        QVERIFY(region::capped({100, 100, 2000, 1000}, settings, canvas));
        settings.capMegapixels = 0;
        QVERIFY(!region::capped({100, 100, 2000, 1000}, settings, canvas));
    }

    // The point of the class: the same answer as the node, for many rectangles.
    void contextAndSizeAreWhatTheNodeComputes() {
        const Size canvas{1024, 768};
        for (const Rect r : {Rect{0, 0, 1024, 768}, Rect{10, 20, 30, 40}, Rect{500, 300, 400, 400}, Rect{1000, 700, 24, 68}}) {
            for (const int padding : {0, 16, 64, 300}) {
                region::Settings settings;
                settings.padding = padding;
                settings.scale = 1.5;
                settings.capMegapixels = 0.5;
                settings.multiple = 8;
                settings.feather = padding == 16 ? 30 : 0;
                settings.targetMegapixels = padding == 300 ? 0.25 : 0;
                const nodes::CropBox bounds = *nodes::rectBox(r.x, r.y, r.width, r.height, canvas.width, canvas.height);
                const nodes::CropBox box = nodes::paddedBox(bounds, nodes::paddingFor(padding, settings.feather), canvas.width, canvas.height);
                const nodes::CropSize size = nodes::regionSize(box, settings.scale, settings.capMegapixels, settings.multiple, settings.targetMegapixels);
                QCOMPARE(region::context(r, padding, canvas, settings.feather), (Rect{box.x, box.y, box.width, box.height}));
                QCOMPARE(region::modelSize(r, settings, canvas), (Size{size.width, size.height}));
            }
        }
    }
};

QTEST_MAIN(TestRegionPick)
#include "test_region_pick.moc"
