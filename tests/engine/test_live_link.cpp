// The canvas live link's decisions (#315, docs/canvas-actions-design.md 16): debounce, one run at a time, cancel the stale run, never
// run a shown state again, switch off on a failure. Seam: engine/live_link.h with a fake clock (the `now` argument); no window.

#include <QtTest>

#include "engine/live_link.h"

using namespace fugue;
using Do = LiveLink::Do;
using End = LiveLink::End;

class TestLiveLink : public QObject {
    Q_OBJECT
    static LiveLink on(const QString &key = "a") {
        LiveLink link;
        link.setEnabled(true, 0);
        link.note(key, true, 0);
        return link;
    }

private slots:
    void aStateRunsOnlyAfterItHasBeenStill() {
        LiveLink link = on();
        QCOMPARE(link.poll(100).what, Do::Nothing);
        QCOMPARE(link.poll(100).waitMs, 500);
        link.note("b", true, 400); // a change restarts the wait
        QCOMPARE(link.poll(900).what, Do::Nothing);
        QCOMPARE(link.poll(1000).what, Do::Start);
    }

    void aChangeCancelsTheStaleRunOnceAndTheNextWaitsForItsEnd() {
        LiveLink link = on();
        QCOMPARE(link.poll(600).what, Do::Start);
        link.note("b", true, 700);
        QCOMPARE(link.poll(700).what, Do::Cancel);
        QCOMPARE(link.poll(710).what, Do::Nothing); // already asked: not again
        link.ended(End::Cancelled, 720);
        QCOMPARE(link.poll(1000).what, Do::Nothing); // the quiet time has not passed
        QCOMPARE(link.poll(1300).what, Do::Start);
    }

    void aShownStateIsNotRunAgain() {
        LiveLink link = on();
        QCOMPARE(link.poll(600).what, Do::Start);
        link.ended(End::Ok, 800);
        QCOMPARE(link.poll(5000).what, Do::Nothing);
        link.note("a", true, 5000); // the same key again changes nothing
        QCOMPARE(link.poll(9000).what, Do::Nothing);
        link.note("b", true, 9000);
        link.note("a", true, 9100); // back to the shown state: its preview is still there, no run
        QCOMPARE(link.poll(20000).what, Do::Nothing);
    }

    void aChangeBackToTheRunningStateKeepsTheRun() {
        LiveLink link = on();
        QCOMPARE(link.poll(600).what, Do::Start);
        link.note("a", true, 650);
        QCOMPARE(link.poll(700).what, Do::Nothing);
    }

    void notReadyStartsNothingAndCancelsARun() {
        LiveLink link = on();
        link.note("a", false, 0);
        QCOMPARE(link.poll(5000).what, Do::Nothing);
        link.note("a", true, 5000);
        QCOMPARE(link.poll(5600).what, Do::Start);
        link.note("a", false, 5700); // the selection went
        QCOMPARE(link.poll(5700).what, Do::Cancel);
    }

    void aSlowRunLengthensTheWaitUpToTheCeiling() {
        LiveLink link = on();
        QCOMPARE(link.quiet(), 600);
        QCOMPARE(link.poll(600).what, Do::Start);
        link.ended(End::Ok, 600 + 4000);
        QCOMPARE(link.quiet(), 2000);
        link.note("b", true, 5000);
        QCOMPARE(link.poll(6999).what, Do::Nothing);
        QCOMPARE(link.poll(7000).what, Do::Start);
        link.ended(End::Ok, 7000 + 60000);
        QCOMPARE(link.quiet(), 5000);
    }

    void aFailureSwitchesTheLinkOff() {
        LiveLink link = on();
        QCOMPARE(link.poll(600).what, Do::Start);
        link.ended(End::Failed, 700);
        QVERIFY(!link.enabled());
        link.note("b", true, 800);
        QCOMPARE(link.poll(9000).what, Do::Nothing);
    }

    void keepingThePreviewRunsNothingUntilTheNextChange() {
        LiveLink link = on();
        QCOMPARE(link.poll(600).what, Do::Start);
        link.ended(End::Ok, 900);
        link.adopt("a+layer"); // the kept layer moved the head
        link.note("a+layer", true, 1000);
        QCOMPARE(link.poll(9000).what, Do::Nothing);
        link.note("a+layer+prompt", true, 9000);
        QCOMPARE(link.poll(9600).what, Do::Start);
    }

    void offDoesNothingAndOnRunsTheCurrentStateOnce() {
        LiveLink link;
        link.note("a", true, 0);
        QCOMPARE(link.poll(9000).what, Do::Nothing);
        link.setEnabled(true, 10000);
        QCOMPARE(link.poll(10100).what, Do::Nothing);
        QCOMPARE(link.poll(10600).what, Do::Start);
    }
};

QTEST_GUILESS_MAIN(TestLiveLink)
#include "test_live_link.moc"
