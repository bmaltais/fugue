// Where the person left the canvas task bar (#381): remembered as fractions of the room, held inside the area at any size,
// pinned, reset. Seam: canvas/task_bar_place.h (no window; the drag itself is MoveGrip's and float_place's).

#include <QtTest>

#include "canvas/task_bar_place.h"

using fugue::TaskBarPlace;

class TestTaskBarPlace : public QObject {
    Q_OBJECT

private slots:
    void neverMovedHasNoPlaceAndPinsNothing() {
        TaskBarPlace bar({});
        QVERIFY(!bar.hasPlace());
        bar.setPinned(true);
        QVERIFY(!bar.pinned());
    }

    void aPlaceIsKeptAndSurvivesARestart() {
        QTemporaryDir dir;
        const QString path = dir.path() + "/bar.json";
        {
            TaskBarPlace bar(path);
            bar.setPlace({0.25, 0.75});
            bar.setPinned(true);
        }
        TaskBarPlace again(path);
        QVERIFY(again.hasPlace());
        QCOMPARE(again.place(), QPointF(0.25, 0.75));
        QVERIFY(again.pinned());
    }

    void aWildPlaceIsHeldToTheRoomAndABadFileIsTheDefault() {
        QTemporaryDir dir;
        const QString path = dir.path() + "/bar.json";
        TaskBarPlace bar(path);
        bar.setPlace({-3, 9});
        QCOMPARE(bar.place(), QPointF(0, 1));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not json");
        f.close();
        QVERIFY(!TaskBarPlace(path).hasPlace());
    }

    void itCanNeverLieOutsideTheAreaAfterAResize() {
        TaskBarPlace bar({});
        bar.setPlace({1, 1}); // the far corner
        const QSizeF box(600, 80);
        const QPointF big = bar.positionIn({1200, 800}, box, 16);
        QCOMPARE(big, QPointF(1200 - 600 - 16, 800 - 80 - 16));
        const QPointF small = bar.positionIn({700, 300}, box, 16); // the window shrank
        QCOMPARE(small, QPointF(700 - 600 - 16, 300 - 80 - 16));
        const QPointF tiny = bar.positionIn({400, 60}, box, 16); // narrower than the bar: centred, not lost
        QCOMPARE(tiny, QPointF((400 - 600) / 2.0, (60 - 80) / 2.0));
    }

    void pinningHoldsItWhereItIsAndResetGivesTheDefaultBack() {
        QTemporaryDir dir;
        const QString path = dir.path() + "/bar.json";
        TaskBarPlace bar(path);
        bar.setPlace({0.5, 0.5});
        bar.setPinned(true);
        bar.setPlace({0, 0}); // refused while pinned
        QCOMPARE(bar.place(), QPointF(0.5, 0.5));
        bar.setPinned(false);
        bar.setPlace({0, 0});
        QCOMPARE(bar.place(), QPointF(0, 0));
        bar.setPinned(true);
        bar.reset();
        QVERIFY(!bar.hasPlace());
        QVERIFY(!bar.pinned());
        QVERIFY(!QFile::exists(path));
    }
};

QTEST_GUILESS_MAIN(TestTaskBarPlace)
#include "test_task_bar_place.moc"
