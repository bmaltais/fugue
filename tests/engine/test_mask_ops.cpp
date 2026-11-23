// Select > Modify and Transform Selection's pure core (#251): grow, shrink, feather, border, smooth and a placement of
// a flat 8-bit mask. Plain numbers; the Canvas view's commands only hand the selection to these.
//
// Seam: engine/mask_ops.h.

#include <QtTest>

#include <cmath>

#include "engine/mask_ops.h"

using namespace fugue::mask;
using namespace fugue::mask::ops;

namespace {

constexpr int W = 60, H = 50;

// A solid rectangle (x, y, w, h) of 255 in a W x H mask.
std::vector<uint8_t> rectangle(int x, int y, int w, int h) {
    std::vector<uint8_t> m(size_t(W) * H, 0);
    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i)
            m[size_t(j) * W + i] = 255;
    return m;
}

int at(const std::vector<uint8_t> &m, int x, int y) { return m[size_t(y) * W + x]; }
int count(const std::vector<uint8_t> &m) {
    int n = 0;
    for (uint8_t v : m)
        n += v > 0;
    return n;
}

} // namespace

class TestMaskOps : public QObject {
    Q_OBJECT

private slots:
    void boundsHoldTheSelection() {
        QCOMPARE(bounds(rectangle(10, 12, 8, 6), W, H), (IRect{10, 12, 8, 6}));
        QVERIFY(bounds(std::vector<uint8_t>(size_t(W) * H, 0), W, H).empty());
    }

    void growMovesEveryEdgeByTheAmount() {
        const auto out = expanded(rectangle(20, 20, 10, 10), W, H, 3);
        const IRect box = bounds(out, W, H);
        QCOMPARE(box, (IRect{17, 17, 16, 16}));
        QCOMPARE(at(out, 25, 17), 255); // straight out from an edge: the full amount
        QCOMPARE(at(out, 25, 16), 0);
    }

    void growRoundsTheCorners() {
        const auto out = expanded(rectangle(20, 20, 10, 10), W, H, 6);
        QCOMPARE(at(out, 14, 14), 0); // the corner of the square a box pass would make
        QCOMPARE(at(out, 14, 25), 255);
    }

    void shrinkEatsEveryEdgeByTheAmount() {
        const auto out = expanded(rectangle(20, 20, 10, 10), W, H, -3);
        QCOMPARE(bounds(out, W, H), (IRect{23, 23, 4, 4}));
        QVERIFY(bounds(expanded(rectangle(20, 20, 10, 10), W, H, -5), W, H).empty());
    }

    void growThenShrinkKeepsAnEmptyMaskEmpty() {
        const std::vector<uint8_t> none(size_t(W) * H, 0);
        QCOMPARE(expanded(none, W, H, 5), none);
        QCOMPARE(expanded(none, W, H, -5), none);
    }

    void aSelectionAtThePictureEdgeIsNotEatenFromThatSide() {
        const auto out = expanded(rectangle(0, 0, 20, 20), W, H, -3);
        QCOMPARE(bounds(out, W, H), (IRect{0, 0, 17, 17}));
    }

    void amountZeroChangesNothing() {
        const auto m = rectangle(5, 5, 9, 9);
        QCOMPARE(expanded(m, W, H, 0), m);
        QCOMPARE(feathered(m, W, H, 0), m);
        QCOMPARE(smoothed(m, W, H, 0), m);
    }

    void featherFadesTheEdgeAndKeepsTheCore() {
        const auto out = feathered(rectangle(20, 15, 20, 20), W, H, 6);
        QCOMPARE(at(out, 30, 25), 255);
        QCOMPARE(at(out, 29, 15) > 0 && at(out, 29, 15) < 255, true); // on the old edge: half
        QVERIFY(std::abs(at(out, 29, 15) - 128) < 40);
        QVERIFY(at(out, 30, 14) > 0 && at(out, 30, 14) < at(out, 30, 15));
        QCOMPARE(at(out, 30, 5), 0); // beyond the reach
        // A fade is monotonic across the edge.
        for (int y = 9; y < 24; ++y)
            QVERIFY(at(out, 30, y) <= at(out, 30, y + 1));
    }

    void featherKeepsASelectionSolidAtThePictureEdge() {
        const auto out = feathered(rectangle(0, 0, 30, 30), W, H, 6);
        QCOMPARE(at(out, 0, 0), 255);
        QCOMPARE(at(out, 0, 15), 255);
        QVERIFY(at(out, 30, 15) > 0 && at(out, 30, 15) < 255);
    }

    void borderIsABandAroundTheEdge() {
        const auto m = rectangle(20, 20, 20, 20);
        const auto out = border(m, W, H, 4);
        QCOMPARE(at(out, 30, 30), 0);  // the inside goes
        QCOMPARE(at(out, 30, 21), 255); // 2 px inside
        QCOMPARE(at(out, 30, 18), 255); // 2 px outside
        QCOMPARE(at(out, 30, 17), 0);
        QCOMPARE(at(out, 30, 23), 0);
        const auto one = border(m, W, H, 1);
        QCOMPARE(at(one, 30, 19), 255); // an odd width: the extra pixel is outside
        QCOMPARE(at(one, 30, 20), 0);
        QCOMPARE(at(one, 30, 18), 0);
    }

    void smoothDropsSpecksAndRoundsCorners() {
        auto m = rectangle(20, 20, 20, 20);
        m[size_t(5) * W + 5] = 255; // a speck
        const auto out = smoothed(m, W, H, 4);
        QCOMPARE(at(out, 5, 5), 0);
        QCOMPARE(at(out, 30, 30), 255);
        QCOMPARE(at(out, 20, 20), 0);   // the corner pixel is rounded off
        QVERIFY(count(out) < count(rectangle(20, 20, 20, 20)));
        for (uint8_t v : out)
            QVERIFY(v == 0 || v == 255);
    }

    void moveShiftsTheSelection() {
        Placement p;
        p.dx = 7;
        p.dy = -4;
        const auto out = transformed(rectangle(20, 20, 10, 10), W, H, p);
        QCOMPARE(bounds(out, W, H), (IRect{27, 16, 10, 10}));
    }

    void scaleGrowsAboutTheCenter() {
        Placement p;
        p.scaleX = 2;
        p.scaleY = 0.5;
        const IRect box = bounds(transformed(rectangle(20, 20, 10, 10), W, H, p), W, H);
        QVERIFY(std::abs(box.width - 20) <= 2 && std::abs(box.height - 5) <= 2); // soft edges add a pixel
        QVERIFY(std::abs((box.x + box.width / 2.0) - 25) <= 1 && std::abs((box.y + box.height / 2.0) - 25) <= 1);
    }

    void aQuarterTurnSwapsTheSides() {
        Placement p;
        p.degrees = 90;
        const IRect box = bounds(transformed(rectangle(20, 20, 16, 6), W, H, p), W, H);
        QVERIFY(std::abs(box.width - 6) <= 1 && std::abs(box.height - 16) <= 1);
    }

    void whatLeavesThePictureIsLost() {
        Placement p;
        p.dx = 100;
        QVERIFY(bounds(transformed(rectangle(20, 20, 10, 10), W, H, p), W, H).empty());
    }

    void assignIsOneUndoStepAndRefusesAWrongSize() {
        MaskPaint paint(W, H);
        paint.fillRect(20, 20, 10, 10);
        const size_t steps = paint.undoSteps();
        QVERIFY(paint.assign(expanded(paint.pixels(), W, H, 4)));
        QCOMPARE(paint.undoSteps(), steps + 1);
        QCOMPARE(bounds(paint.pixels(), W, H), (IRect{16, 16, 18, 18}));
        paint.undo();
        QCOMPARE(bounds(paint.pixels(), W, H), (IRect{20, 20, 10, 10}));
        paint.redo();
        QCOMPARE(bounds(paint.pixels(), W, H), (IRect{16, 16, 18, 18}));
        QVERIFY(!paint.assign(std::vector<uint8_t>(5, 0)));
        // The same pixels are no step.
        const size_t again = paint.undoSteps();
        QVERIFY(paint.assign(paint.pixels()));
        QCOMPARE(paint.undoSteps(), again);
    }

    void identityAndEmptyReturnTheSame() {
        const auto m = rectangle(20, 20, 10, 10);
        QCOMPARE(transformed(m, W, H, Placement()), m);
        const std::vector<uint8_t> none(size_t(W) * H, 0);
        Placement p;
        p.degrees = 30;
        QCOMPARE(transformed(none, W, H, p), none);
    }
};

QTEST_GUILESS_MAIN(TestMaskOps)
#include "test_mask_ops.moc"
