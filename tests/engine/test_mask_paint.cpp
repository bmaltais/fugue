// The mask editor's core (#172, spec 14.4): an 8-bit mask painted with dabs, a lasso and a rectangle,
// with undo by tiles and a PNG round trip. Plain numbers, no window: the editor draws what this holds.
//
// Seam: engine/mask_paint.h.

#include <QtTest>

#include <cmath>

#include "engine/mask_paint.h"
#include "engine/vips.h"

using namespace fugue::mask;

namespace {

Brush brush(double size, double hardness = 0.8, double opacity = 1) { return {size, hardness, opacity, {}}; }

int sum(const MaskPaint &m) {
    int total = 0;
    for (uint8_t v : m.pixels())
        total += v;
    return total;
}

// One stroke along the points.
void stroke(MaskPaint &m, Mode mode, const Brush &b, const std::vector<Point> &points) {
    m.beginStroke(mode, b);
    for (const Point &p : points)
        m.strokeTo(p);
    m.endStroke();
}

} // namespace

class TestMaskPaint : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(fugue::initVips()); }

    // ---- dabs ----------------------------------------------------------------------------------

    void aSoftDabFallsOffFromTheCenterToTheRadius() {
        QCOMPARE(dabCoverage(0, 10, 0), 1.0);
        QCOMPARE(dabCoverage(10, 10, 0), 0.0);
        QCOMPARE(dabCoverage(12, 10, 0), 0.0);
        QCOMPARE(dabCoverage(5, 10, 0), 0.5); // smoothstep is 0.5 at the middle
        double last = 1;
        for (double d = 0; d <= 10; d += 0.5) {
            const double c = dabCoverage(d, 10, 0);
            QVERIFY(c <= last + 1e-12);
            last = c;
        }
        // Hardness 0.5: whole inside half the radius, then the fall.
        QCOMPARE(dabCoverage(5, 10, 0.5), 1.0);
        QCOMPARE(dabCoverage(7.5, 10, 0.5), 0.5);
        QCOMPARE(dabCoverage(10, 10, 0.5), 0.0);
    }

    void aHardDabIsWholeInsideAndAntialiasedAtTheEdge() {
        QCOMPARE(dabCoverage(9, 10, 1), 1.0);
        QCOMPARE(dabCoverage(10, 10, 1), 0.5);
        QCOMPARE(dabCoverage(11, 10, 1), 0.0);
        QVERIFY(dabCoverage(10.25, 10, 1) < 0.5 && dabCoverage(10.25, 10, 1) > 0);
    }

    void aSingleDabPaintsAroundThePoint() {
        MaskPaint m(100, 100);
        m.beginStroke(Mode::Paint, brush(40, 1));
        m.strokeTo({50, 50});
        m.endStroke();
        QCOMPARE(m.at(50, 50), uint8_t(255));
        QCOMPARE(m.at(40, 50), uint8_t(255)); // 10 px from the center
        QCOMPARE(m.at(75, 50), uint8_t(0));   // 25 px
        const int edge = m.at(69, 50);        // pixel center 19.5 from the center: inside by half a pixel
        QVERIFY(edge > 0 && edge <= 255);
        // The area is about pi r^2 pixels.
        QVERIFY(std::abs(sum(m) / 255.0 - M_PI * 400) < 12);
    }

    void aSoftDabIsWeakerAtTheEdgeThanAHardOne() {
        MaskPaint soft(100, 100), hard(100, 100);
        soft.beginStroke(Mode::Paint, brush(60, 0));
        soft.strokeTo({50, 50});
        soft.endStroke();
        hard.beginStroke(Mode::Paint, brush(60, 1));
        hard.strokeTo({50, 50});
        hard.endStroke();
        QCOMPARE(soft.at(50, 50), uint8_t(255));
        QVERIFY(soft.at(65, 50) > 100 && soft.at(65, 50) < 155); // halfway out, about half
        QCOMPARE(hard.at(65, 50), uint8_t(255));
        QVERIFY(sum(soft) < sum(hard));
        // Falls monotonically outward.
        for (int x = 50; x < 79; ++x)
            QVERIFY(soft.at(x + 1, 50) <= soft.at(x, 50));
    }

    void opacityScalesWhatAStrokeSets() {
        MaskPaint m(40, 40);
        m.beginStroke(Mode::Paint, brush(20, 1, 0.5));
        m.strokeTo({20, 20});
        m.endStroke();
        QCOMPARE(m.at(20, 20), uint8_t(128));
    }

    void aDabNearTheEdgeStaysInsideThePicture() {
        MaskPaint m(30, 30);
        stroke(m, Mode::Paint, brush(40), {{-5, -5}, {35, 35}});
        QCOMPARE(m.at(0, 0), uint8_t(255));
        QCOMPARE(m.at(29, 29), uint8_t(255));
        stroke(m, Mode::Paint, brush(10), {{500, 500}}); // far outside: nothing, no crash
    }

    // ---- strokes -------------------------------------------------------------------------------

    void dabsLandEveryTenthOfTheSizeAlongAStroke() {
        // Size 40 -> every 4 px, so a 40 px line leaves a smooth band, and with hardness 1 it is
        // whole at the middle line.
        MaskPaint m(160, 60);
        stroke(m, Mode::Paint, brush(40, 1), {{20, 30}, {100, 30}});
        for (int x = 20; x <= 100; ++x)
            QCOMPARE(m.at(x, 30), uint8_t(255));
        QCOMPARE(m.at(10, 30), uint8_t(255)); // the cap of the first dab
        QCOMPARE(m.at(125, 30), uint8_t(0));
        // The band is as tall as the brush all along.
        QCOMPARE(m.at(60, 12), uint8_t(255));
        QCOMPARE(m.at(60, 48), uint8_t(255));
        QCOMPARE(m.at(60, 6), uint8_t(0));
    }

    void theSpacingCarriesOverFromOneMoveToTheNext() {
        // The same line in one move or in many small ones is the same mask.
        MaskPaint one(200, 60), many(200, 60);
        stroke(one, Mode::Paint, brush(30, 0.3), {{20, 30}, {180, 30}});
        std::vector<Point> points;
        for (int x = 20; x <= 180; x += 3)
            points.push_back({double(x), 30});
        stroke(many, Mode::Paint, brush(30, 0.3), points);
        QCOMPARE(one.pixels(), many.pixels());
    }

    void aSmallBrushSpacesDabsAtOnePixelAtLeast() {
        MaskPaint m(50, 10);
        stroke(m, Mode::Paint, brush(2, 1), {{5, 5}, {45, 5}});
        for (int x = 5; x <= 45; ++x)
            QVERIFY2(m.at(x, 5) > 200, qPrintable(QString::number(x)));
    }

    void aStrokeNeverBuildsUpOverItself() {
        // A stroke that crosses itself many times is never stronger than one pass of the same dab:
        // paint is max, not add.
        MaskPaint m(80, 80);
        std::vector<Point> path;
        for (int i = 0; i < 60; ++i)
            path.push_back({40 + 10 * std::cos(i * 0.5), 40 + 10 * std::sin(i * 0.5)});
        stroke(m, Mode::Paint, brush(30, 0), path);
        QVERIFY(m.at(40, 40) > 0 && m.at(40, 40) < 255); // dabs 10 px away, none on the spot
        // Fifty dabs on one spot are the one dab: a soft rim does not darken with each.
        MaskPaint many(80, 80), single(80, 80);
        std::vector<Point> spot(50, Point{40.5, 40.5});
        stroke(many, Mode::Paint, brush(30, 0), spot);
        stroke(single, Mode::Paint, brush(30, 0), {{40.5, 40.5}});
        QCOMPARE(many.pixels(), single.pixels());
    }

    void paintNeverLowersAndEraseNeverRaises() {
        MaskPaint m(100, 100);
        stroke(m, Mode::Paint, brush(50, 1), {{50, 50}});
        const std::vector<uint8_t> before = m.pixels();
        stroke(m, Mode::Paint, brush(40, 0), {{30, 50}, {70, 50}});
        for (size_t i = 0; i < before.size(); ++i)
            QVERIFY(m.pixels()[i] >= before[i]);
        const std::vector<uint8_t> painted = m.pixels();
        stroke(m, Mode::Erase, brush(30, 0.5), {{20, 50}, {80, 50}});
        for (size_t i = 0; i < painted.size(); ++i)
            QVERIFY(m.pixels()[i] <= painted[i]);
    }

    void eraseClearsWhatItCoversAndKeepsTheRest() {
        MaskPaint m(100, 100);
        m.fill();
        stroke(m, Mode::Erase, brush(30, 1), {{50, 50}});
        QCOMPARE(m.at(50, 50), uint8_t(0));
        QCOMPARE(m.at(40, 50), uint8_t(0));
        QCOMPARE(m.at(80, 50), uint8_t(255));
        // A soft erase leaves the soft rim.
        MaskPaint soft(100, 100);
        soft.fill();
        stroke(soft, Mode::Erase, brush(40, 0), {{50.5, 50.5}});
        QCOMPARE(soft.at(50, 50), uint8_t(0));
        QVERIFY(soft.at(65, 50) > 0 && soft.at(65, 50) < 255);
    }

    void pressureScalesTheSize() {
        MaskPaint full(100, 100), half(100, 100);
        full.beginStroke(Mode::Paint, brush(40, 1));
        full.strokeTo({50, 50}, 1);
        full.endStroke();
        half.beginStroke(Mode::Paint, brush(40, 1));
        half.strokeTo({50, 50}, 0.5);
        half.endStroke();
        QVERIFY(std::abs(sum(half) / double(sum(full)) - 0.25) < 0.03); // the area is the size squared
    }

    // ---- shapes --------------------------------------------------------------------------------

    void aRectangleFillsWholePixelsAndAntialiasesFractionalEdges() {
        MaskPaint m(40, 40);
        m.fillRect(10, 10, 20, 10);
        QCOMPARE(m.at(10, 10), uint8_t(255));
        QCOMPARE(m.at(29, 19), uint8_t(255));
        QCOMPARE(m.at(9, 10), uint8_t(0));
        QCOMPARE(m.at(30, 10), uint8_t(0));
        QCOMPARE(m.at(10, 20), uint8_t(0));
        MaskPaint f(40, 40);
        f.fillRect(10.5, 10.25, 10, 10); // left edge half a pixel, top a quarter
        QCOMPARE(f.at(10, 15), uint8_t(128));
        QCOMPARE(f.at(15, 10), uint8_t(191)); // 0.75 of 255
        QCOMPARE(f.at(10, 10), uint8_t(96)); // 0.5 x 0.75 = 0.375
        QCOMPARE(f.at(15, 15), uint8_t(255));
        // Dragged the other way round it is the same rectangle.
        MaskPaint r(40, 40);
        r.fillRect(30, 20, -20, -10);
        QCOMPARE(r.pixels(), m.pixels());
    }

    void aRectangleCutByThePictureAndAnEraseRectangle() {
        MaskPaint m(20, 20);
        m.fillRect(-10, -10, 20, 20);
        QCOMPARE(m.at(0, 0), uint8_t(255));
        QCOMPARE(m.at(9, 9), uint8_t(255));
        QCOMPARE(m.at(10, 10), uint8_t(0));
        m.fillRect(5, 5, 100, 100, Mode::Erase);
        QCOMPARE(m.at(4, 4), uint8_t(255));
        QCOMPARE(m.at(5, 5), uint8_t(0));
        m.fillRect(100, 100, 5, 5); // outside: nothing and no step
        const size_t steps = m.undoSteps();
        m.fillRect(100, 100, 5, 5);
        QCOMPARE(m.undoSteps(), steps);
    }

    void aLassoFillsItsInsideWithAntialiasedEdges() {
        MaskPaint m(60, 60);
        m.fillPolygon({{10, 10}, {40, 10}, {40, 40}, {10, 40}});
        QCOMPARE(m.at(10, 10), uint8_t(255));
        QCOMPARE(m.at(39, 39), uint8_t(255));
        QCOMPARE(m.at(40, 20), uint8_t(0));
        QCOMPARE(m.at(9, 20), uint8_t(0));
        // A triangle's diagonal edge is soft: some pixels are partial, and the area is close to exact.
        MaskPaint t(60, 60);
        t.fillPolygon({{0, 0}, {40, 0}, {0, 40}});
        bool partial = false;
        for (uint8_t v : t.pixels())
            partial = partial || (v > 0 && v < 255);
        QVERIFY(partial);
        QVERIFY(std::abs(sum(t) / 255.0 - 800) < 6);
    }

    void aSelfCrossingLassoFillsEveryLoop() {
        // A figure eight: the two lobes are wound opposite ways, and both are filled.
        MaskPaint m(60, 30);
        m.fillPolygon({{5, 5}, {25, 25}, {25, 5}, {5, 25}}); // a bow tie, the sides crossing at (15, 15)
        QCOMPARE(m.at(24, 15), uint8_t(255)); // the right lobe
        QCOMPARE(m.at(6, 15), uint8_t(255));  // the left lobe
        QCOMPARE(m.at(15, 8), uint8_t(0));    // above the crossing, between the sides
        QCOMPARE(m.at(15, 22), uint8_t(0));
        // A loop drawn twice over itself (winding 2) is still filled, not a hole.
        MaskPaint twice(40, 40);
        twice.fillPolygon({{5, 5}, {35, 5}, {35, 35}, {5, 35}, {5, 5}, {35, 5}, {35, 35}, {5, 35}});
        QCOMPARE(twice.at(20, 20), uint8_t(255));
        // A pentagram's middle is filled too.
        std::vector<Point> star;
        for (int i = 0; i < 5; ++i) {
            const double a = -M_PI / 2 + i * 4 * M_PI / 5;
            star.push_back({30 + 25 * std::cos(a), 30 + 25 * std::sin(a)});
        }
        MaskPaint s(60, 60);
        s.fillPolygon(star);
        QCOMPARE(s.at(30, 30), uint8_t(255));
    }

    void aLassoWithAltErasesAndDegenerateOnesDoNothing() {
        MaskPaint m(40, 40);
        m.fill();
        m.fillPolygon({{10, 10}, {30, 10}, {30, 30}, {10, 30}}, Mode::Erase);
        QCOMPARE(m.at(20, 20), uint8_t(0));
        QCOMPARE(m.at(5, 5), uint8_t(255));
        const size_t steps = m.undoSteps();
        m.fillPolygon({{1, 1}, {5, 5}}); // two points: no area
        m.fillPolygon({{1, 1}, {5, 5}, {9, 9}}); // collinear: no area
        QCOMPARE(m.undoSteps(), steps);
    }

    // ---- invert, clear, fill -------------------------------------------------------------------

    void invertClearAndFill() {
        MaskPaint m(30, 20);
        QVERIFY(m.isEmpty());
        m.fillRect(0, 0, 10, 20);
        m.invert();
        QCOMPARE(m.at(0, 0), uint8_t(0));
        QCOMPARE(m.at(10, 0), uint8_t(255));
        QVERIFY(!m.isEmpty());
        m.invert();
        QCOMPARE(m.at(0, 0), uint8_t(255));
        m.clear();
        QVERIFY(m.isEmpty());
        m.fill();
        QCOMPARE(sum(m), 255 * 30 * 20);
        // Inverting keeps the soft values: 255 - v.
        MaskPaint soft(50, 50);
        stroke(soft, Mode::Paint, brush(30, 0), {{25, 25}});
        const std::vector<uint8_t> before = soft.pixels();
        soft.invert();
        for (size_t i = 0; i < before.size(); ++i)
            QCOMPARE(int(soft.pixels()[i]), 255 - before[i]);
    }

    void aWholeMaskStepThatChangesNothingIsNoStep() {
        MaskPaint m(30, 20);
        m.clear(); // already empty
        QCOMPARE(m.undoSteps(), size_t(0));
        m.fill();
        m.fill();
        QCOMPARE(m.undoSteps(), size_t(1));
    }

    // ---- undo and redo -------------------------------------------------------------------------

    void undoAndRedoGoStrokeByStroke() {
        MaskPaint m(100, 100);
        stroke(m, Mode::Paint, brush(20, 1), {{20, 20}});
        const std::vector<uint8_t> one = m.pixels();
        stroke(m, Mode::Paint, brush(20, 1), {{70, 70}});
        const std::vector<uint8_t> two = m.pixels();
        QVERIFY(m.canUndo());
        QVERIFY(!m.canRedo());
        m.undo();
        QCOMPARE(m.pixels(), one);
        QVERIFY(m.canRedo());
        m.undo();
        QVERIFY(m.isEmpty());
        QVERIFY(!m.canUndo());
        m.redo();
        QCOMPARE(m.pixels(), one);
        m.redo();
        QCOMPARE(m.pixels(), two);
        QVERIFY(!m.canRedo());
        // A new change drops the redo steps.
        m.undo();
        stroke(m, Mode::Paint, brush(20, 1), {{50, 20}});
        QVERIFY(!m.canRedo());
    }

    void undoCoversEveryKindOfStep() {
        MaskPaint m(70, 70);
        std::vector<std::vector<uint8_t>> states{m.pixels()};
        stroke(m, Mode::Paint, brush(20, 0.5), {{20, 20}, {50, 25}});
        states.push_back(m.pixels());
        m.fillRect(5, 40, 30, 20);
        states.push_back(m.pixels());
        m.fillPolygon({{40, 40}, {65, 45}, {50, 65}}, Mode::Paint, 0.7);
        states.push_back(m.pixels());
        m.invert();
        states.push_back(m.pixels());
        stroke(m, Mode::Erase, brush(30, 0.2), {{30, 30}});
        states.push_back(m.pixels());
        m.clear();
        states.push_back(m.pixels());
        m.fill();
        states.push_back(m.pixels());
        for (size_t i = states.size() - 1; i > 0; --i) {
            QCOMPARE(m.pixels(), states[i]);
            m.undo();
        }
        QCOMPARE(m.pixels(), states[0]);
        for (size_t i = 1; i < states.size(); ++i) {
            m.redo();
            QCOMPARE(m.pixels(), states[i]);
        }
    }

    void undoKeepsAtLeastAHundredAndFiftyStepsAndOnlyTheTouchedTiles() {
        // 150 small strokes on a large picture: all can be undone, and memory is the tiles touched,
        // a few of 64 x 64 each, not 150 pictures.
        MaskPaint m(4000, 3000);
        std::vector<std::vector<uint8_t>> sums;
        for (int i = 0; i < 150; ++i)
            stroke(m, Mode::Paint, brush(20, 0.6), {{double(100 + 20 * i), double(200 + 7 * i)}, {double(110 + 20 * i), double(210 + 7 * i)}});
        QCOMPARE(m.undoSteps(), size_t(150));
        const size_t tileBytes = size_t(MaskPaint::tileSize) * MaskPaint::tileSize;
        QVERIFY(m.undoBytes() <= 150 * 4 * tileBytes); // a stroke of 20 px touches at most four tiles
        QVERIFY(m.undoBytes() < size_t(4000) * 3000 / 10);
        const int total = sum(m);
        QVERIFY(total > 0);
        for (int i = 0; i < 150; ++i)
            m.undo();
        QVERIFY(m.isEmpty());
        QCOMPARE(m.undoBytes(), size_t(0));
        QCOMPARE(m.redoSteps(), size_t(150));
        for (int i = 0; i < 150; ++i)
            m.redo();
        QCOMPARE(sum(m), total);
        // Undone and redone are bytes the other way round, not more: the total stays.
        QCOMPARE(m.redoBytes(), size_t(0));
    }

    void theOldestStepsGoPastTheLimitButAHundredStay() {
        MaskPaint m(64, 64);
        for (int i = 0; i < 400; ++i)
            m.fillRect(i % 64, i / 64, 1, 1); // 400 different pixels, 400 steps
        QVERIFY(m.undoSteps() >= MaskPaint::minimumSteps);
        QVERIFY(m.undoSteps() <= MaskPaint::maximumSteps);
        int undone = 0;
        while (m.canUndo()) {
            m.undo();
            ++undone;
        }
        QVERIFY(undone >= 100);
    }

    void aStepKeepsOnlyTheTilesItChanged() {
        MaskPaint m(256, 256);
        m.fillRect(70, 70, 10, 10); // inside one tile
        QCOMPARE(m.undoBytes(), size_t(64 * 64));
        m.fillRect(60, 60, 10, 10); // across four
        QCOMPARE(m.undoBytes(), size_t(64 * 64 * 5));
        // Edge tiles are as small as the picture's edge.
        MaskPaint e(70, 70);
        e.invert();
        QCOMPARE(e.undoBytes(), size_t(70 * 70));
    }

    void aStrokeInProgressCannotBeUndone() {
        MaskPaint m(50, 50);
        stroke(m, Mode::Paint, brush(10), {{10, 10}});
        m.beginStroke(Mode::Paint, brush(10));
        m.strokeTo({30, 30});
        QVERIFY(!m.canUndo());
        m.undo(); // ignored
        QVERIFY(m.at(30, 30) > 0);
        m.endStroke();
        QCOMPARE(m.undoSteps(), size_t(2));
    }

    // ---- the dirty rectangle ---------------------------------------------------------------------

    void theDirtyRectangleIsWhatChangedSinceItWasTaken() {
        MaskPaint m(200, 200);
        QVERIFY(m.takeDirty().empty());
        m.fillRect(10, 10, 20, 20);
        QCOMPARE(m.takeDirty(), (IRect{10, 10, 20, 20}));
        QVERIFY(m.takeDirty().empty());
        m.beginStroke(Mode::Paint, brush(10, 1));
        m.strokeTo({100, 100});
        const IRect dab = m.takeDirty();
        QVERIFY(!dab.empty());
        QVERIFY(dab.width < 20 && dab.height < 20);
        m.endStroke();
        m.undo();
        QVERIFY(!m.takeDirty().empty()); // an undo says what it changed too
        m.invert();
        QCOMPARE(m.takeDirty(), (IRect{0, 0, 200, 200}));
    }

    // ---- PNG -------------------------------------------------------------------------------------

    void aMaskRoundTripsThroughAnEightBitGrayPng() {
        MaskPaint m(97, 61);
        stroke(m, Mode::Paint, brush(30, 0.3), {{20, 20}, {70, 40}});
        m.fillRect(3, 3, 10, 10);
        const fugue::Result<QByteArray> png = m.toPng();
        QVERIFY2(png.ok(), qPrintable(png.error().message));
        // The header: 97 x 61, bit depth 8, color type 0 (gray).
        QVERIFY(png->startsWith("\x89PNG\r\n\x1a\n"));
        QCOMPARE(png->mid(12, 4), QByteArray("IHDR"));
        QCOMPARE(int(uchar(png->at(19))), 97);
        QCOMPARE(int(uchar(png->at(23))), 61);
        QCOMPARE(int(uchar(png->at(24))), 8);
        QCOMPARE(int(uchar(png->at(25))), 0);
        const fugue::Result<MaskPaint> back = MaskPaint::fromPng(*png);
        QVERIFY2(back.ok(), qPrintable(back.error().message));
        QCOMPARE(back->width(), 97);
        QCOMPARE(back->height(), 61);
        QCOMPARE(back->pixels(), m.pixels());
    }

    void theSameMaskIsTheSameBytes() {
        MaskPaint a(50, 40), b(50, 40);
        a.fillRect(5, 5, 20, 20);
        stroke(b, Mode::Paint, brush(8), {{1, 1}});
        b.undo(); // the history differs, the pixels do not
        b.fillRect(5, 5, 20, 20);
        QCOMPARE(*a.toPng(), *b.toPng());
        QVERIFY(a.toPng()->startsWith("\x89PNG"));
        // No metadata chunks: a gray PNG's header, its data and the end.
        QVERIFY(!a.toPng()->contains("tEXt"));
        QVERIFY(!a.toPng()->contains("tIME"));
    }

    void anEmptyMaskIsAllZeroAfterTheTrip() {
        MaskPaint m(33, 17);
        const fugue::Result<MaskPaint> back = MaskPaint::fromPng(*m.toPng());
        QVERIFY(back.ok());
        QVERIFY(back->isEmpty());
    }

    void otherPicturesReadAsTheirFirstChannelAndNonsenseIsAnError() {
        std::vector<uint8_t> rgb;
        for (int i = 0; i < 8 * 4; ++i)
            rgb.insert(rgb.end(), {200, 10, 20});
        QByteArray bytes;
        {
            void *buffer = nullptr;
            size_t size = 0;
            vips::VImage::new_from_memory(rgb.data(), rgb.size(), 8, 4, 3, VIPS_FORMAT_UCHAR).write_to_buffer(".png", &buffer, &size);
            bytes = QByteArray(static_cast<const char *>(buffer), qsizetype(size));
            g_free(buffer);
        }
        const fugue::Result<MaskPaint> m = MaskPaint::fromPng(bytes);
        QVERIFY(m.ok());
        QCOMPARE(m->at(3, 2), uint8_t(200)); // the red channel
        QVERIFY(!MaskPaint::fromPng("not a picture").ok());
        QVERIFY(!MaskPaint::fromPng({}).ok());
    }

    // ---- scaling ---------------------------------------------------------------------------------

    void scalingIsBilinearAndKeepsTheSizeAsItIs() {
        MaskPaint m(4, 2);
        m.fillRect(0, 0, 2, 2); // left half white
        const MaskPaint same = m.scaled(4, 2);
        QCOMPARE(same.pixels(), m.pixels());
        const MaskPaint up = m.scaled(8, 4);
        QCOMPARE(up.width(), 8);
        QCOMPARE(up.height(), 4);
        QCOMPARE(up.at(0, 0), uint8_t(255));
        QCOMPARE(up.at(7, 3), uint8_t(0));
        // Across the edge the values step down through grays, not at once.
        const int a = up.at(3, 1), b = up.at(4, 1);
        QVERIFY(a < 255 && a > b && b > 0);
        QCOMPARE(a + b, 255);
        // Down: a flat mask stays flat.
        MaskPaint flat(40, 40);
        flat.fill();
        const MaskPaint small = flat.scaled(7, 9);
        for (uint8_t v : small.pixels())
            QCOMPARE(v, uint8_t(255));
        // One pixel wide is fine.
        QCOMPARE(m.scaled(1, 1).pixels().size(), size_t(1));
    }
};

QTEST_GUILESS_MAIN(TestMaskPaint)
#include "test_mask_paint.moc"
