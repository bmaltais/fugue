// Groups and Frames in the window (spec 5, 12.4): Ctrl+G frames, Ctrl+Shift+E groups (asking
// about sinks first), Tab and a double-click on the body enter, Escape and the breadcrumb leave,
// Ctrl+Shift+G ungroups, a Frame's title drags it with its nodes, and the Advanced header opens
// and folds. Driven by keys and the mouse on the real Main.qml, offscreen.

#include <QFont>
#include <QQuickStyle>

#include <functional>
#include <QtTest>

#include "canvas_app.h"
#include "engine/group_library.h"
#include "look.h"
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
    void initTestCase() { QQuickStyle::setStyle("Material"); } // as main() does
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

    // #55 and #72: F2 renames the selected node, P pins it (a drag leaves it), and a
    // double-click on a group card's port renames that port on the group's face.
    void renameAndPinFromTheWindow() {
        App app(load(kGraph));
        QVERIFY(app.open());
        app.workflows.dismissPendingTrust();
        app.workflows.dismissNotice();
        QQuickItem *ask = item(app.window->contentItem(), "renameAsk");
        auto answer = [&](const QString &text) {
            QQuickItem *field = nullptr;
            QTRY_VERIFY((field = item(app.window->contentItem(), "renameField")) && field->isVisible());
            QTRY_VERIFY(field->hasActiveFocus());
            field->setProperty("text", text);
            app.key(Qt::Key_Return);
        };
        // A title is edited in place on the header (#74): Enter keeps, Esc drops.
        auto inPlace = [&](const QString &text, Qt::Key key = Qt::Key_Return) {
            QQuickItem *field = nullptr;
            QTRY_VERIFY((field = item(app.window->contentItem(), "titleEditor")) && field->isVisible());
            QTRY_VERIFY(field->hasActiveFocus());
            QCOMPARE(field->property("selectedText").toString(), field->property("text").toString());
            field->setProperty("text", text);
            app.key(key);
            QTRY_VERIFY(!item(app.window->contentItem(), "titleEditor"));
        };
        app.model.select({"gen"});
        app.key(Qt::Key_F2);
        inPlace("Hero shot");
        QTRY_COMPARE(app.model.nodeTitle("gen"), QStringLiteral("Hero shot"));
        QVERIFY(!ask || !ask->isVisible());
        app.key(Qt::Key_F2);
        inPlace("Dropped", Qt::Key_Escape);
        QCOMPARE(app.model.nodeTitle("gen"), QStringLiteral("Hero shot"));

        app.key(Qt::Key_P);
        QTRY_VERIFY(app.model.selectionAll("pinned"));
        const QPointF at = app.model.position("gen") + QPointF(40, 10);
        app.drag(Qt::LeftButton, app.screen(at.x(), at.y()), app.screen(at.x() + 80, at.y() + 40));
        QCOMPARE(app.model.position("gen"), QPointF(300, 0));
        app.key(Qt::Key_P);
        QTRY_VERIFY(!app.model.selectionAll("pinned"));

        // A double-click on a node's title renames it too.
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, app.screen(at.x(), at.y()));
        inPlace("");
        QTRY_COMPARE(app.model.nodeTitle("gen"), QStringLiteral("Generate Image"));

        // A group card's output: double-click its port, and the face shows the new label.
        app.model.select({"prompt", "gen"});
        app.key(Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(app.model.document().groups.size(), 1);
        const QString instance = app.model.selectedIds().value(0);
        const QVariantMap target = app.model.portToRename(instance, "image", true);
        QVERIFY(!target.isEmpty());
        QPointF port;
        const NodeList *nodes = app.model.nodes();
        for (int r = 0; r < nodes->rowCount(); ++r)
            if (nodes->data(nodes->index(r), NodeList::NodeId).toString() == instance)
                for (const QVariant &p : nodes->data(nodes->index(r), NodeList::Outputs).toList())
                    if (p.toMap().value("name") == QLatin1String("image"))
                        port = app.model.position(instance)
                               + QPointF(nodes->data(nodes->index(r), NodeList::Width).toReal(), p.toMap().value("y").toReal());
        QVERIFY(!port.isNull());
        QTest::mouseDClick(app.window, Qt::LeftButton, {}, app.screen(port.x(), port.y()));
        answer("Picture");
        QTRY_COMPARE(app.model.portToRename(instance, "image", true).value("label").toString(), QStringLiteral("Picture"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #72: a Frame's title zooms with the canvas, as ComfyUI's group titles do, down to 10 px on
    // screen, where it stops so it still reads; it no longer keeps its full size when zoomed out.
    void aFrameTitleShrinksWithTheZoomToTenPixels() {
        App app(load(kGraph));
        QVERIFY(app.open());
        app.model.select({"prompt"});
        app.key(Qt::Key_G, Qt::ControlModifier);
        QTRY_COMPARE(app.model.frames().size(), 1);
        auto onScreen = [&] {
            QQuickItem *title = nullptr;
            std::function<void(QQuickItem *)> walk = [&](QQuickItem *from) {
                for (QQuickItem *child : from->childItems()) {
                    if (child->objectName() == QLatin1String("frameTitleText") && child->isVisible())
                        title = child;
                    walk(child);
                }
            };
            walk(app.window->contentItem());
            if (!title)
                return -1.0;
            return title->property("font").value<QFont>().pixelSize() * item(app.window->contentItem(), "world")->scale();
        };
        const qreal full = app.theme.look()->property("frameTitleSize").toReal();
        QTRY_COMPARE(onScreen(), full);
        const QVariantMap frame = app.model.frames().first().toMap();
        app.model.setView(frame.value("x").toReal(), frame.value("y").toReal(), 0.7);
        QMetaObject::invokeMethod(app.canvas, "applyView");
        QTRY_VERIFY2(qAbs(onScreen() - full * 0.7) < 1, qPrintable(QString::number(onScreen()) + " at " + QString::number(item(app.window->contentItem(), "world")->scale())));
        app.model.setView(frame.value("x").toReal(), frame.value("y").toReal(), 0.3);
        QMetaObject::invokeMethod(app.canvas, "applyView");
        QTRY_VERIFY2(qAbs(onScreen() - 10) < 1, qPrintable(QString::number(onScreen())));
    }

    // #74: selected text reads in every field: the interface editor's fields draw their selection
    // in the theme's colors, its text at 4.5:1 on it (not Material's white on the pale accent).
    void selectedTextReadsInEveryField() {
        App app(load(kGraph));
        QVERIFY(app.open());
        app.model.select({"gen"});
        app.key(Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(app.model.document().groups.size(), 1);
        QVERIFY(app.commands.trigger("group.interface"));
        QTRY_VERIFY(app.model.inGroup());
        int fields = 0;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *from) {
            for (QQuickItem *child : from->childItems()) {
                const QVariant selection = child->property("selectionColor");
                // One nobody can select in does not count: read-only, or the label of a ComboBox
                // that is not editable.
                const QVariant editable = from->property("editable");
                const bool selectable = (!child->property("readOnly").toBool() || child->property("selectByMouse").toBool())
                                        && (!editable.isValid() || editable.toBool());
                if (child->isVisible() && selectable && selection.isValid()
                    && child->property("selectedTextColor").isValid()) {
                    ++fields;
                    const double ratio = fugue::look::contrast(child->property("selectedTextColor").value<QColor>(),
                                                               selection.value<QColor>());
                    QVERIFY2(ratio >= 4.5, qPrintable(QString("%1 %2: %3").arg(child->metaObject()->className(),
                                                                              child->objectName()).arg(ratio)));
                }
                walk(child);
            }
        };
        QQuickItem *root = app.window->contentItem();
        while (root->parentItem())
            root = root->parentItem();
        // The editor opens with Material's fade: its fields are there once it has.
        QTRY_VERIFY((fields = 0, walk(root), fields >= 3));
        fields = 0;
        walk(root);
        QVERIFY2(fields >= 3, qPrintable(QString::number(fields)));
    }

    // #74: a dialog's highlighted button (Rename, Save) reads: its label at 4.5:1 on its fill,
    // not Material's white on the pale accent.
    void aHighlightedButtonReads() {
        if (!QFileInfo::exists("/usr/share/omarchy/themes/kanagawa"))
            QSKIP("Omarchy's themes are not installed here");
        App app(load(kGraph), {}, Look{"kanagawa"}); // a pale accent (#dcd7ba), as the owner's
        QVERIFY(app.open());
        // The port rename question (a title is renamed in place).
        QMetaObject::invokeMethod(&app.model, "portRenameRequested", Q_ARG(QString, "gen"), Q_ARG(QString, "image"),
                                  Q_ARG(bool, true));
        QQuickItem *ok = nullptr;
        QTRY_VERIFY((ok = item(app.window->contentItem(), "renameOk")) && ok->isVisible());
        // Its label is the theme's accent foreground, made to read on the accent the window gives
        // Material for a highlighted button's fill (Main.qml: Material.accent).
        auto *label = ok->property("contentItem").value<QQuickItem *>();
        QVERIFY(label);
        const CanvasLook *look = app.theme.look();
        QCOMPARE(label->property("color").value<QColor>(), look->property("accentForeground").value<QColor>());
        QVERIFY(fugue::look::contrast(look->property("accentForeground").value<QColor>(),
                                      look->property("accent").value<QColor>()) >= 4.5);
        app.key(Qt::Key_Escape);
    }

    // #74: the group card's footer has an Enter button, as ComfyUI's "Enter subgraph".
    void theGroupCardsEnterButtonEntersIt() {
        App app(load(kGraph));
        QVERIFY(app.open());
        app.model.select({"prompt", "gen"});
        app.key(Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(app.model.document().groups.size(), 1);
        app.workflows.dismissPendingTrust();
        app.workflows.dismissNotice();
        QQuickItem *enter = nullptr;
        QTRY_VERIFY((enter = item(app.window->contentItem(), "enterGroupButton")) && enter->isVisible());
        QTest::qWait(100);
        QTest::mouseClick(app.window, Qt::LeftButton, {},
                          enter->mapToScene(QPointF(enter->width() / 2, enter->height() / 2)).toPoint());
        QTRY_VERIFY(app.model.inGroup());
    }

    // #74: renaming a library group's card in place asks whether to rename it in the library too.
    void renamingALibraryGroupsCardOffersTheLibrary() {
        App app(load(kGraph));
        QVERIFY(app.open());
        app.workflows.dismissPendingTrust();
        app.workflows.dismissNotice();
        app.model.select({"prompt", "gen"});
        app.key(Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(app.model.document().groups.size(), 1);
        const QString instance = app.model.selectedIds().value(0);
        QVERIFY2(app.model.saveGroupToLibrary(), qPrintable(app.model.groupProblem()));
        const QString library = fugue::libraryDir();
        QVERIFY(QFileInfo(library + "/group/node.json").isFile());
        app.model.select({instance});
        app.key(Qt::Key_F2);
        QQuickItem *field = nullptr;
        QTRY_VERIFY((field = item(app.window->contentItem(), "titleEditor")) && field->hasActiveFocus());
        field->setProperty("text", QStringLiteral("Fox pass"));
        app.key(Qt::Key_Return);
        QQuickItem *yes = nullptr;
        QTRY_VERIFY((yes = item(app.window->contentItem(), "libraryRenameYes")) && yes->isVisible());
        QTest::qWait(100);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, yes->mapToScene(QPointF(yes->width() / 2, yes->height() / 2)).toPoint());
        QTRY_VERIFY(QFileInfo(library + "/fox-pass/node.json").isFile());
        QVERIFY(!QFileInfo(library + "/group").exists());
        QDir(library).removeRecursively();
    }

    // #74: a double-click on a Frame's title edits it in place.
    void aFrameTitleIsEditedInPlace() {
        App app(load(kGraph));
        QVERIFY(app.open());
        app.model.select({"prompt"});
        app.key(Qt::Key_G, Qt::ControlModifier);
        QTRY_COMPARE(app.model.frames().size(), 1);
        const QVariantMap frame = app.model.frames().first().toMap();
        QTest::mouseDClick(app.window, Qt::LeftButton, {},
                           app.screen(frame.value("x").toReal() + 30, frame.value("y").toReal() + 8));
        QQuickItem *field = nullptr;
        QTRY_VERIFY((field = item(app.window->contentItem(), "titleEditor")) && field->hasActiveFocus());
        field->setProperty("text", QStringLiteral("Inputs"));
        app.key(Qt::Key_Return);
        QTRY_COMPARE(app.model.frames().first().toMap().value("title").toString(), QStringLiteral("Inputs"));
        QVERIFY(!item(app.window->contentItem(), "titleEditor"));
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
