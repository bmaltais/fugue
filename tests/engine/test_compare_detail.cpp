// Which picture a Compare Images node's face draws, the 512 px copy or the file (#371). Plain numbers, no window.
//
// Seam: engine/compare_detail.h.

#include <QtTest>

#include "engine/compare_detail.h"

using namespace fugue::compare;

class TestCompareDetail : public QObject {
    Q_OBJECT

private slots:
    void aCopyPixelOverOneScreenPixelTakesTheFile() {
        // A 1024 px file has a 512 px copy: one copy pixel is two file pixels, so it covers 2 x scale screen pixels.
        QVERIFY(!useFile(0.5, copyWidthOf(1024), 1024, false, false)); // covers 1.0: the copy is still sharp
        QVERIFY(useFile(0.6, copyWidthOf(1024), 1024, false, false));
        QVERIFY(useFile(8, copyWidthOf(4096), 4096, false, false));
    }

    void hysteresisKeepsTheChoiceBetweenTheThresholds() {
        // 4096 px: covers = scale * 8, taken above 1.1 (scale 0.1375), let go below 0.9 (scale 0.1125).
        QVERIFY(!useFile(0.125, 512, 4096, false, false)); // covers 1.0, was the copy: stays the copy
        QVERIFY(useFile(0.125, 512, 4096, false, true));   // covers 1.0, was the file: stays the file
        QVERIFY(!useFile(0.1, 512, 4096, false, true));    // covers 0.8: back to the copy
    }

    void fitAndSmallFilesAlwaysDrawTheCopy() {
        QVERIFY(!useFile(8, 512, 4096, true, true));  // at fit, however large the node is
        QVERIFY(!useFile(8, copyWidthOf(400), 400, false, true)); // the file is its own copy
        QVERIFY(!useFile(8, 0, 0, false, false));     // no size yet
    }
};

QTEST_GUILESS_MAIN(TestCompareDetail)
#include "test_compare_detail.moc"
