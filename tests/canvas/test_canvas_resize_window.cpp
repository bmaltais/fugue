// Resizing and auto-size in the real window (#49): the grip is dragged and double-clicked with
// the mouse, undo takes the size back, a saved workflow comes back with its sizes, and an
// image drives the node's size.

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QQuickStyle>
#include <QUndoStack>

#include "canvas_app.h"

using namespace fugue;
using canvas_test::App;
using canvas_test::item;
using canvas_test::load;

namespace {

const char *sheet = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}}}
  }},
  "layout": {"nodes": {"gen": {"pos": [100, 100]}, "save": {"pos": [600, 100]}},
             "view": {"center": [500, 300], "zoom": 1}}
})";

QQuickItem *nodeItem(App &app, const QString &id) { return item(app.window->contentItem(), "node-" + id); }

// The node's bottom right corner, a little inside, in window coordinates.
QPoint grip(App &app, const QString &id) {
    const QRectF box(nodeItem(app, id)->position(), nodeItem(app, id)->size());
    return app.screen(box.right() - 4, box.bottom() - 4);
}

} // namespace

class TestCanvasResizeWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void draggingTheGripResizesTheNodeAndUndoTakesItBack() {
        App app(load(sheet));
        QVERIFY(app.open());
        const QSizeF before = nodeItem(app, "gen")->size();
        const QPoint from = grip(app, "gen");
        app.drag(Qt::LeftButton, from, from + QPoint(60, 40));
        QCOMPARE(app.model.document().layout.value("nodes").toObject().value("gen").toObject().value("size").toArray(),
                 (QJsonArray{before.width() + 60, before.height() + 40}));
        QVERIFY(qAbs(nodeItem(app, "gen")->width() - (before.width() + 60)) < 1);
        QVERIFY(qAbs(nodeItem(app, "gen")->height() - (before.height() + 40)) < 1);
        app.model.undo();
        QCOMPARE(nodeItem(app, "gen")->size(), before);
        QVERIFY(!app.model.document().layout.value("nodes").toObject().value("gen").toObject().contains("size"));
    }

    void aDoubleClickOnTheGripReturnsToAuto() {
        App app(load(sheet));
        QVERIFY(app.open());
        const QSizeF before = nodeItem(app, "gen")->size();
        QPoint at = grip(app, "gen");
        app.drag(Qt::LeftButton, at, at + QPoint(50, 50));
        QVERIFY(nodeItem(app, "gen")->width() > before.width());
        const int steps = app.model.undoStack()->count();
        at = grip(app, "gen");
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, at);
        QCOMPARE(nodeItem(app, "gen")->size(), before);
        QCOMPARE(app.model.undoStack()->count(), steps + 1); // one step
        QVERIFY(app.warnings.isEmpty());
    }

    void aSavedWorkflowOpensWithItsSizes() {
        QByteArray saved;
        {
            App app(load(sheet));
            QVERIFY(app.open());
            const QPoint from = grip(app, "save");
            app.drag(Qt::LeftButton, from, from + QPoint(80, 30));
            saved = app.model.document().save();
        }
        App app(load(saved));
        QVERIFY(app.open());
        App plain(load(sheet));
        QVERIFY(plain.open());
        QVERIFY(qAbs(nodeItem(app, "save")->width() - (nodeItem(plain, "save")->width() + 80)) < 1);
        QVERIFY(qAbs(nodeItem(app, "save")->height() - (nodeItem(plain, "save")->height() + 30)) < 1);
        QCOMPARE(nodeItem(app, "gen")->size(), nodeItem(plain, "gen")->size());
    }

    // FUGUE_SIZE_SHOT=<file.png> QT_QUICK_BACKEND=rhi build/test_canvas_resize_window screenshot
    void screenshot() {
        const QString out = qEnvironmentVariable("FUGUE_SIZE_SHOT");
        if (out.isEmpty())
            QSKIP("set FUGUE_SIZE_SHOT to a .png path");
        App app(load(R"({"fugue":1,"graph":{"nodes":{
          "p":{"type":"fugue/text","version":"1.0.0","inputs":{"text":"a red fox in a misty birch wood at dawn, soft light, a long prompt that wraps over several lines of the node"}},
          "g":{"type":"fugue/generate-image","version":"1.0.0","inputs":{"prompt":{"from":"p.text"}}},
          "x":{"type":"acme.tools/ghost","version":"1.0.0","inputs":{"in":{"from":"p.text"}}}}},
          "layout":{"nodes":{"p":{"pos":[0,0]},"g":{"pos":[300,0]},"x":{"pos":[0,200]}},"view":{"center":[300,150],"zoom":1}}})"));
        QVERIFY(app.open());
        QTest::mouseMove(app.window, grip(app, "g"));
        QTest::qWait(200);
        QVERIFY(app.window->grabWindow().save(out));
    }

    void aPressOnTheBodyStillMovesTheNode() {
        App app(load(sheet));
        QVERIFY(app.open());
        const QPoint from = app.screen(100 + 120, 100 + 14);
        app.drag(Qt::LeftButton, from, from + QPoint(30, 0));
        QVERIFY(qAbs(app.model.position("gen").x() - 130) < 1);
        QVERIFY(!app.model.document().layout.value("nodes").toObject().value("gen").toObject().contains("size"));
    }

    // A fixture image stands in for a result: its aspect gives the preview's height, and the
    // thumbnail item sits where the model says. A wider node makes a taller preview.
    void anImageDrivesAutoSize() {
        QTemporaryDir dir;
        const QString file = dir.filePath("fixture.png");
        QImage image(400, 200, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QVERIFY(image.save(file));
        App app(load(sheet));
        QVERIFY(app.open());
        const qreal plain = nodeItem(app, "save")->height();
        const QImage head(file);
        app.model.setRunState("save", {{"state", "cached"}, {"thumbnail", QUrl::fromLocalFile(file).toString()},
                                       {"thumbnailAspect", qreal(head.height()) / head.width()},
                                       {"thumbnailInfo", "400 × 200"}});
        QTRY_VERIFY(nodeItem(app, "save")->height() > plain);
        QCOMPARE(nodeItem(app, "save")->height(), plain + 4 + 114);
        QQuickItem *thumb = item(nodeItem(app, "save"), "thumbnail");
        QVERIFY(thumb);
        QCOMPARE(thumb->height(), 114.0);
        QCOMPARE(thumb->width(), 228.0);
        // Dragging the grip wider re-fits the preview to the node.
        const QPoint from = grip(app, "save");
        app.drag(Qt::LeftButton, from, from + QPoint(100, 0));
        QCOMPARE(thumb->width(), 328.0);
        QCOMPARE(thumb->height(), 164.0);
        QCOMPARE(nodeItem(app, "save")->height(), plain + 4 + 164);
        // A pinned height stays when the result goes; auto-size gives it back.
        app.model.clearRunState();
        QCOMPARE(nodeItem(app, "save")->height(), plain + 4 + 164);
        QVERIFY(app.model.autoSize("save"));
        QCOMPARE(nodeItem(app, "save")->height(), plain);
    }
};

QTEST_MAIN(TestCanvasResizeWindow)
#include "test_canvas_resize_window.moc"
