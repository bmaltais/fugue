// The geometry of a crop frame (#73): a rectangle of whole pixels in an image, moved and resized by
// handles, with an optional ratio lock, and the mapping between image pixels and the coordinates
// of a view (a node's face, the large view). Plain numbers, no window: the frame on the node and in
// the large view are drawn from these, and any other caller can use them.
//
// Seam: engine/crop_geometry.h.

#include <QtTest>

#include "engine/crop_geometry.h"

using namespace fugue::crop;

namespace {
const Size image{200, 100};
}

class TestCropGeometry : public QObject {
    Q_OBJECT

private slots:
    // ---- what the Crop node applies ------------------------------------------------------

    void appliedIsWhatTheCropNodeCuts() {
        // A rectangle inside the image is itself.
        QCOMPARE(applied({10, 20, 30, 40}, image), (Rect{10, 20, 30, 40}));
        // One that runs past the edge stops at the edge (the manifest's wording).
        QCOMPARE(applied({150, 20, 100, 40}, image), (Rect{150, 20, 50, 40}));
        QCOMPARE(applied({10, 90, 30, 40}, image), (Rect{10, 90, 30, 10}));
        // The origin is held inside the image, the size at one pixel at least.
        QCOMPARE(applied({500, 500, 0, -3}, image), (Rect{199, 99, 1, 1}));
        QCOMPARE(applied({-5, -5, 20, 20}, image), (Rect{0, 0, 20, 20}));
    }

    void clampedKeepsTheSizeAndSlidesInside() {
        QCOMPARE(clamped({190, 95, 30, 40}, image), (Rect{170, 60, 30, 40}));
        QCOMPARE(clamped({-9, -9, 30, 40}, image), (Rect{0, 0, 30, 40}));
        // Larger than the image: the image.
        QCOMPARE(clamped({10, 10, 900, 900}, image), (Rect{0, 0, 200, 100}));
        QCOMPARE(clamped({10, 10, 0, 0}, image), (Rect{10, 10, 1, 1}));
    }

    // ---- moving --------------------------------------------------------------------------

    void movedKeepsTheSizeAndStopsAtTheEdges() {
        QCOMPARE(moved({10, 10, 50, 40}, 5, -3, image), (Rect{15, 7, 50, 40}));
        QCOMPARE(moved({10, 10, 50, 40}, 500, 500, image), (Rect{150, 60, 50, 40}));
        QCOMPARE(moved({10, 10, 50, 40}, -500, -500, image), (Rect{0, 0, 50, 40}));
        // Against one edge it still slides along the other.
        QCOMPARE(moved({150, 10, 50, 40}, 30, 8, image), (Rect{150, 18, 50, 40}));
    }

    // ---- resizing, free ------------------------------------------------------------------

    void aCornerMovesTwoEdgesAndHoldsTheOpposite() {
        const Rect start{50, 20, 60, 40};
        QCOMPARE(resized(start, Handle::BottomRight, 10, 5, image), (Rect{50, 20, 70, 45}));
        QCOMPARE(resized(start, Handle::TopLeft, -10, -5, image), (Rect{40, 15, 70, 45}));
        QCOMPARE(resized(start, Handle::TopRight, 10, -5, image), (Rect{50, 15, 70, 45}));
        QCOMPARE(resized(start, Handle::BottomLeft, -10, 5, image), (Rect{40, 20, 70, 45}));
    }

    void anEdgeMovesOneEdge() {
        const Rect start{50, 20, 60, 40};
        QCOMPARE(resized(start, Handle::Right, 10, 99, image), (Rect{50, 20, 70, 40}));
        QCOMPARE(resized(start, Handle::Left, -10, 99, image), (Rect{40, 20, 70, 40}));
        QCOMPARE(resized(start, Handle::Top, 99, -5, image), (Rect{50, 15, 60, 45}));
        QCOMPARE(resized(start, Handle::Bottom, 99, 5, image), (Rect{50, 20, 60, 45}));
    }

    void aHandleStopsAtTheImageEdgeAndAtOnePixel() {
        const Rect start{50, 20, 60, 40};
        QCOMPARE(resized(start, Handle::BottomRight, 999, 999, image), (Rect{50, 20, 150, 80}));
        QCOMPARE(resized(start, Handle::TopLeft, -999, -999, image), (Rect{0, 0, 110, 60}));
        // Dragged past the opposite edge: it stops one pixel from it, it never flips or vanishes.
        QCOMPARE(resized(start, Handle::Right, -999, 0, image), (Rect{50, 20, 1, 40}));
        QCOMPARE(resized(start, Handle::Left, 999, 0, image), (Rect{109, 20, 1, 40}));
        QCOMPARE(resized(start, Handle::Bottom, 0, -999, image), (Rect{50, 20, 60, 1}));
        QCOMPARE(resized(start, Handle::Top, 0, 999, image), (Rect{50, 59, 60, 1}));
    }

    void fromTheCenterTheOppositeEdgeMirrors() {
        const Rect start{50, 20, 60, 40}; // center 80, 40
        QCOMPARE(resized(start, Handle::Right, 10, 0, image, 0, true), (Rect{40, 20, 80, 40}));
        QCOMPARE(resized(start, Handle::TopLeft, -10, -5, image, 0, true), (Rect{40, 15, 80, 50}));
        // The limit is the nearer image edge on each axis, the center stays: 80 from the left edge
        // and 40 from the top.
        QCOMPARE(resized(start, Handle::BottomRight, 999, 999, image, 0, true), (Rect{0, 0, 160, 80}));
    }

    // ---- resizing, ratio lock --------------------------------------------------------------

    void aCornerKeepsTheRatioFollowingTheLargerDrag() {
        const Rect start{20, 20, 40, 40}; // 1:1
        QCOMPARE(resized(start, Handle::BottomRight, 20, 5, image, 1.0), (Rect{20, 20, 60, 60}));
        QCOMPARE(resized(start, Handle::BottomRight, 5, 20, image, 1.0), (Rect{20, 20, 60, 60}));
        QCOMPARE(resized(start, Handle::TopLeft, -10, -2, image, 1.0), (Rect{10, 10, 50, 50}));
    }

    void anEdgeKeepsTheRatioAroundTheMiddleOfTheOtherAxis() {
        const Rect start{50, 30, 40, 40}; // middle y = 50
        // 2:1 and the right edge drags out 20: 60 wide, 30 high, centered on y = 50.
        QCOMPARE(resized(start, Handle::Right, 20, 0, image, 2.0), (Rect{50, 35, 60, 30}));
        // A top edge drag sets the height; the width follows around x = 70.
        QCOMPARE(resized(start, Handle::Top, 0, -10, image, 2.0), (Rect{20, 20, 100, 50}));
    }

    void theRatioHoldsAtTheEdgesToo() {
        const Rect start{150, 60, 40, 30}; // 4:3
        const double ratio = 4.0 / 3.0;
        const Rect big = resized(start, Handle::BottomRight, 999, 999, image, ratio);
        QCOMPARE(big, (Rect{150, 60, 50, 38})); // the image's right edge limits: 50 wide, 37.5 high
        QVERIFY(qAbs(double(big.width) / big.height - ratio) < 0.05);
        const Rect small = resized(start, Handle::BottomRight, -999, -999, image, ratio);
        QVERIFY(small.width >= 1 && small.height >= 1);
        QVERIFY(qAbs(double(small.width) / small.height - ratio) < 1.0);
    }

    void aLockedFrameFromTheCenterStaysInside() {
        const Rect start{90, 40, 20, 20};
        const Rect big = resized(start, Handle::BottomRight, 999, 999, image, 1.0, true);
        QCOMPARE(big, (Rect{50, 0, 100, 100}));
    }

    void fitRatioShrinksTheLongSideAroundTheCenter() {
        QCOMPARE(fitRatio({0, 0, 200, 100}, 1.0, image), (Rect{50, 0, 100, 100}));
        QCOMPARE(fitRatio({0, 0, 200, 100}, 16.0 / 9.0, image), (Rect{11, 0, 178, 100}));
        QCOMPARE(fitRatio({20, 10, 40, 80}, 2.0, image), (Rect{20, 40, 40, 20}));
        // A free ratio leaves it alone.
        QCOMPARE(fitRatio({20, 10, 40, 80}, 0, image), (Rect{20, 10, 40, 80}));
    }

    void ratiosAreNamedOrWrittenAsAPair() {
        QCOMPARE(ratioOf("free", image), 0.0);
        QCOMPARE(ratioOf("", image), 0.0);
        QCOMPARE(ratioOf("source", image), 2.0);
        QCOMPARE(ratioOf("1:1", image), 1.0);
        QVERIFY(qAbs(ratioOf("16:9", image) - 16.0 / 9.0) < 1e-9);
        QCOMPARE(ratioOf("3:4", image), 0.75);
        QCOMPARE(ratioOf("0:5", image), 0.0);
        QCOMPARE(ratioOf("nonsense", image), 0.0);
    }

    // ---- which handle is under a point -----------------------------------------------------

    void handleAtTellsCornersEdgesAndTheInside() {
        const Rect frame{50, 20, 100, 60};
        const double grab = 6; // image pixels (the caller divides screen pixels by the scale)
        QCOMPARE(handleAt(frame, 50, 20, grab), Handle::TopLeft);
        QCOMPARE(handleAt(frame, 150, 80, grab), Handle::BottomRight);
        QCOMPARE(handleAt(frame, 150, 20, grab), Handle::TopRight);
        QCOMPARE(handleAt(frame, 50, 80, grab), Handle::BottomLeft);
        QCOMPARE(handleAt(frame, 100, 20, grab), Handle::Top);
        QCOMPARE(handleAt(frame, 100, 80, grab), Handle::Bottom);
        QCOMPARE(handleAt(frame, 50, 50, grab), Handle::Left);
        QCOMPARE(handleAt(frame, 150, 50, grab), Handle::Right);
        QCOMPARE(handleAt(frame, 100, 50, grab), Handle::Inside);
        QCOMPARE(handleAt(frame, 10, 10, grab), Handle::None);
        // The grab reaches a little outside the frame too.
        QCOMPARE(handleAt(frame, 46, 17, grab), Handle::TopLeft);
    }

    void aSmallFrameKeepsItsInsideToMove() {
        // At 8 px wide a grab of 6 would be all handles; the inside middle stays for moving.
        const Rect frame{50, 20, 8, 8};
        QCOMPARE(handleAt(frame, 54, 24, 6), Handle::Inside);
        QCOMPARE(handleAt(frame, 50, 20, 6), Handle::TopLeft);
    }

    // ---- pixels and view coordinates ---------------------------------------------------------

    void aMappingGoesBothWays() {
        const Mapping map{30, 10, 2.0}; // image pixel (0, 0) at view (30, 10), 2 view units a pixel
        QCOMPARE(map.toView(0, 0), (Point{30, 10}));
        QCOMPARE(map.toView(10, 5), (Point{50, 20}));
        QCOMPARE(map.toImage(50, 20), (Point{10, 5}));
        QCOMPARE(map.toViewRect({10, 5, 20, 10}), (RectF{50, 20, 40, 20}));
    }

    void aPixelRectIsTheOneUnderTwoViewPoints() {
        const Mapping map{30, 10, 2.0};
        // Corners round to the nearest pixel edge, the order of the points does not matter, and the
        // result stays in the image.
        QCOMPARE(map.imageRect(50, 20, 90, 40, image), (Rect{10, 5, 20, 10}));
        QCOMPARE(map.imageRect(90, 40, 50, 20, image), (Rect{10, 5, 20, 10}));
        QCOMPARE(map.imageRect(-100, -100, 1000, 1000, image), (Rect{0, 0, 200, 100}));
        QCOMPARE(map.imageRect(51, 21, 51, 21, image).width, 1); // never empty
    }

    void fittedCentersTheImageInABox() {
        const Mapping wide = fitted(image, 400, 400); // 200 x 100 into 400 x 400: scale 2, centered
        QCOMPARE(wide.scale, 2.0);
        QCOMPARE(wide.originX, 0.0);
        QCOMPARE(wide.originY, 100.0);
        const Mapping tall = fitted({100, 200}, 400, 100); // scale 0.5
        QCOMPARE(tall.scale, 0.5);
        QCOMPARE(tall.originX, 175.0);
        QCOMPARE(tall.originY, 0.0);
        // Nothing to fit into an empty box: no division by zero.
        QCOMPARE(fitted({0, 0}, 10, 10).scale, 1.0);
    }

    // ---- a drag is a path of positions ----------------------------------------------------------

    void theResultOfADragDependsOnTheStartAndTheTotalOffsetOnly() {
        // The same total offset reaches the same rectangle however the pointer got there.
        const Rect start{50, 20, 60, 40};
        const Rect a = resized(start, Handle::BottomRight, 12, 9, image);
        const Rect viaFar = resized(start, Handle::BottomRight, 12, 9, image);
        QCOMPARE(a, viaFar);
        // And going out past an edge and back returns to where the offset says, not stuck at the edge.
        QCOMPARE(resized(start, Handle::BottomRight, 5, 5, image), (Rect{50, 20, 65, 45}));
    }
};

QTEST_GUILESS_MAIN(TestCropGeometry)
#include "test_crop_geometry.moc"
