// The Canvas view's guides model (#253): guides kept per canvas in a sidecar file, validated on read, never a revision, and
// the snap function the Region frame calls. The model has no window; the store is the test's own folder.

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/guides_model.h"
#include "engine/canvas_store.h"

using namespace fugue;

class TestGuidesModel : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_a, m_b;

    static QVariantMap rect(int x, int y, int w, int h) { return {{"x", x}, {"y", y}, {"width", w}, {"height", h}}; }

private slots:
    void init() {
        canvas::CanvasStore store(m_dir.path());
        mask::PaintDocument doc(400, 300);
        doc.addLayer(mask::LayerKind::Paint, "Base");
        m_a = store.create("a", doc, "test")->id;
        m_b = store.create("b", doc, "test")->id;
    }

    void guidesAreAddedMovedAndRemovedAndSurviveAReload() {
        GuidesModel model;
        model.setStoreRoot(m_dir.path());
        model.setCanvasId(m_a);
        model.setCanvasSize({400, 300});
        QVERIFY(model.guides().isEmpty());
        QSignalSpy changed(&model, &GuidesModel::guidesChanged);
        QVERIFY(model.addGuide("x", 120));
        QVERIFY(model.addGuide("y", 80));
        QVERIFY(!model.addGuide("x", 120));   // a repeat
        QVERIFY(!model.addGuide("x", 401));   // off the canvas
        QVERIFY(!model.addGuide("x", -1));
        QVERIFY(!model.addGuide("y", 301));
        QVERIFY(!model.addGuide("z", 5));
        QVERIFY(changed.count() >= 2);
        QCOMPARE(model.guides().size(), 2);
        QCOMPARE(model.guides()[0].toMap().value("axis").toString(), QString("x"));
        QVERIFY(model.moveGuide(0, 140));
        QVERIFY(!model.moveGuide(0, 999));
        QVERIFY(!model.moveGuide(5, 10));
        QCOMPARE(model.guides()[0].toMap().value("at").toInt(), 140);
        // Another model on the same folder reads what was kept.
        GuidesModel again;
        again.setStoreRoot(m_dir.path());
        again.setCanvasId(m_a);
        QCOMPARE(again.guides().size(), 2);
        QCOMPARE(again.guides()[0].toMap().value("at").toInt(), 140);
        // They belong to the canvas: another one has none.
        again.setCanvasId(m_b);
        QVERIFY(again.guides().isEmpty());
        QVERIFY(model.removeGuide(1));
        QCOMPARE(model.guides().size(), 1);
        QVERIFY(model.clearGuides());
        QVERIFY(!model.clearGuides());
        QVERIFY(model.guides().isEmpty());
    }

    void moveAndRemoveKeepTheIndexOrderOfTheList() {
        GuidesModel model;
        model.setStoreRoot(m_dir.path());
        model.setCanvasId(m_a);
        model.setCanvasSize({400, 300});
        model.addGuide("x", 300);
        model.addGuide("x", 100);
        QCOMPARE(model.guides()[0].toMap().value("at").toInt(), 100); // by position
        QVERIFY(model.moveGuide(0, 350)); // 100 -> 350 re-sorts: the list is [300, 350]
        QCOMPARE(model.guides()[0].toMap().value("at").toInt(), 300);
        QCOMPARE(model.guides()[1].toMap().value("at").toInt(), 350);
    }

    void theGridSpacingIsKeptWithTheGuidesAndClamped() {
        GuidesModel model;
        model.setStoreRoot(m_dir.path());
        model.setCanvasId(m_a);
        QCOMPARE(model.gridSpacing(), model.defaultGrid());
        QVERIFY(model.setGridSpacing(24));
        QVERIFY(!model.setGridSpacing(24));
        QVERIFY(model.setGridSpacing(1)); // clamped to the least
        QCOMPARE(model.gridSpacing(), model.minGrid());
        QVERIFY(model.setGridSpacing(1000000));
        QCOMPARE(model.gridSpacing(), model.maxGrid());
        GuidesModel again;
        again.setStoreRoot(m_dir.path());
        again.setCanvasId(m_a);
        QCOMPARE(again.gridSpacing(), model.maxGrid());
    }

    void aDamagedFileReadsAsNoGuidesWithAProblem() {
        {
            QFile file(m_dir.path() + "/canvases/" + m_a + "/guides.json");
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(R"({"format": 1, "guides": [{"axis": "q", "at": 1}]})");
        }
        GuidesModel model;
        model.setStoreRoot(m_dir.path());
        model.setCanvasId(m_a);
        QVERIFY(model.guides().isEmpty());
        QVERIFY(model.problem().contains("guides"));
        model.setCanvasSize({400, 300});
        QVERIFY(model.addGuide("x", 10)); // writing replaces the bad file
        QVERIFY(model.problem().isEmpty());
    }

    void snapRectPullsToTheGuidesGridAndMultipleAsAsked() {
        GuidesModel model;
        model.setStoreRoot(m_dir.path());
        model.setCanvasId(m_a);
        model.setCanvasSize({400, 300});
        model.addGuide("x", 100);
        model.setGridSpacing(50);
        // Zoom 2: 8 screen pixels are 4 canvas pixels.
        const QVariantMap guideOnly = {{"guides", true}, {"zoom", 2.0}};
        QCOMPARE(model.snapRect(rect(102, 20, 80, 40), false, guideOnly).value("x").toInt(), 100);
        QCOMPARE(model.snapRect(rect(106, 20, 80, 40), false, guideOnly).value("x").toInt(), 106); // 6 > 4: not pulled
        // The grid is off unless asked for.
        QCOMPARE(model.snapRect(rect(152, 22, 80, 40), false, guideOnly).value("y").toInt(), 22);
        const QVariantMap gridToo = {{"guides", true}, {"grid", true}, {"zoom", 2.0}};
        const QVariantMap r = model.snapRect(rect(152, 22, 80, 40), false, gridToo);
        QCOMPARE(r.value("x").toInt(), 150);
        QCOMPARE(r.value("y").toInt(), 22); // 22 is 22 off the grid line 0 and 28 off 50: not pulled
    }

    void theSnapPointIsTheSameFunction() {
        GuidesModel model;
        model.setStoreRoot(m_dir.path());
        model.setCanvasId(m_a);
        model.setCanvasSize({400, 300});
        model.addGuide("y", 200);
        const QVariantMap p = model.snapPoint({{"x", 17}, {"y", 203}}, {{"guides", true}, {"zoom", 1.0}});
        QCOMPARE(p.value("y").toInt(), 200);
        QCOMPARE(p.value("x").toInt(), 17);
    }
};

QTEST_GUILESS_MAIN(TestGuidesModel)
#include "test_guides_model.moc"
