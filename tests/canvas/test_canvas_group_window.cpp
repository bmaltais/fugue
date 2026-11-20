// Groups and Frames in the window (spec 5, 12.4): Ctrl+G frames, Ctrl+Shift+E groups (asking
// about sinks first), Tab and a double-click on the body enter, Escape and the breadcrumb leave,
// Ctrl+Shift+G ungroups, a Frame's title drags it with its nodes, and the Advanced header opens
// and folds. Driven by keys and the mouse on the real Main.qml, offscreen.

#include <QtTest>

#include "canvas_app.h"
#include "../window_grab.h"

using namespace canvas_test;

namespace {

const QByteArray kGraph = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a fox"}},
    "gen": {"type": "fugue/generate-image", "version": "1.0.0", "inputs": {"prompt": {"from": "prompt.text"}}},
    "save": {"type": "fugue/save-image", "version": "1.0.0", "inputs": {"image": {"from": "gen.image"}}}
  }},
  "layout": {"nodes": {"prompt": {"pos": [0, 0]}, "gen": {"pos": [300, 0]}, "save": {"pos": [640, 0]}},
             "view": {"center": [400, 200], "zoom": 1}}
})";

} // namespace

class TestCanvasGroupWindow : public QObject {
    Q_OBJECT

private slots:
    void keysAndClicksMakeEnterAndLeaveGroups() {
        App app(load(kGraph));
        QVERIFY(app.open());
        // Ctrl+Shift+E with a sink selected asks first; Leave outside keeps it out.
        app.model.select({"gen", "save"});
        app.key(Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        QQuickItem *ask = nullptr;
        QTRY_VERIFY((ask = item(app.window->contentItem(), "groupSinksOutside")) && ask->isVisible());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, ask->mapToScene(QPointF(ask->width() / 2, ask->height() / 2)).toPoint());
        QTRY_COMPARE(app.model.document().groups.size(), 1);
        const QString instance = app.model.selectedIds().value(0);
        QVERIFY(app.model.isGroup(instance));
        QVERIFY(app.model.document().graph.nodes.contains("save"));

        // Tab enters the selected group; the breadcrumb shows; Escape leaves.
        app.key(Qt::Key_Tab);
        QTRY_VERIFY(app.model.inGroup());
        QQuickItem *crumbs = item(app.window->contentItem(), "breadcrumb");
        QVERIFY(crumbs && crumbs->isVisible());
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.model.inGroup());
        QVERIFY(!crumbs->isVisible());

        // A double-click on the group's body enters it; the breadcrumb's first crumb leaves.
        const QPointF body = app.model.position(instance) + QPointF(40, app.model.headerHeight() + 10);
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, app.screen(body.x(), body.y()));
        QTRY_VERIFY(app.model.inGroup());
        QQuickItem *top = nullptr;
        QTRY_VERIFY((top = item(app.window->contentItem(), "crumb0")) && top->isVisible());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, top->mapToScene(QPointF(top->width() / 2, top->height() / 2)).toPoint());
        QTRY_VERIFY(!app.model.inGroup());

        // Ctrl+Shift+G ungroups.
        app.model.select({instance});
        app.key(Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(app.model.document().groups.isEmpty());
        QVERIFY(app.model.document().graph.nodes.contains("gen"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #72: a selected group's ring goes around the whole stack, the second card 1u down and
    // right included, so it is accent on all four sides.
    void aSelectedGroupIsRingedAllAround() {
        App app(load(kGraph));
        QVERIFY(app.open());
        app.model.select({"prompt", "gen"});
        app.key(Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(app.model.document().groups.size(), 1);
        const QString instance = app.model.selectedIds().value(0);
        QVERIFY(app.model.isGroup(instance));
        app.workflows.dismissPendingTrust();
        app.workflows.dismissNotice();
        QTest::qWait(200);
        const NodeList *nodes = app.model.nodes();
        QRectF box;
        for (int r = 0; r < nodes->rowCount(); ++r)
            if (nodes->data(nodes->index(r), NodeList::NodeId).toString() == instance)
                box = QRectF(app.model.position(instance), QSizeF(nodes->data(nodes->index(r), NodeList::Width).toReal(),
                                                                   nodes->data(nodes->index(r), NodeList::Height).toReal()));
        QVERIFY(!box.isEmpty());
        const qreal u = app.model.unit();
        const QImage shot = test_support::grab(app.window);
        const QColor accent = app.theme.look()->property("accent").value<QColor>();
        const auto ring = [&](qreal x, qreal y) {
            const QColor c = shot.pixelColor(app.screen(x, y));
            return qAbs(c.red() - accent.red()) <= 2 && qAbs(c.green() - accent.green()) <= 2
                   && qAbs(c.blue() - accent.blue()) <= 2;
        };
        const qreal midX = box.center().x(), midY = box.center().y();
        QVERIFY(ring(box.left() - 3, midY));
        QVERIFY(ring(midX, box.top() - 3));
        QVERIFY2(ring(box.right() + u + 3, midY), "right side");
        QVERIFY2(ring(midX, box.bottom() + u + 3), "bottom side");
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aFrameMovesWithItsNodesAndAdvancedOpensOnClick() {
        App app(load(kGraph));
        QVERIFY(app.open());
        app.model.select({"prompt"});
        app.key(Qt::Key_G, Qt::ControlModifier);
        QTRY_COMPARE(app.model.frames().size(), 1);
        const QVariantMap frame = app.model.frames().first().toMap();
        const QPoint title = app.screen(frame.value("x").toReal() + 30, frame.value("y").toReal() + 8);
        app.drag(Qt::LeftButton, title, title + QPoint(0, 60));
        QTRY_COMPARE(app.model.position("prompt"), QPointF(0, 60));
        QCOMPARE(app.model.frames().first().toMap().value("y").toReal(), frame.value("y").toReal() + 60);
        QCOMPARE(app.model.position("gen"), QPointF(300, 0));

        // Generate Image's Advanced header: a click shows its inputs.
        QVariantMap header;
        for (int row = 0; row < app.model.nodes()->rowCount(); ++row) {
            const QModelIndex index = app.model.nodes()->index(row, 0);
            if (app.model.nodes()->data(index, NodeList::NodeId) != "gen")
                continue;
            for (const QVariant &port : app.model.nodes()->data(index, NodeList::Inputs).toList())
                if (port.toMap().value("header") == "Advanced")
                    header = port.toMap();
        }
        QVERIFY(!header.isEmpty());
        const QPointF at = app.model.position("gen") + QPointF(40, header.value("headerTop").toReal() + 6);
        QCOMPARE(app.model.advancedHeaderAt(at.x(), at.y()), QStringLiteral("gen"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(at.x(), at.y()));
        QTRY_VERIFY(app.model.scopeDocument().layout.value("nodes").toObject().value("gen").toObject().value("advanced").toBool());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }
};

QTEST_MAIN(TestCanvasGroupWindow)
#include "test_canvas_group_window.moc"
