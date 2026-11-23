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
             "view": {"center": [500, 380], "zoom": 1}}
})";

const char *sinks = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {}},
    "show": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "gen.image"}}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}}}
  }},
  "layout": {"nodes": {"gen": {"pos": [0, 0]}, "show": {"pos": [300, 0]}, "save": {"pos": [300, 300]}},
             "view": {"center": [300, 200], "zoom": 1}}
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
        // The auto size settles a moment after opening (a busy machine shows it late): read it once it holds still.
        QSizeF before = nodeItem(app, "gen")->size();
        for (int i = 0; i < 40; ++i) {
            QTest::qWait(50);
            const QSizeF now = nodeItem(app, "gen")->size();
            if (now == before)
                break;
            before = now;
        }
        QPoint at = grip(app, "gen");
        app.drag(Qt::LeftButton, at, at + QPoint(50, 50));
        QVERIFY(nodeItem(app, "gen")->width() > before.width());
        const int steps = app.model.undoStack()->count();
        at = grip(app, "gen");
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, at);
        QTRY_COMPARE(nodeItem(app, "gen")->size(), before);
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
    // with the corner radius FUGUE_SIZE_RADIUS (0 when unset)
    void screenshot() {
        const QString out = qEnvironmentVariable("FUGUE_SIZE_SHOT");
        if (out.isEmpty())
            QSKIP("set FUGUE_SIZE_SHOT to a .png path");
        App app(load(R"({"fugue":1,"graph":{"nodes":{
          "p":{"type":"fugue/text","version":"1.0.0","inputs":{"text":"a red fox in a misty birch wood at dawn, soft light, a long prompt that wraps over several lines of the node"}},
          "g":{"type":"fugue/generate-image","version":"1.0.0","inputs":{"prompt":{"from":"p.text"}}},
          "x":{"type":"acme.tools/ghost","version":"1.0.0","inputs":{"in":{"from":"p.text"}}}}},
          "layout":{"nodes":{"p":{"pos":[0,0]},"g":{"pos":[300,0]},"x":{"pos":[0,200]}},"view":{"center":[300,150],"zoom":1}}})"),
                {}, canvas_test::Look{{}, qEnvironmentVariableIntValue("FUGUE_SIZE_RADIUS")});
        QVERIFY(app.open());
        app.workflows.dismissPendingTrust(); // the placeholder's pack: not what this shows
        app.workflows.dismissNotice();
        // "p" selected shows its quiet mark; the pointer on "g"'s top left corner shows that one.
        app.model.select({"p"});
        const QPointF g = app.model.position("g");
        QTest::mouseMove(app.window, app.screen(g.x() + 4, g.y() + 4));
        QTest::qWait(200);
        QVERIFY(test_support::grab(app.window).save(out));
    }

    // Any corner resizes (#63); the top left one moves the node too, so its bottom right corner
    // stays where it was, and one undo takes both back.
    void draggingTheTopLeftCornerResizesAndMovesTheNode() {
        App app(load(sheet));
        QVERIFY(app.open());
        const QSizeF before = nodeItem(app, "gen")->size();
        const QPoint from = app.screen(100 + 4, 100 + 4);
        app.drag(Qt::LeftButton, from, from + QPoint(-40, -30));
        QVERIFY(qAbs(app.model.position("gen").x() - 60) < 1);
        QVERIFY(qAbs(app.model.position("gen").y() - 70) < 1);
        QVERIFY(qAbs(nodeItem(app, "gen")->width() - (before.width() + 40)) < 1);
        QVERIFY(qAbs(nodeItem(app, "gen")->height() - (before.height() + 30)) < 1);
        QVERIFY(qAbs(nodeItem(app, "gen")->x() + nodeItem(app, "gen")->width() - (100 + before.width())) < 1);
        app.model.undo();
        QCOMPARE(app.model.position("gen"), QPointF(100, 100));
        QCOMPARE(nodeItem(app, "gen")->size(), before);
        QVERIFY(app.warnings.isEmpty());
    }

    // A port wins over the corner it sits in: a press on the last input of a node, inside its
    // bottom left corner's square, drags a link, not the node's size.
    void aPortInACornerStillStartsALink() {
        App app(load(sheet));
        QVERIFY(app.open());
        const QRectF box(nodeItem(app, "gen")->position(), nodeItem(app, "gen")->size());
        QVariantMap port;
        qreal y = box.bottom();
        while (port.isEmpty() && y > box.bottom() - 40)
            port = app.model.portAt(box.left(), --y, 8);
        QVERIFY(!port.isEmpty());
        QVERIFY(!app.model.resizeCornerAt(box.left() + 2, y, 16).isEmpty()); // inside the square
        const QPoint at = app.screen(box.left() + 2, y);
        QTest::mousePress(app.window, Qt::LeftButton, {}, at);
        QTest::mouseMove(app.window, at + QPoint(-30, 10));
        QTest::mouseMove(app.window, at + QPoint(-60, 20));
        QCOMPARE(app.canvas->property("gesture").toString(), QStringLiteral("link"));
        QTest::mouseRelease(app.window, Qt::LeftButton, {}, at + QPoint(-60, 20));
        QCOMPARE(nodeItem(app, "gen")->size(), box.size());
    }

    // Each corner shows the resize cursor of its diagonal before the press; the body does not.
    void eachCornerShowsItsResizeCursor() {
        App app(load(sheet));
        QVERIFY(app.open());
        QQuickItem *input = item(app.window->contentItem(), "canvasInput");
        QVERIFY(input);
        const QRectF box(nodeItem(app, "gen")->position(), nodeItem(app, "gen")->size());
        const QList<std::pair<QPointF, Qt::CursorShape>> corners{
            {box.topLeft() + QPointF(4, 4), Qt::SizeFDiagCursor},
            {box.bottomRight() - QPointF(4, 4), Qt::SizeFDiagCursor},
            {box.topRight() + QPointF(-4, 4), Qt::SizeBDiagCursor},
            {box.bottomLeft() + QPointF(4, -4), Qt::SizeBDiagCursor},
            {box.center(), Qt::ArrowCursor}};
        for (const auto &[at, shape] : corners) {
            QTest::mouseMove(app.window, app.screen(at.x(), at.y()));
            QTRY_COMPARE(input->cursor().shape(), shape);
        }
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
    void theInfoChipNeverPassesTheNodeAndOnlySaveImageNamesAFile() {
        QTemporaryDir dir;
        const QString file = dir.filePath("fixture.png");
        QImage image(800, 1024, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QVERIFY(image.save(file));
        const QString hash = "b512d2e337f6a4507fe7c4c6a08ba8d0c36cd2a7a494aa523331dcb026e0ee78.png";
        App app(load(sinks));
        QVERIFY(app.open());
        for (const char *id : {"show", "save"}) {
            // What CanvasRuns sends: a Preview has no file; a Save Image has the name it wrote.
            const bool saves = QLatin1String(id) == "save";
            app.model.setRunState(id, {{"state", "cached"}, {"thumbnail", QUrl::fromLocalFile(file).toString()},
                                       {"thumbnailAspect", 1024.0 / 800}, {"thumbnailInfo", "800 \u00d7 1024"},
                                       {"thumbnailFile", saves ? hash : QString()}});
        }
        for (const char *id : {"show", "save"}) {
            QQuickItem *node = nodeItem(app, id);
            QTRY_VERIFY(item(node, "infoChip") && item(node, "infoChip")->isVisible());
            QQuickItem *chip = item(node, "infoChip");
            QQuickItem *thumb = item(node, "thumbnail");
            QVERIFY(chip->width() > 0);
            QVERIFY2(chip->width() <= thumb->width(), id);
            QVERIFY(chip->x() >= 0 && chip->x() + chip->width() <= thumb->width());
            QQuickItem *size = item(chip, "infoSize");
            QVERIFY(size->property("text").toString().endsWith("800 \u00d7 1024"));
            QVERIFY(size->x() >= 0 && size->x() + size->width() <= chip->width()); // the size stays whole
            QQuickItem *name = item(chip, "infoFile");
            if (QLatin1String(id) == "show") {
                QVERIFY(name->property("text").toString().isEmpty());
                QVERIFY(!size->property("text").toString().contains("b512"));
            } else {
                QCOMPARE(name->property("text").toString(), hash); // the full name, for the tooltip
                QVERIFY(name->width() < name->property("implicitWidth").toReal());  // elided
                QVERIFY(name->x() + name->width() <= size->x());
            }
        }
        QVERIFY(app.warnings.isEmpty());
    }

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
        // When the result goes, the pinned height was only room for it: the node is its
        // content's height again (Appendix A). Auto-size then only unpins it.
        app.model.clearRunState();
        QCOMPARE(nodeItem(app, "save")->height(), plain);
        QVERIFY(app.model.autoSize("save"));
        QCOMPARE(nodeItem(app, "save")->height(), plain);
    }
};

QTEST_MAIN(TestCanvasResizeWindow)
#include "test_canvas_resize_window.moc"
