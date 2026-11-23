// The canvas action router (#233): selection, prompt and crop frame in, the action and its region out.
// Plain numbers, no window and no store. The rules are in docs/inpaint-outpaint-research.md ("The router").
//
// Seam: engine/canvas_router.h.

#include <QtTest>

#include "engine/canvas_router.h"

using namespace fugue::canvas;

namespace {

RouteInput onCanvas(int width, int height) {
    RouteInput in;
    in.canvasWidth = width;
    in.canvasHeight = height;
    return in;
}

bool mentions(const Route &route, const char *word) {
    for (const std::string &reason : route.reasons)
        if (reason.find(word) != std::string::npos)
            return true;
    return false;
}

} // namespace

Q_DECLARE_METATYPE(RouteAction)

class TestCanvasRouter : public QObject {
    Q_OBJECT

private slots:
    // The action alone, over the whole table of inputs. Region and numbers have their own tests below.
    void theActionFollowsTheTable_data() {
        QTest::addColumn<int>("canvasWidth");
        QTest::addColumn<int>("canvasHeight");
        QTest::addColumn<bool>("selected");
        QTest::addColumn<int>("x");
        QTest::addColumn<int>("y");
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<double>("coverage");
        QTest::addColumn<bool>("prompt");
        QTest::addColumn<double>("strength"); // 0: not set
        QTest::addColumn<RouteAction>("action");

        // name                                          canvas      sel    x     y     w     h    cov    prompt strength action
        QTest::newRow("no canvas") << 0 << 0 << true << 0 << 0 << 10 << 10 << 1.0 << true << 0.0 << RouteAction::None;
        QTest::newRow("nothing selected, no prompt") << 1024 << 1024 << false << 0 << 0 << 0 << 0 << 1.0 << false << 0.0 << RouteAction::None;
        QTest::newRow("nothing selected, a prompt") << 1024 << 1024 << false << 0 << 0 << 0 << 0 << 1.0 << true << 0.0 << RouteAction::Refine;
        QTest::newRow("a rectangle and a prompt") << 1024 << 1024 << true << 500 << 230 << 300 << 340 << 1.0 << true << 0.0 << RouteAction::Fill;
        QTest::newRow("a rectangle, no prompt") << 1024 << 1024 << true << 500 << 230 << 300 << 340 << 1.0 << false << 0.0 << RouteAction::Remove;
        QTest::newRow("a lasso of half the picture and a prompt") << 1024 << 1024 << true << 0 << 0 << 1024 << 1024 << 0.65 << true << 0.0 << RouteAction::Fill;
        QTest::newRow("a selection of the edge, no prompt") << 1024 << 1024 << true << 0 << 396 << 323 << 628 << 0.7 << false << 0.0 << RouteAction::Remove;
        QTest::newRow("almost everything and a prompt") << 1024 << 1024 << true << 0 << 0 << 1024 << 1024 << 0.95 << true << 0.0 << RouteAction::Refine;
        QTest::newRow("almost everything, no prompt") << 1024 << 1024 << true << 0 << 0 << 1024 << 1024 << 0.95 << false << 0.0 << RouteAction::None;
        QTest::newRow("a strength below 1") << 1024 << 1024 << true << 408 << 544 << 442 << 476 << 0.6 << true << 0.4 << RouteAction::Refine;
        QTest::newRow("a strength below 1, no prompt") << 1024 << 1024 << true << 408 << 544 << 442 << 476 << 0.6 << false << 0.4 << RouteAction::Refine;
        QTest::newRow("a strength of 1 is the action's own") << 1024 << 1024 << true << 408 << 544 << 442 << 476 << 0.6 << true << 1.0 << RouteAction::Fill;
        QTest::newRow("a selection of a few pixels") << 1024 << 1024 << true << 10 << 10 << 4 << 4 << 1.0 << true << 0.0 << RouteAction::None;
        QTest::newRow("a lasso that selects almost nothing") << 1024 << 1024 << true << 10 << 10 << 100 << 100 << 0.001 << true << 0.0 << RouteAction::None;
        QTest::newRow("a selection past the left edge") << 1024 << 1024 << true << -200 << 100 << 400 << 300 << 1.0 << true << 0.0 << RouteAction::Expand;
        QTest::newRow("past the edge, no prompt") << 1024 << 1024 << true << 900 << 100 << 300 << 300 << 1.0 << false << 0.0 << RouteAction::Expand;
        QTest::newRow("touching the edge is not past it") << 1024 << 1024 << true << 724 << 0 << 300 << 300 << 1.0 << true << 0.0 << RouteAction::Fill;
        QTest::newRow("wholly outside the canvas") << 1024 << 1024 << true << 1100 << 0 << 100 << 100 << 1.0 << true << 0.0 << RouteAction::Expand;
    }

    void theActionFollowsTheTable() {
        QFETCH(int, canvasWidth);
        QFETCH(int, canvasHeight);
        QFETCH(bool, selected);
        QFETCH(int, x);
        QFETCH(int, y);
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(double, coverage);
        QFETCH(bool, prompt);
        QFETCH(double, strength);
        QFETCH(RouteAction, action);
        RouteInput in = onCanvas(canvasWidth, canvasHeight);
        if (selected)
            in.selection = RouteRect{x, y, width, height};
        in.coverage = coverage;
        in.hasPrompt = prompt;
        if (strength > 0)
            in.strength = strength;
        const Route route = routeAction(in);
        QCOMPARE(route.action, action);
        QVERIFY2(!route.reasons.empty(), "every route says why");
    }

    void theActionNamesAreTheWordsTheDocUses() {
        QCOMPARE(QString::fromLatin1(routeActionName(RouteAction::None)), QStringLiteral("none"));
        QCOMPARE(QString::fromLatin1(routeActionName(RouteAction::Fill)), QStringLiteral("fill"));
        QCOMPARE(QString::fromLatin1(routeActionName(RouteAction::Remove)), QStringLiteral("remove"));
        QCOMPARE(QString::fromLatin1(routeActionName(RouteAction::Expand)), QStringLiteral("expand"));
        QCOMPARE(QString::fromLatin1(routeActionName(RouteAction::Refine)), QStringLiteral("refine"));
    }

    void noSelectionSaysToSelectFirst() {
        const Route route = routeAction(onCanvas(1024, 1024));
        QVERIFY(mentions(route, "Select"));
    }

    void refineWithoutASelectionTakesTheWholeCanvas() {
        RouteInput in = onCanvas(1024, 800);
        in.hasPrompt = true;
        const Route route = routeAction(in);
        QCOMPARE(route.region, (RouteRect{0, 0, 1024, 800}));
        QCOMPARE(route.fill, (RouteRect{0, 0, 1024, 800}));
        QCOMPARE(route.strength, 0.7);
        QCOMPARE(route.padding, 0);
        QCOMPARE(route.scale, 1.0); // 0.82 MP is within 0.8 of the target: not scaled
        in.canvasHeight = 512;
        QVERIFY(routeAction(in).scale > 1.0); // 0.52 MP is scaled up
    }

    // Fill: padding is half the longer side, held to 64..256; the region is redrawn near a megapixel.
    void fillPadsByTheSelectionsSizeAndScalesUp() {
        RouteInput in = onCanvas(1024, 1024);
        in.selection = RouteRect{500, 230, 300, 340};
        in.hasPrompt = true;
        const Route route = routeAction(in);
        QCOMPARE(route.padding, 170);
        QCOMPARE(route.fill, (RouteRect{500, 230, 300, 340}));
        QCOMPARE(route.region, (RouteRect{330, 60, 640, 680}));
        QCOMPARE(route.strength, 1.0);
        QVERIFY(!route.edgeContact);
        QVERIFY(qAbs(route.scale - std::sqrt(1e6 / (640.0 * 680.0))) < 1e-9);
        QCOMPARE(route.canvasAfter, (RouteRect{0, 0, 1024, 1024}));
    }

    void aSmallSelectionGetsTheLeastPaddingAndTheLargestScale() {
        RouteInput in = onCanvas(1024, 1024);
        in.selection = RouteRect{400, 400, 100, 100};
        in.hasPrompt = true;
        const Route route = routeAction(in);
        QCOMPARE(route.padding, 64);
        QCOMPARE(route.region, (RouteRect{336, 336, 228, 228}));
        QCOMPARE(route.scale, 4.0); // 4.39 wanted, the Region's limit is 4
    }

    void aLargeSelectionGetsTheMostPadding() {
        RouteInput in = onCanvas(2048, 2048);
        in.selection = RouteRect{600, 600, 800, 700};
        in.hasPrompt = true;
        const Route route = routeAction(in);
        QCOMPARE(route.padding, 256);
        QCOMPARE(route.scale, 1.0); // 1312 x 1212 is above a megapixel: the Region's cap brings it down
    }

    void removeKeepsMoreSurroundings() {
        RouteInput in = onCanvas(1024, 1024);
        in.selection = RouteRect{532, 248, 157, 317}; // a floor lamp
        const Route route = routeAction(in);
        QCOMPARE(route.action, RouteAction::Remove);
        QCOMPARE(route.padding, 158); // half of 317, above Remove's 128
        in.selection = RouteRect{500, 500, 60, 80};
        QCOMPARE(routeAction(in).padding, 128); // Remove's floor, not Fill's 64
    }

    // A selection on the canvas's edge: the region is moved inside, keeping its size, as Crop to Mask does.
    void anEdgeSelectionMovesTheRegionInside() {
        RouteInput in = onCanvas(1024, 1024);
        in.selection = RouteRect{0, 396, 323, 628}; // the plant on the left and bottom edges
        in.coverage = 0.7;
        const Route route = routeAction(in);
        QVERIFY(route.edgeContact);
        QCOMPARE(route.padding, 256);
        // 323 + 512 wide from x 0; 628 + 512 = 1140 tall is more than the canvas, so all of its height.
        QCOMPARE(route.region, (RouteRect{0, 0, 835, 1024}));
        QVERIFY(mentions(route, "edge"));
    }

    void aSelectionInTheCornerKeepsItsRegionSize() {
        RouteInput in = onCanvas(1024, 1024);
        in.selection = RouteRect{924, 924, 100, 100};
        in.hasPrompt = true;
        const Route route = routeAction(in);
        QVERIFY(route.edgeContact);
        QCOMPARE(route.region, (RouteRect{796, 796, 228, 228}));
    }

    void refineUsesTheStrengthGiven() {
        RouteInput in = onCanvas(1024, 1024);
        in.selection = RouteRect{408, 544, 442, 476};
        in.hasPrompt = true;
        in.strength = 0.35;
        QCOMPARE(routeAction(in).strength, 0.35);
        in.strength.reset();
        in.coverage = 0.95;
        in.selection = RouteRect{0, 0, 1024, 1024};
        QCOMPARE(routeAction(in).strength, 0.7); // the router's own default for a near-whole selection
    }

    // Expand from the crop tool: the canvas grows, the new border plus an overlap is the fill.
    void expandLeftAndRight() {
        RouteInput in = onCanvas(640, 1024);
        in.resize = RouteRect{-192, 0, 1024, 1024};
        in.hasPrompt = true;
        const Route route = routeAction(in);
        QCOMPARE(route.action, RouteAction::Expand);
        QCOMPARE(route.canvasAfter, (RouteRect{-192, 0, 1024, 1024}));
        QCOMPARE(route.overlap, 32);
        // Two sides: the fill's bounds run across the new canvas, and so does the region.
        QCOMPARE(route.fill, (RouteRect{0, 0, 1024, 1024}));
        QCOMPARE(route.region, (RouteRect{0, 0, 1024, 1024}));
        QCOMPARE(route.passes, 1);
        QCOMPARE(route.strength, 1.0);
    }

    void expandUpKeepsAContextBandOfTheOldPicture() {
        RouteInput in = onCanvas(768, 720);
        in.resize = RouteRect{0, -304, 768, 1024};
        in.hasPrompt = true;
        const Route route = routeAction(in);
        QCOMPARE(route.action, RouteAction::Expand);
        QCOMPARE(route.fill, (RouteRect{0, 0, 768, 336}));     // 304 new rows and 32 of overlap
        QCOMPARE(route.padding, 256);                          // the context margin
        QCOMPARE(route.region, (RouteRect{0, 0, 768, 592}));   // the fill and 256 rows of the old picture
        QVERIFY(qAbs(route.scale - std::sqrt(1e6 / (768.0 * 592.0))) < 1e-9);
    }

    void aSelectionPastTheEdgeGrowsTheCanvasToHoldIt() {
        RouteInput in = onCanvas(1024, 1024);
        in.selection = RouteRect{-200, 100, 400, 300};
        in.hasPrompt = true;
        const Route route = routeAction(in);
        QCOMPARE(route.canvasAfter, (RouteRect{-200, 0, 1224, 1024}));
        QCOMPARE(route.fill, (RouteRect{0, 0, 232, 1024})); // the whole new strip, or it would stay empty
        QCOMPARE(route.region, (RouteRect{0, 0, 488, 1024}));
        QVERIFY(mentions(route, "past"));
    }

    void expandWithoutAPromptSaysWhatItWillUse() {
        RouteInput in = onCanvas(1024, 1024);
        in.resize = RouteRect{0, 0, 1280, 1024};
        const Route route = routeAction(in);
        QCOMPARE(route.action, RouteAction::Expand);
        QVERIFY(mentions(route, "prompt"));
    }

    void aLargeExpandIsSplitIntoPasses() {
        RouteInput in = onCanvas(512, 512);
        in.resize = RouteRect{-600, 0, 1112, 512};
        in.hasPrompt = true;
        const Route route = routeAction(in);
        QCOMPARE(route.passes, 3); // 600 px on a 512 px side: 1.17 of it, at most half of it per pass
    }

    void aCropFrameInsideTheCanvasIsNotGenerative() {
        RouteInput in = onCanvas(1024, 1024);
        in.resize = RouteRect{100, 100, 500, 500};
        in.hasPrompt = true;
        const Route route = routeAction(in);
        QCOMPARE(route.action, RouteAction::None);
        QVERIFY(mentions(route, "crop"));
    }

    void theCropFrameWinsOverTheSelection() {
        RouteInput in = onCanvas(1024, 1024);
        in.selection = RouteRect{100, 100, 100, 100};
        in.resize = RouteRect{0, 0, 1024, 1280};
        in.hasPrompt = true;
        const Route route = routeAction(in);
        QCOMPARE(route.action, RouteAction::Expand);
        QCOMPARE(route.fill, (RouteRect{0, 992, 1024, 288}));
    }

    void aGrowAndCropFrameExpandsOnlyWhereItGrows() {
        RouteInput in = onCanvas(1024, 1024);
        in.resize = RouteRect{100, 0, 1200, 1024}; // 100 cut on the left, 276 added on the right
        in.hasPrompt = true;
        const Route route = routeAction(in);
        QCOMPARE(route.action, RouteAction::Expand);
        QCOMPARE(route.canvasAfter, (RouteRect{100, 0, 1200, 1024}));
        QCOMPARE(route.fill, (RouteRect{892, 0, 308, 1024})); // x 924 - 32 in the new canvas's pixels
    }

    void tuningChangesTheNumbers() {
        RouteInput in = onCanvas(1024, 1024);
        in.selection = RouteRect{400, 400, 100, 100};
        in.hasPrompt = true;
        RouteTuning tuning;
        tuning.minPadding = 32;
        tuning.maxScale = 2.0;
        const Route route = routeAction(in, tuning);
        QCOMPARE(route.padding, 50);
        QCOMPARE(route.scale, 2.0);
    }
};

QTEST_GUILESS_MAIN(TestCanvasRouter)
#include "test_canvas_router.moc"
