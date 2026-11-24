// What one Generate came to (#231, #337): what lands from a run's events, what a failed or cancelled run leaves, and when Expand's
// growth is taken back. Seam: engine/canvas_act_outcome.h with the session's canvas store in the test's own home; the task bar and
// `fugue canvas act` only show this answer.

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include <vector>

#include "engine/canvas_act_outcome.h"
#include "engine/canvas_store.h"
#include "engine/paint_document.h"

using namespace fugue;
using namespace fugue::canvas;
using namespace fugue::mask;

namespace {

QString makeCanvas() {
    PaintDocument document(64, 64);
    document.addLayer(LayerKind::Paint, "Base");
    document.fillRect(0, 0, 64, 64);
    return CanvasStore::forSession().create("outcome", document, "test")->id;
}

QString head(const QString &canvas) { return CanvasStore::forSession().info(canvas)->head; }

// A layer on the head, as a Write lands it: the new revision.
QString land(const QString &canvas) {
    CanvasStore store = CanvasStore::forSession();
    const MaskPaint picture(8, 8, std::vector<uint8_t>(8 * 8 * 4, 255), 4);
    return store.pasteLayer(canvas, head(canvas), picture, 16, 16, -1, "Fill", "test")->revision.id;
}

QString grow(const QString &canvas) {
    return CanvasStore::forSession().resize(canvas, head(canvas), IRect{0, 0, 80, 64}, edgefill::Mode::Edge, 0, "action:test")->revision.id;
}

QJsonObject result(const char *kind, const char *node, const QString &canvas, const QString &revision) {
    const QJsonObject meta{{"canvas", canvas}, {"revision", revision}};
    return {{"event", kind}, {"node", node}, {"outputs", QJsonObject{{"canvas", QJsonObject{{"meta", meta}}}}}};
}

ActOutcome outcomeFor(const QString &canvas, const QString &growRevision = {}) {
    ActionGraph graph;
    graph.writes << "write";
    graph.news << "new";
    ActPlan plan;
    plan.canvas = canvas;
    plan.grew = !growRevision.isEmpty();
    plan.growRevision = growRevision;
    return ActOutcome(graph, plan, "Fill");
}

} // namespace

class TestCanvasActOutcome : public QObject {
    Q_OBJECT

private slots:
    void anOkRunPlacesItsCachedWriteAgain() {
        const QString canvas = makeCanvas(), before = head(canvas), landed = land(canvas);
        QVERIFY(CanvasStore::forSession().undo(canvas, landed)->moved);
        ActOutcome outcome = outcomeFor(canvas);
        QCOMPARE(outcome.note("r", result("node.cached", "write", canvas, landed)), landed);
        QCOMPARE(head(canvas), before); // nothing moves before the run has ended ok
        const ActOutcome::RunEnd end = outcome.endRun("r", "ok");
        QVERIFY(!end.error);
        QCOMPARE(head(canvas), landed);
        // Still on the head: nothing new can come of the same prompt, seed and selection, and the notice says why.
        outcome.note("again", result("node.cached", "write", canvas, landed));
        QVERIFY(outcome.endRun("again", "ok").notice.contains("Fill"));
    }

    void aRunThatDidNotEndOkLandsNothing_data() {
        QTest::addColumn<QString>("status");
        QTest::addColumn<QString>("code");
        QTest::newRow("failed") << "failed" << "real_error";
        QTest::newRow("cancelled") << "cancelled" << "";
    }
    void aRunThatDidNotEndOkLandsNothing() {
        QFETCH(QString, status);
        QFETCH(QString, code);
        const QString canvas = makeCanvas(), before = head(canvas), landed = land(canvas);
        QVERIFY(CanvasStore::forSession().undo(canvas, landed)->moved);
        ActOutcome outcome = outcomeFor(canvas);
        outcome.note("r", result("node.cached", "write", canvas, landed));
        outcome.note("r", result("node.done", "new", "canvas-made", {}));
        Error warning = makeError("a_warning", "Only a warning.");
        warning.severity = Error::Severity::Warning;
        outcome.note("r", {{"event", "run.done"}, {"errors", toJson(Errors{warning, makeError("real_error", "It broke.")})}});
        const ActOutcome::RunEnd end = outcome.endRun("r", status);
        QCOMPARE(head(canvas), before);
        QVERIFY(end.made.isEmpty());
        QCOMPARE(end.error ? end.error->code : QString(), code);
    }

    void aCachedWriteThatCannotBePlacedIsAnError() {
        const QString canvas = makeCanvas(), landed = land(canvas);
        QVERIFY(CanvasStore::forSession().undo(canvas, landed)->moved);
        const QString grown = grow(canvas); // the canvas has another size now
        ActOutcome outcome = outcomeFor(canvas);
        outcome.note("r", result("node.cached", "write", canvas, landed));
        const ActOutcome::RunEnd end = outcome.endRun("r", "ok");
        QVERIFY(end.error);
        QCOMPARE(end.error->code, QString("canvas_conflict"));
        QCOMPARE(head(canvas), grown);
    }

    void aFailedExpandTakesItsGrowthBack() {
        const QString canvas = makeCanvas(), before = head(canvas);
        ActOutcome outcome = outcomeFor(canvas, grow(canvas));
        outcome.endRun("r", "failed");
        QVERIFY(outcome.finish().contains("did not finish"));
        QCOMPARE(head(canvas), before);
        QVERIFY(outcome.finish().isEmpty()); // once
    }

    void aVariationThatLandedKeepsTheGrowth() {
        const QString canvas = makeCanvas();
        ActOutcome outcome = outcomeFor(canvas, grow(canvas));
        const QString landed = land(canvas);
        outcome.endRun("first", "ok");
        outcome.endRun("second", "cancelled");
        QVERIFY(outcome.finish().isEmpty());
        QCOMPARE(head(canvas), landed);
    }

    void anOkExpandKeepsTheGrowth() {
        const QString canvas = makeCanvas(), grown = grow(canvas);
        ActOutcome outcome = outcomeFor(canvas, grown);
        outcome.endRun("r", "ok");
        QVERIFY(outcome.finish().isEmpty());
        QCOMPARE(head(canvas), grown);
    }
};

QTEST_GUILESS_MAIN(TestCanvasActOutcome)
#include "test_canvas_act_outcome.moc"
