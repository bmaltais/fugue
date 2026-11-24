// Which picture a Compare Images node's face draws, the 512 px copy or the file (#371). Plain numbers, no window.
//
// Seam: engine/compare_detail.h.

#include <QtTest>

#include "engine/compare_detail.h"
#include "engine/compare_scale.h"

using namespace fugue::compare;

class TestCompareDetail : public QObject {
    Q_OBJECT

private slots:
    void aCopyPixelOverOneScreenPixelTakesTheFile() {
        // A 1024 px file has a 512 px copy: one copy pixel is two file pixels, so it covers 2 x scale screen pixels.
        QVERIFY(!useFile(0.5, copyWidthOf(1024), 1024, false)); // covers 1.0: the copy is still sharp
        QVERIFY(useFile(0.6, copyWidthOf(1024), 1024, false));
        QVERIFY(useFile(8, copyWidthOf(4096), 4096, false));
        // A face at its own fit, 800 px wide, showing a 3072 px restore: the 512 px copy is stretched 1.56x, so the file.
        QVERIFY(useFile(800.0 / 3072, copyWidthOf(3072), 3072, false));
    }

    void hysteresisKeepsTheChoiceBetweenTheThresholds() {
        // 4096 px: covers = scale * 8, taken above 1.1 (scale 0.1375), let go below 0.9 (scale 0.1125).
        QVERIFY(!useFile(0.125, 512, 4096, false)); // covers 1.0, was the copy: stays the copy
        QVERIFY(useFile(0.125, 512, 4096, true));   // covers 1.0, was the file: stays the file
        QVERIFY(!useFile(0.1, 512, 4096, true));    // covers 0.8: back to the copy
    }

    void smallFilesAlwaysDrawTheCopy() {
        QVERIFY(!useFile(8, copyWidthOf(400), 400, true)); // the file is its own copy
        QVERIFY(!useFile(8, 0, 0, false));     // no size yet
    }

    // 100% follows the larger picture (#420): a 3x restore B against its original A.
    void hundredPercentIsTheLargerPicturesAndTogglesWhenTheSizesDiffer() {
        QCOMPARE(actualScale(1024, 1024, 3072, 3072, true), 3.0);  // B's real pixels: A's frame at 3x
        QCOMPARE(actualScale(1024, 1024, 3072, 3072, false), 1.0); // 100% of A
        QCOMPARE(actualScale(1024, 512, 3072, 3072, true), 6.0);   // B is cropped to A's shape: the larger ratio covers
        QCOMPARE(actualScale(1024, 1024, 512, 512, true), 1.0);    // B smaller: A stays the 100%
        QCOMPARE(actualScale(1024, 1024, 1024, 1024, true), 1.0);  // equal
        QCOMPARE(actualScale(1024, 1024, 0, 0, true), 1.0);        // no B (a single picture)

        // From Fit: B; pressed again: A; again: B.
        Actual v = nextActual(1024, 1024, 3072, 3072, false, false);
        QVERIFY(v.actual && v.ofB);
        v = nextActual(1024, 1024, 3072, 3072, true, v.ofB);
        QVERIFY(v.actual && !v.ofB);
        v = nextActual(1024, 1024, 3072, 3072, true, v.ofB);
        QVERIFY(v.actual && v.ofB);
        // B the same size or smaller: always A.
        for (double side : {512.0, 1024.0}) {
            v = nextActual(1024, 1024, side, side, false, false);
            QVERIFY(v.actual && !v.ofB);
            v = nextActual(1024, 1024, side, side, true, false);
            QVERIFY(v.actual && !v.ofB);
        }
        // The detail rule at 100% of B shows the file one to one, so it loads the files.
        const double shown = actualScale(512, 512, 3072, 3072, true) * 512 / 3072;
        QCOMPARE(shown, 1.0);
        QVERIFY(useFile(shown, copyWidthOf(3072), 3072, false));
    }
};

QTEST_GUILESS_MAIN(TestCompareDetail)
#include "test_compare_detail.moc"
