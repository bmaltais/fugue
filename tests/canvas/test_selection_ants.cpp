// The marching ants' outline (#313): the contour of a mask at the view's resolution (a square's bounds and closed path, a
// triangle's, a hole, the 0.5 level of a feathered edge, a bound on the points of a huge canvas) and the SelectionAnts
// item that caches it per mask change and view (no window: the dashes' drawing is looked at in the Canvas view's picture).

#include <QtTest>

#include <algorithm>

#include "canvas/mask_layer.h"
#include "canvas/selection_ants.h"
#include "canvas/selection_contour.h"

using namespace fugue;

namespace {

QRectF bounds(const ContourPath &path) {
    double l = path.front().x(), r = l, t = path.front().y(), b = t;
    for (const QPointF &p : path) {
        l = std::min(l, p.x()); r = std::max(r, p.x());
        t = std::min(t, p.y()); b = std::max(b, p.y());
    }
    return QRectF(QPointF(l, t), QPointF(r, b));
}

bool closedPath(const ContourPath &path) { return path.size() > 3 && path.front() == path.back(); }

uint8_t square(int x, int y, int x0, int y0, int x1, int y1) { return x >= x0 && x < x1 && y >= y0 && y < y1 ? 255 : 0; }

} // namespace

class TestSelectionAnts : public QObject {
    Q_OBJECT
private slots:
    void aSquareHasOneClosedPathOnItsEdge() {
        const auto paths = selectionContour(400, 300, [](int x, int y) { return square(x, y, 100, 50, 200, 150); }, QRect(0, 0, 400, 300), 1);
        QCOMPARE(paths.size(), size_t(1));
        QVERIFY(closedPath(paths[0]));
        const QRectF box = bounds(paths[0]);
        QCOMPARE(box, QRectF(QPointF(100, 50), QPointF(200, 150))); // the pixels' outer edges
    }

    void aTriangleHasAClosedPathWithinItsBounds() {
        const auto tri = [](int x, int y) -> uint8_t { return x >= 50 && y >= 50 && (x - 50) <= (y - 50) && y < 150 ? 255 : 0; };
        const auto paths = selectionContour(400, 300, tri, QRect(0, 0, 400, 300), 1);
        QCOMPARE(paths.size(), size_t(1));
        QVERIFY(closedPath(paths[0]));
        const QRectF box = bounds(paths[0]);
        QVERIFY(qAbs(box.left() - 50) < 1 && qAbs(box.top() - 50) < 1 && qAbs(box.bottom() - 150) < 1 && qAbs(box.right() - 150) < 1);
    }

    void aHoleIsAnotherPathAndInverseKeepsTheEdgeCountsAndTheBorder() {
        const auto ring = [](int x, int y) { return uint8_t(square(x, y, 50, 50, 250, 250) && !square(x, y, 100, 100, 200, 200) ? 255 : 0); };
        const auto paths = selectionContour(400, 300, ring, QRect(0, 0, 400, 300), 1);
        QCOMPARE(paths.size(), size_t(2));
        QVERIFY(closedPath(paths[0]) && closedPath(paths[1]));
        // Inverse: the same two edges, and the picture's border is not an edge (outside the picture is not selected: it is cut there).
        const auto inverse = [&ring](int x, int y) { return uint8_t(255 - ring(x, y)); };
        const auto flipped = selectionContour(400, 300, inverse, QRect(0, 0, 400, 300), 1);
        QVERIFY(flipped.size() >= 3); // the ring's two and the picture's own border
        const auto none = selectionContour(400, 300, [](int, int) { return uint8_t(0); }, QRect(0, 0, 400, 300), 1);
        QVERIFY(none.empty());
    }

    void aFeatheredEdgeIsOutlinedAtItsMiddle() {
        const auto soft = [](int, int y) -> uint8_t { return y < 100 ? 255 : y >= 110 ? 0 : uint8_t(255 - (y - 100) * 25); };
        const auto paths = selectionContour(200, 200, soft, QRect(0, 0, 200, 200), 1);
        QVERIFY(!paths.empty());
        for (const ContourPath &p : paths)
            for (const QPointF &pt : p)
                if (pt.y() > 50 && pt.x() > 1 && pt.x() < 199)
                    QVERIFY(pt.y() > 104 && pt.y() < 107); // 127.5 sits near row 105
    }

    void aHugeCanvasStaysBounded() {
        // 16384 square, a lasso as an octagon-ish shape: only the window (a 1920 x 1080 view at 0.1) is sampled.
        const int size = 16384;
        const double zoom = 0.1;
        const int cell = contourCell(zoom);
        QCOMPARE(cell, 32);
        const auto shape = [](int x, int y) -> uint8_t { return x > 2000 && x < 14000 && y > 3000 && y < 13000 && (x + y) % 9000 < 8900 ? 255 : 0; };
        const QRect window(0, 0, int(1920 / zoom), int(1080 / zoom));
        const auto paths = selectionContour(size, size, shape, window, cell);
        int points = 0;
        for (const ContourPath &p : paths)
            points += int(p.size());
        QVERIFY(points > 0);
        QVERIFY2(points < 4 * (1920 + 1080) / 4 * 4, qPrintable(QString::number(points)));
    }

    void theCellFollowsTheZoom() {
        QCOMPARE(contourCell(2), 1);
        QCOMPARE(contourCell(1), 2);
                QCOMPARE(contourCell(0.1), 32);
        QCOMPARE(contourCell(0.0001), 1 << 14);
    }

    void theItemOutlinesARectangleAndAMaskAndFollowsTheMask() {
        MaskLayer layer;
        layer.setImageSize({400, 300});
        SelectionAnts ants;
        ants.setSize({400, 300});
        ants.setMask(&layer);
        ants.setZoom(1);
        QCoreApplication::processEvents();
        QVERIFY(!ants.outlined()); // an empty mask: nothing
        layer.fillRect(100, 100, 100, 50, false);
        QTRY_VERIFY(ants.outlined());
        QCOMPARE(ants.pathCount(), 1);
        QVERIFY(ants.closed(0));
        const QRectF box = ants.outlineBounds();
        QVERIFY(qAbs(box.left() - 100) <= 2 && qAbs(box.right() - 200) <= 2 && qAbs(box.top() - 100) <= 2 && qAbs(box.bottom() - 150) <= 2); // one 2 px cell of slack
        const int before = ants.pointCount();
        layer.invert(); // Inverse changes the outline: the same edge plus the picture's own
        QTRY_VERIFY(ants.pointCount() != before);
        layer.clear(); // Deselect
        QTRY_VERIFY(!ants.outlined());
        ants.setRect(QRectF(10, 20, 30, 40));
        QTRY_COMPARE(ants.pathCount(), 1);
        QCOMPARE(ants.outlineBounds(), QRectF(10, 20, 30, 40));
        ants.setRect(QRectF());
        QTRY_VERIFY(!ants.outlined());
    }
};

QTEST_MAIN(TestSelectionAnts)
#include "test_selection_ants.moc"
