// The menu bar and the context menus in the real window (#50, Main.qml offscreen): each top
// menu opens with the mouse and with Alt+letter, the keys move through them, items run their
// commands with the registry's state, the hamburger takes over when narrow, and right-clicks
// open the canvas, node and Frame menus with their colors.

#include <QClipboard>
#include <QImage>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QQuickStyle>
#include <QSignalSpy>
#include <QUndoStack>

#include "canvas_app.h"
#include "menu/menu_commands.h"
#include "menu/menus.h"

using namespace fugue;
using canvas_test::App;
using canvas_test::load;

namespace {

// Two Text nodes inside a Frame, and a third outside it; the view at zoom 1.
const char *framed = R"({
  "fugue": 1,
  "graph": {"nodes": {
    "a": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "fox"}},
    "b": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "snow"}},
    "c": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "far"}}
  }},
  "layout": {"nodes": {"a": {"pos": [40, 80]}, "b": {"pos": [40, 260]}, "c": {"pos": [600, 80]}},
             "frames": [{"id": "f1", "title": "Inputs", "rect": [0, 0, 340, 480]}],
             "view": {"center": [480, 300], "zoom": 1}}
})";

// One Text node, far to the lower right of the view's centre.
const char *nearCorner = R"({
  "fugue": 1,
  "graph": {"nodes": {"c": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "far"}}}},
  "layout": {"nodes": {"c": {"pos": [640, 280]}}, "view": {"center": [480, 300], "zoom": 1}}
})";

// A visible item by name, in the window's items (the overlay where menus draw included).
QQuickItem *visibleItem(QQuickItem *from, const QString &name) {
    for (QQuickItem *child : from->childItems()) {
        if (!child->isVisible())
            continue;
        if (child->objectName() == name)
            return child;
        if (QQuickItem *found = visibleItem(child, name))
            return found;
    }
    return nullptr;
}

QQuickItem *find(QQuickWindow *window, const QString &name) {
    QQuickItem *root = window->contentItem();
    while (root->parentItem())
        root = root->parentItem();
    return visibleItem(root, name);
}

QPoint centerOf(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

QObject *popup(QQuickWindow *window, const QString &name) { return window->findChild<QObject *>(name); }

bool isOpen(QQuickWindow *window, const QString &name) {
    QObject *menu = popup(window, name);
    return menu && menu->property("visible").toBool();
}

// The menu's current row (Up and Down move it), by name.
QString currentRow(QQuickWindow *window, const QString &name) {
    QObject *menu = popup(window, name);
    QQuickItem *item = nullptr;
    QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, item),
                              Q_ARG(int, menu->property("currentIndex").toInt()));
    return item ? item->objectName() : QString();
}

// No menu is showing.
bool allClosed(QQuickWindow *window) {
    return !isOpen(window, "menuDropdown") && !isOpen(window, "menuEverything") && !isOpen(window, "contextMenuPopup");
}

// A popup's rectangle in the window: its x, y, width and height in its parent.
QRectF popupRect(QObject *menu) {
    auto *parent = menu->property("parent").value<QQuickItem *>();
    const QPointF at = parent->mapToScene(QPointF(menu->property("x").toReal(), menu->property("y").toReal()));
    return QRectF(at, QSizeF(menu->property("width").toReal(), menu->property("height").toReal()));
}

QRectF sceneRect(QQuickItem *item) { return QRectF(item->mapToScene(QPointF(0, 0)), QSizeF(item->width(), item->height())); }

QString rectText(const QRectF &r) { return QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()); }

// The menu is wholly inside the window.
bool insideWindow(QQuickWindow *window, QObject *menu) {
    return QRectF(0, 0, window->width(), window->height()).contains(popupRect(menu));
}

QQuickItem *rowAt(QObject *menu, int index) {
    QQuickItem *item = nullptr;
    QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, index));
    return item;
}

// A visible item that must be there.
#define NEED(name) [&] { QQuickItem *found = find(app.window, name); if (!found) qFatal("no visible %s", qPrintable(QString(name))); return found; }()

} // namespace

class MenusTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void eachTopMenuOpensWithTheMouseAndItsAltKey() {
        App app(load(framed));
        QVERIFY(app.open());
        const QVariantList bar = app.commands.menu("bar");
        QCOMPARE(bar.size(), 6);
        for (int i = 0; i < bar.size(); ++i) {
            const QString title = bar[i].toMap().value("title").toString();
            QQuickItem *button = find(app.window, QStringLiteral("menuTitle-%1").arg(i));
            QVERIFY2(button, qPrintable(title));
            QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(button));
            QTRY_VERIFY2(isOpen(app.window, "menuDropdown"), qPrintable(title));
            // Its first command that can run is a row of the open menu (the first one, when none
            // can: this harness has no runs, so Run has none).
            QString first;
            for (const QVariant &row : bar[i].toMap().value("items").toList())
                if (first.isEmpty() && row.toMap().value("enabled").toBool())
                    first = row.toMap().value("command").toString();
            if (first.isEmpty())
                first = bar[i].toMap().value("items").toList().first().toMap().value("command").toString();
            QVERIFY2(find(app.window, "menuItem-" + first), qPrintable(first));
            // A second press on the title closes it.
            QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(button));
            QTRY_VERIFY2(!isOpen(app.window, "menuDropdown"), qPrintable(title));

            const QChar letter = title.at(title.indexOf('&') + 1).toUpper();
            QTest::keyClick(app.window, letter.toLatin1(), Qt::AltModifier);
            QTRY_VERIFY2(isOpen(app.window, "menuDropdown"), qPrintable(title));
            QTRY_COMPARE(currentRow(app.window, "menuDropdown"), "menuItem-" + first);
            QVERIFY(app.commands.menuOpen());
            QTest::keyClick(app.window, Qt::Key_Escape);
            QTRY_VERIFY2(!isOpen(app.window, "menuDropdown"), qPrintable(title));
        }
        QVERIFY(!app.commands.menuOpen());
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theKeysWalkTheMenusAndRunACommand() {
        App app(load(framed));
        QVERIFY(app.open());
        app.model.select({"c"});
        // F10 opens the first menu; Right and Left move along the bar.
        app.key(Qt::Key_F10);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QTRY_COMPARE(currentRow(app.window, "menuDropdown"), QString("menuItem-file.new"));
        app.key(Qt::Key_Right); // Edit, on its first row that can run (nothing to undo yet)
        QTRY_COMPARE(currentRow(app.window, "menuDropdown"), QString("menuItem-edit.copy"));
        app.key(Qt::Key_Left);
        QTRY_COMPARE(currentRow(app.window, "menuDropdown"), QString("menuItem-file.new"));
        // Run has nothing that can run here (this harness has no runs): Left and Right still move on.
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app.window, "menuDropdown"));
        QTest::keyClick(app.window, 'R', Qt::AltModifier);
        QTRY_COMPARE(NEED("menuBar")->property("current").toInt(), 4);
        QTRY_VERIFY(NEED("menuItem-run.start"));
        app.key(Qt::Key_Right);
        QTRY_COMPARE(currentRow(app.window, "menuDropdown"), QString("menuItem-app.shortcuts"));
        app.key(Qt::Key_Right); // wraps to File
        QTRY_COMPARE(currentRow(app.window, "menuDropdown"), QString("menuItem-file.new"));
        app.key(Qt::Key_Left); // wraps to Help
        QTRY_COMPARE(currentRow(app.window, "menuDropdown"), QString("menuItem-app.shortcuts"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app.window, "menuDropdown"));

        // Edit > Delete with the keys: Down until it is current, then Enter.
        QTest::keyClick(app.window, 'E', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        for (int i = 0; i < 12 && currentRow(app.window, "menuDropdown") != "menuItem-edit.delete"; ++i)
            app.key(Qt::Key_Down);
        QCOMPARE(currentRow(app.window, "menuDropdown"), QString("menuItem-edit.delete"));
        app.key(Qt::Key_Return);
        QTRY_VERIFY(!isOpen(app.window, "menuDropdown"));
        QCOMPARE(app.model.nodes()->rowCount(), 2);

        // Undo is enabled now, and the mouse runs it.
        QTest::keyClick(app.window, 'E', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QQuickItem *undo = find(app.window, "menuItem-edit.undo");
        QVERIFY(undo && undo->isEnabled());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(undo));
        QTRY_COMPARE(app.model.nodes()->rowCount(), 3);
        QTRY_VERIFY(!isOpen(app.window, "menuDropdown"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void itemsShowTheRegistrysState() {
        App app(load(framed));
        QVERIFY(app.open());
        QTest::keyClick(app.window, 'E', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        // Nothing selected: Delete is off. Rename (#55) is not built yet: off, at half opacity.
        // Settings (#51) is.
        QVERIFY(!NEED("menuItem-edit.delete")->isEnabled());
        QQuickItem *rename = find(app.window, "menuItem-edit.rename");
        QVERIFY(rename && !rename->isEnabled());
        QVERIFY(rename->opacity() <= 0.5);
        QVERIFY(NEED("menuItem-app.settings")->isEnabled());
        QCOMPARE(NEED("menuItem-edit.undo")->property("text").toString(), QString("Undo"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app.window, "menuDropdown"));
        // Opened again after a selection, the state is fresh.
        app.model.select({"a"});
        QTest::keyClick(app.window, 'E', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QVERIFY(NEED("menuItem-edit.delete")->isEnabled());
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(allClosed(app.window));

        // View > Show links is a toggle with the links' state.
        QTest::keyClick(app.window, 'V', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QQuickItem *links = NEED("menuItem-view.toggle-links");
        QVERIFY(links->property("checkable").toBool() && links->property("checked").toBool());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(links));
        QTRY_VERIFY(app.model.linksHidden());
        QTest::keyClick(app.window, 'V', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QVERIFY(!NEED("menuItem-view.toggle-links")->property("checked").toBool());
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(allClosed(app.window));

        // Help > Documentation opens the docs.
        QTest::keyClick(app.window, 'H', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("menuItem-help.docs")));
        QTRY_COMPARE(app.openedUrls.size(), 1);
        QVERIFY(app.openedUrls.first().toString().contains("github.com/bmaltais/fugue"));
        // Help > About Fugue opens a card, and Esc closes it.
        QTRY_VERIFY(allClosed(app.window));
        QTest::keyClick(app.window, 'H', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("menuItem-help.about")));
        QTRY_VERIFY(isOpen(app.window, "about"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app.window, "about"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #72: a context menu offers what applies to what was clicked. Commands for a group, a port
    // or an image are left out of a plain node's menu, as are commands not built yet; the menu
    // bar keeps them all, off. Separators never start, end or double up.
    void contextMenusOfferOnlyWhatApplies() {
        App app(load(framed));
        QVERIFY(app.open());
        auto ids = [](const QVariantList &items) {
            QStringList list;
            for (const QVariant &item : items)
                list << (item.toMap().value("separator").toBool() ? QStringLiteral("|")
                                                                    : item.toMap().value("command").toString());
            return list;
        };
        auto tidy = [](const QStringList &list) {
            return !list.isEmpty() && list.first() != "|" && list.last() != "|"
                   && !list.join(',').contains(QStringLiteral("|,|"));
        };
        app.model.select({"c"});
        QStringList node = ids(app.commands.menu("node", {{"node", "c"}}));
        for (const char *off : {"group.ungroup", "group.enter", "group.make-unique", "group.update", "group.expose",
                                "group.unexpose", "node.open-image", "node.copy-image", "node.save-image"})
            QVERIFY2(!node.contains(QLatin1String(off)), off);
        for (const QVariant &item : app.commands.menu("node", {{"node", "c"}}))
            QVERIFY2(!item.toMap().contains("command") || item.toMap().value("available").toBool(),
                     qPrintable(item.toMap().value("command").toString()));
        QVERIFY2(tidy(node), qPrintable(node.join(',')));
        QStringList canvas = ids(app.commands.menu("canvas", {}));
        QVERIFY(!canvas.contains("group.leave") && !canvas.contains("group.interface"));
        QVERIFY2(tidy(canvas), qPrintable(canvas.join(',')));

        // A group: Ungroup and Enter are there; Make unique only once another instance shares it.
        app.model.select({"a", "b"});
        const QString group = app.model.makeGroup();
        QVERIFY(app.model.isGroup(group));
        app.model.select({group});
        node = ids(app.commands.menu("node", {{"node", group}}));
        QVERIFY(node.contains("group.ungroup") && node.contains("group.enter"));
        QVERIFY(!node.contains("group.make-unique"));
        QVERIFY(app.commands.trigger("edit.clone"));
        QCOMPARE(app.model.selectedIds().size(), 1);
        node = ids(app.commands.menu("node", {{"node", app.model.selectedIds().first()}}));
        QVERIFY2(node.contains("group.make-unique"), qPrintable(node.join(',')));
        // Inside it, the canvas menu leaves it.
        QVERIFY(app.model.enterGroup(group));
        QVERIFY(ids(app.commands.menu("canvas", {})).contains("group.leave"));
        QVERIFY(app.model.leaveGroup());

        // The menu bar keeps every command, off where it does not apply.
        app.model.select({"c"});
        bool listed = false;
        for (const QVariant &top : app.commands.menu("bar"))
            for (const QVariant &item : top.toMap().value("items").toList())
                if (item.toMap().value("command") == QLatin1String("group.enter")) {
                    listed = true;
                    QVERIFY(!item.toMap().value("enabled").toBool());
                }
        QVERIFY(listed);
    }

    // #74: a toolbar over the selection, as ComfyUI's: its common commands, Enter on a group,
    // and "…" for the node's whole menu.
    void aToolbarSitsOverTheSelection() {
        App app(load(framed));
        QVERIFY(app.open());
        QVERIFY(!find(app.window, "selectionToolbar"));
        app.model.select({"c"});
        QQuickItem *bar = nullptr;
        QTRY_VERIFY((bar = find(app.window, "selectionToolbar")) != nullptr);
        // Above the node, inside the window.
        const QRectF node = app.model.nodeRect("c");
        QVERIFY(bar->mapToScene(QPointF(0, bar->height())).y() <= app.screen(node.x(), node.y()).y());
        for (const char *button : {"toolbar-node.bypass", "toolbar-colors", "toolbar-edit.delete", "toolbar-more"})
            QVERIFY2(find(app.window, button), button);
        QVERIFY(!find(app.window, "toolbar-group.enter")); // not a group
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("toolbar-node.bypass")));
        QTRY_COMPARE(app.model.document().graph.nodes.value("c").mode, NodeMode::Bypass);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("toolbar-more")));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QCOMPARE(popup(app.window, "contextMenu")->property("kind").toString(), QString("node"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(allClosed(app.window));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("toolbar-colors")));
        QTRY_VERIFY(find(app.window, "menuItem-node.color.red"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("menuItem-node.color.red")));
        QTRY_COMPARE(app.model.document().layout.value("nodes").toObject().value("c").toObject().value("color").toString(),
                     QStringLiteral("red"));
        // A group's toolbar enters it; none while nothing is selected.
        app.model.select({"a", "b"});
        const QString group = app.model.makeGroup();
        app.model.select({group});
        QTRY_VERIFY(find(app.window, "toolbar-group.enter"));
        QTest::qWait(50); // the row places it
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("toolbar-group.enter")));
        QTRY_VERIFY(app.model.inGroup());
        app.model.select({});
        QTRY_VERIFY(!find(app.window, "selectionToolbar"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // #55, #74: Rename on a node's context menu edits its title in place.
    void renameFromTheContextMenu() {
        App app(load(framed));
        QVERIFY(app.open());
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(620, 90));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("menuItem-edit.rename")));
        QQuickItem *field = nullptr;
        QTRY_VERIFY((field = find(app.window, "titleEditor")) != nullptr);
        QCOMPARE(field->property("text").toString(), app.model.nodeTitle("c"));
        field->setProperty("text", QStringLiteral("Far away"));
        // A click elsewhere keeps the title (#74).
        QTest::mouseClick(app.window, Qt::LeftButton, {}, app.screen(500, 500));
        QTRY_COMPARE(app.model.nodeTitle("c"), QStringLiteral("Far away"));
    }

    // #72: a row that is off looks off (dimmed), not merely a little paler than one that is on.
    void aDisabledRowIsDimmed() {
        App app(load(framed));
        QVERIFY(app.open());
        QTest::keyClick(app.window, 'E', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QQuickItem *redo = NEED("menuItem-edit.redo");
        QVERIFY(!redo->isEnabled());
        QVERIFY(redo->opacity() <= 0.5);
        QCOMPARE(NEED("menuItem-app.settings")->opacity(), 1.0);
        app.key(Qt::Key_Escape);
    }

    // #72: commands are on only when they can act. Run to here needs exactly one node; Run, Run
    // first and the exports a node at all; Paste a workflow or an image on the clipboard, not
    // any text.
    void commandsAreOnOnlyWhenTheyCanAct() {
        App app(load(framed));
        QVERIFY(app.open());
        for (const char *id : {"run.to-here", "run.start", "run.start-first", "file.export-share", "file.export-compiled"})
            if (!app.commands.isEnabled(QLatin1String(id)) && !app.commands.entry(QLatin1String(id)).value("available").toBool())
                app.commands.setHandler(QLatin1String(id), [] {});
        app.model.select({"a"});
        QVERIFY(app.commands.isEnabled("run.to-here"));
        app.model.select({"a", "b"});
        QVERIFY(!app.commands.isEnabled("run.to-here"));
        for (const char *id : {"run.start", "run.start-first", "file.export-share", "file.export-compiled"})
            QVERIFY2(app.commands.isEnabled(QLatin1String(id)), id);
        app.model.selectAll();
        app.model.deleteSelection();
        for (const char *id : {"run.start", "run.start-first", "file.export-share", "file.export-compiled"})
            QVERIFY2(!app.commands.isEnabled(QLatin1String(id)), id);

        QClipboard *clipboard = QGuiApplication::clipboard();
        clipboard->setText(QStringLiteral("just some words"));
        QVERIFY(!app.commands.isEnabled("edit.paste"));
        QVERIFY(!app.commands.isEnabled("edit.paste-connected"));
        clipboard->setText(QStringLiteral("{\"fugue\": 1, \"graph\": {\"nodes\": {}}}"));
        QVERIFY(app.commands.isEnabled("edit.paste"));
        QImage image(4, 4, QImage::Format_RGB32);
        image.fill(Qt::red);
        clipboard->setImage(image);
        QVERIFY(app.commands.isEnabled("edit.paste"));
        QVERIFY(!app.commands.isEnabled("edit.paste-connected"));
    }

    void bareKeysBelongToAnOpenMenu() {
        App app(load(framed));
        QVERIFY(app.open());
        app.commands.setUnsavedWork(true);
        QSignalSpy quit(&app.commands, &CommandRegistry::quitConfirmationRequested);
        QTest::keyClick(app.window, 'F', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        app.key(Qt::Key_Q); // app.quit's key, asleep while the menu is open
        app.key(Qt::Key_L); // view.toggle-links
        QCOMPARE(quit.size(), 0);
        QVERIFY(!app.model.linksHidden());
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app.window, "menuDropdown"));
        app.key(Qt::Key_Q);
        QTRY_COMPARE(quit.size(), 1);
    }

    void aNarrowWindowHasOneMenuButton() {
        App app(load(framed));
        QVERIFY(app.open());
        QVERIFY(!find(app.window, "menuButton"));
        app.window->resize(180, app.window->height());
        QTRY_VERIFY(find(app.window, "menuButton"));
        QVERIFY(!find(app.window, "menuTitle-0"));
        // It opens every menu as a submenu.
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("menuButton")));
        QTRY_VERIFY(isOpen(app.window, "menuEverything"));
        QVERIFY(find(app.window, "subMenu-File"));
        QVERIFY(find(app.window, "subMenu-Help"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app.window, "menuEverything"));
        // Alt+letter opens that menu under the button.
        QTest::keyClick(app.window, 'V', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QVERIFY(find(app.window, "menuItem-view.fit"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app.window, "menuDropdown"));
        // F10 opens the menu button's menu.
        app.key(Qt::Key_F10);
        QTRY_VERIFY(isOpen(app.window, "menuEverything"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(allClosed(app.window));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void rightClickOpensTheMenuOfWhatIsUnderIt() {
        App app(load(framed));
        QVERIFY(app.open());
        auto *menu = popup(app.window, "contextMenu");
        QVERIFY(menu);

        // Empty canvas: the canvas menu; Add node opens search where it was opened.
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(500, 500));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QCOMPARE(menu->property("kind").toString(), QString("canvas"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("menuItem-node.search")));
        QTRY_VERIFY(app.model.search()->property("active").toBool());
        QCOMPARE(app.model.search()->property("point").toPointF(), QPointF(500, 500));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.model.search()->property("active").toBool());

        // A node outside the selection: selected first, then its menu; Bypass acts on it.
        app.model.select({"a"});
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(620, 90));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QCOMPARE(menu->property("kind").toString(), QString("node"));
        QCOMPARE(app.model.selectedIds(), QStringList{"c"});
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("menuItem-node.bypass")));
        QTRY_COMPARE(app.model.document().graph.nodes.value("c").mode, NodeMode::Bypass);
        // Inside the selection, the selection stays.
        app.model.select({"a", "b"});
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(60, 90));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QCOMPARE(app.model.selectedIds().size(), 2);
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(app.window, "contextMenuPopup"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // A menu closes at once, so the next click or key is not lost to it: with Material's
    // 220 ms closing fade, popup() on the closing menu did nothing and the fade then hid it,
    // which lost a right-click made just after choosing a row (and made
    // rightClickOpensTheMenuOfWhatIsUnderIt fail on a busy machine), and keys went to it.
    void aMenuReopensRightAfterItCloses() {
        App app(load(framed));
        QVERIFY(app.open());
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(620, 90));
        QTRY_VERIFY(popup(app.window, "contextMenuPopup")->property("opened").toBool());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("menuItem-node.bypass")));
        QVERIFY(!popup(app.window, "contextMenuPopup")->property("opened").toBool());
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(60, 90));
        QTRY_VERIFY(popup(app.window, "contextMenuPopup")->property("opened").toBool());
        QCOMPARE(popup(app.window, "contextMenu")->property("kind").toString(), QString("node"));
        QVERIFY(find(app.window, "menuItem-node.bypass"));

        // The menu bar: a press on the open title closes it, a press on the next one at once
        // opens that one.
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(allClosed(app.window));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("menuTitle-0")));
        QTRY_VERIFY(popup(app.window, "menuDropdown")->property("opened").toBool());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("menuTitle-0")));
        QVERIFY(!popup(app.window, "menuDropdown")->property("opened").toBool());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("menuTitle-2")));
        QTRY_VERIFY(popup(app.window, "menuDropdown")->property("opened").toBool());
        QVERIFY(find(app.window, "menuItem-view.fit"));
        // Keys too: Escape, then at once Alt+H opens Help.
        app.key(Qt::Key_Escape);
        QTest::keyClick(app.window, 'H', Qt::AltModifier);
        QTRY_VERIFY(popup(app.window, "menuDropdown")->property("opened").toBool());
        QTRY_VERIFY(find(app.window, "menuItem-app.shortcuts"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(allClosed(app.window));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void colorsApplyToTheSelectionOrTheFrameAsOneStep() {
        App app(load(framed));
        QVERIFY(app.open());
        app.model.select({"a", "b"});
        const int steps = app.model.undoStack()->count();
        // Node menu > Colors > Red, with the mouse.
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(60, 90));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QQuickItem *colors = find(app.window, "subMenu-Colors");
        QVERIFY(colors);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(colors));
        QTRY_VERIFY(find(app.window, "menuItem-node.color.red"));
        QQuickItem *red = find(app.window, "menuItem-node.color.red");
        QTRY_VERIFY(red->isVisible() && red->width() > 0);
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(red));
        auto color = [&](const QString &id) {
            return app.model.document().layout.value("nodes").toObject().value(id).toObject().value("color").toString();
        };
        QTRY_COMPARE(color("a"), QString("red"));
        QCOMPARE(color("b"), QString("red"));
        QCOMPARE(color("c"), QString());
        QCOMPARE(app.model.undoStack()->count(), steps + 1);
        app.model.undo();
        QCOMPARE(color("a"), QString());
        QCOMPARE(color("b"), QString());

        // On the Frame's empty part: the Frame menu, its colors go to the Frame alone, not
        // to the selection the menu keeps.
        app.model.select({"c"});
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(320, 30));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QCOMPARE(popup(app.window, "contextMenu")->property("kind").toString(), QString("frame"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("subMenu-Colors")));
        QTRY_VERIFY(find(app.window, "menuItem-node.color.blue"));
        QQuickItem *blue = find(app.window, "menuItem-node.color.blue");
        QVERIFY(blue->isEnabled());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(blue));
        QTRY_COMPARE(app.model.frames().first().toMap().value("color").toString(), QString("blue"));
        QCOMPARE(color("c"), QString());
        // "No color" takes it off again.
        applyLayoutColor(app.model, "none", "f1");
        QCOMPARE(app.model.frames().first().toMap().value("color").toString(), QString());

        // Each color is a command, so a key can be bound to it.
        app.model.select({"c"});
        QVERIFY(app.commands.run("node.color.green"));
        QCOMPARE(color("c"), QString("green"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // Pictures of the menus for a look at them, not a check:
    //   FUGUE_MENU_SHOTS=/tmp/shots QT_QUICK_BACKEND=rhi build/test_menus screenshots
    void toggleLabelsNameWhatTheCommandWillDo() {
        App app(load(framed));
        QVERIFY(app.open());
        const auto label = [&](const char *command) {
            return app.commands.entry(command).value("label").toString();
        };
        // Nothing selected, or nothing in the state: the plain verbs.
        QCOMPARE(label("node.bypass"), QString("Bypass"));
        app.model.select({"c"});
        QCOMPARE(label("node.bypass"), QString("Bypass"));
        QCOMPARE(label("node.mute"), QString("Mute"));
        QCOMPARE(label("node.collapse"), QString("Collapse"));
        // The `?` overlay keeps the neutral name.
        QCOMPARE(app.commands.entry("node.bypass").value("description").toString(), QString("Bypass"));

        // A bypassed node: the context menu and the Node menu offer to undo it.
        app.model.toggleMode("bypass");
        app.model.toggleCollapsed();
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(620, 90));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QCOMPARE(NEED("menuItem-node.bypass")->property("text").toString(), QString("Remove bypass"));
        QCOMPARE(NEED("menuItem-node.mute")->property("text").toString(), QString("Mute"));
        QCOMPARE(NEED("menuItem-node.collapse")->property("text").toString(), QString("Expand"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(allClosed(app.window));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("menuTitle-3")));
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QCOMPARE(NEED("menuItem-node.bypass")->property("text").toString(), QString("Remove bypass"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(allClosed(app.window));

        // Muted reads Unmute; a mixed selection says what the command does: apply to all.
        app.model.toggleMode("bypass");
        app.model.toggleMode("mute");
        QCOMPARE(label("node.mute"), QString("Unmute"));
        QCOMPARE(label("node.bypass"), QString("Bypass"));
        app.model.select({"a", "c"});
        QCOMPARE(label("node.mute"), QString("Mute"));
        app.model.toggleMode("mute");
        QCOMPARE(label("node.mute"), QString("Unmute"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void screenshots_data() {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<int>("radius");
        QTest::newRow("ethereal r0") << "ethereal" << 0;
        QTest::newRow("flexoki-light r8") << "flexoki-light" << 8;
    }
    void screenshots() {
        QFETCH(QString, theme);
        QFETCH(int, radius);
        const QString out = qEnvironmentVariable("FUGUE_MENU_SHOTS");
        if (out.isEmpty())
            QSKIP("set FUGUE_MENU_SHOTS to a folder to take the screenshots");
        if (!QFile::exists("/usr/share/omarchy/themes/" + theme))
            QSKIP("Omarchy's themes are not installed here");
        App app(load(framed), {}, canvas_test::Look{theme, radius});
        QTRY_COMPARE(app.theme.radius(), radius);
        QVERIFY(app.open());
        applyLayoutColor(app.model, "blue", "f1");
        app.model.select({"c"});
        applyLayoutColor(app.model, "red");
        const QString name = QStringLiteral("%1/%2-r%3").arg(out, theme).arg(radius);
        QTest::keyClick(app.window, 'E', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QTest::mouseMove(app.window, centerOf(NEED("subMenu-Colors")));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("subMenu-Colors")));
        QTRY_VERIFY(find(app.window, "menuItem-node.color.red"));
        QTest::qWait(400);
        QVERIFY(test_support::grab(app.window).save(name + "-edit.png"));
        app.key(Qt::Key_Escape);
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(allClosed(app.window));
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(620, 90));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QTest::mouseMove(app.window, centerOf(NEED("menuItem-node.mute")));
        QTest::qWait(400);
        QVERIFY(test_support::grab(app.window).save(name + "-node.png"));
    }

    // Down from the first row to the last: each current row is wholly inside the window and
    // inside the menu (a capped menu scrolls it into view).
    static void everyRowIsReachable(App &app, const char *name) {
        QObject *menu = popup(app.window, name);
        const int count = menu->property("count").toInt();
        QVERIFY(count > 2);
        for (int step = 0; step < count + 2; ++step) {
            const int at = menu->property("currentIndex").toInt();
            if (at >= 0 && rowAt(menu, at)->isEnabled()) {
                QQuickItem *row = rowAt(menu, at);
                QQuickItem *content = menu->property("contentItem").value<QQuickItem *>();
                QTRY_VERIFY2(QRectF(0, 0, app.window->width(), app.window->height()).contains(sceneRect(row)),
                             qPrintable(row->objectName() + " " + rectText(sceneRect(row))));
                QTRY_VERIFY2(sceneRect(content).adjusted(-1, -1, 1, 1).contains(sceneRect(row)),
                             qPrintable(row->objectName() + " " + rectText(sceneRect(row)) + " in " + rectText(sceneRect(content))));
            }
            app.key(Qt::Key_Down);
        }
    }

    void aContextMenuStaysInsideTheWindowAtEveryCorner() {
        App app(load(framed));
        QVERIFY(app.open());
        app.window->resize(700, 400);
        QTRY_COMPARE(app.window->width(), 700);
        app.model.select({"c"});
        const QPoint corners[] = {{4, 60}, {696, 60}, {4, 396}, {696, 396}, {350, 396}, {696, 200}};
        for (const QPoint &corner : corners) {
            QTest::mouseClick(app.window, Qt::RightButton, {}, corner);
            QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
            QObject *menu = popup(app.window, "contextMenuPopup");
            QTRY_VERIFY2(insideWindow(app.window, menu), qPrintable(rectText(popupRect(menu))));
            // The menu never covers the point it opened at (it flips beside it) unless it had to shift.
            app.key(Qt::Key_Down);
            everyRowIsReachable(app, "contextMenuPopup");
            app.key(Qt::Key_Escape);
            QTRY_VERIFY(!isOpen(app.window, "contextMenuPopup"));
        }
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aNodeMenuNearTheBottomRightKeepsDeleteInside() {
        App app(load(nearCorner));
        QVERIFY(app.open());
        app.window->resize(700, 400);
        QTRY_COMPARE(app.window->width(), 700);
        const QPoint at = app.screen(650, 292);
        QVERIFY2(at.x() > 600 && at.y() > 330 && at.x() < 700 && at.y() < 400, qPrintable(QString::number(at.x()) + "," + QString::number(at.y())));
        QTest::mouseClick(app.window, Qt::RightButton, {}, at);
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QObject *menu = popup(app.window, "contextMenuPopup");
        QCOMPARE(popup(app.window, "contextMenu")->property("kind").toString(), QString("node"));
        QTRY_VERIFY2(insideWindow(app.window, menu), qPrintable(rectText(popupRect(menu))));
        QVERIFY(find(app.window, "menuItem-edit.delete"));
        app.key(Qt::Key_Down);
        everyRowIsReachable(app, "contextMenuPopup");
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void aMenuTallerThanTheWindowIsCappedAndScrolls() {
        App app(load(framed));
        QVERIFY(app.open());
        app.window->resize(700, 160);
        QTRY_COMPARE(app.window->height(), 160);
        app.model.select({"c"});
        QTest::mouseClick(app.window, Qt::RightButton, {}, QPoint(600, 100));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QObject *menu = popup(app.window, "contextMenuPopup");
        QTRY_VERIFY2(insideWindow(app.window, menu), qPrintable(rectText(popupRect(menu))));
        QQuickItem *content = menu->property("contentItem").value<QQuickItem *>();
        QVERIFY(content->property("contentHeight").toReal() > content->height()); // it scrolls
        app.key(Qt::Key_Down);
        everyRowIsReachable(app, "contextMenuPopup");
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theColorsSubmenuOpensLeftWhenThereIsNoRoomOnTheRight() {
        App app(load(nearCorner));
        QVERIFY(app.open());
        app.window->resize(700, 400);
        QTRY_COMPARE(app.window->width(), 700);
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(650, 292));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QQuickItem *colors = NEED("subMenu-Colors");
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(colors));
        QQuickItem *red = nullptr;
        QTRY_VERIFY((red = find(app.window, "menuItem-node.color.red")));
        QObject *parentMenu = popup(app.window, "contextMenuPopup");
        QObject *sub = nullptr;
        for (QObject *child : parentMenu->children())
            if (child->property("sub").toBool() && child->property("visible").toBool())
                sub = child;
        QVERIFY(sub);
        QTRY_VERIFY2(insideWindow(app.window, sub), qPrintable(rectText(popupRect(sub))));
        QVERIFY(popupRect(sub).right() <= popupRect(parentMenu).right()); // beside it, on the left
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void theBarMenusAndTheMenuButtonStayInsideANarrowWindow() {
        App app(load(framed));
        QVERIFY(app.open());
        app.window->resize(260, 260);
        QTRY_VERIFY(find(app.window, "menuButton"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("menuButton")));
        QTRY_VERIFY(isOpen(app.window, "menuEverything"));
        QObject *menu = popup(app.window, "menuEverything");
        QTRY_VERIFY2(insideWindow(app.window, menu), qPrintable(rectText(popupRect(menu))));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("subMenu-Help")));
        QTRY_VERIFY(find(app.window, "menuItem-help.about"));
        for (QObject *child : menu->children())
            if (child->property("sub").toBool() && child->property("visible").toBool())
                QTRY_VERIFY2(insideWindow(app.window, child), qPrintable(rectText(popupRect(child))));
        for (int i = 0; i < 3 && !allClosed(app.window); ++i) {
            app.key(Qt::Key_Escape);
            QTest::qWait(300);
        }
        QTRY_VERIFY(allClosed(app.window));
        // A bar menu opened with its key, in a window narrower than the menu.
        QTest::keyClick(app.window, 'V', Qt::AltModifier);
        QTRY_VERIFY(isOpen(app.window, "menuDropdown"));
        QObject *dropdown = popup(app.window, "menuDropdown");
        QTRY_VERIFY2(insideWindow(app.window, dropdown), qPrintable(rectText(popupRect(dropdown))));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(allClosed(app.window));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    // The row under the pointer, and the one the keys are on, take the menu-row fill; one
    // current row at a time, a disabled one never, a submenu's parent while it is open.
    void theCurrentRowIsFilledOnDarkAndLightThemes_data() {
        QTest::addColumn<QString>("theme");
        QTest::newRow("dark") << "ethereal";
        QTest::newRow("light") << "flexoki-light";
    }
    void theCurrentRowIsFilledOnDarkAndLightThemes() {
        QFETCH(QString, theme);
        if (!QFile::exists("/usr/share/omarchy/themes/" + theme))
            QSKIP("theme not installed here");
        App app(load(framed), {}, canvas_test::Look{theme, 8});
        QVERIFY(app.open());
        auto *look = app.theme.property("look").value<QObject *>();
        const QColor fill = look->property("menuRow").value<QColor>();
        auto filled = [&](const QString &name) {
            QQuickItem *row = find(app.window, name);
            auto *bg = row ? row->property("background").value<QQuickItem *>() : nullptr;
            return bg && bg->property("color").value<QColor>() == fill;
        };
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(620, 90));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QQuickItem *bypass = NEED("menuItem-node.bypass");
        QQuickItem *mute = NEED("menuItem-node.mute");
        QTest::mouseMove(app.window, centerOf(bypass) + QPoint(3, 2));
        QTest::mouseMove(app.window, centerOf(bypass));
        QTest::qWait(100);
        QTRY_VERIFY(filled("menuItem-node.bypass"));
        QVERIFY(!filled("menuItem-node.mute"));
        QTest::mouseMove(app.window, centerOf(mute));
        QTRY_VERIFY(filled("menuItem-node.mute"));
        QVERIFY(!filled("menuItem-node.bypass"));
        // The keys move the same highlight.
        app.key(Qt::Key_Down);
        QTRY_VERIFY(!filled("menuItem-node.mute"));
        // A disabled row (not built yet and off) is not filled under the pointer.
        for (const QVariant &entry : app.commands.menu("node", {}))
            if (entry.toMap().contains("enabled") && !entry.toMap().value("enabled").toBool()) {
                QQuickItem *off = find(app.window, "menuItem-" + entry.toMap().value("command").toString());
                if (off) {
                    QTest::mouseMove(app.window, centerOf(off));
                    QTest::qWait(50);
                    QVERIFY(!filled(off->objectName()));
                }
                break;
            }
        // The Colors row stays filled while its submenu is open.
        QQuickItem *colors = NEED("subMenu-Colors");
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(colors));
        QTRY_VERIFY(find(app.window, "menuItem-node.color.red"));
        QTRY_VERIFY(filled("subMenu-Colors"));
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }

    void shiftF10OpensTheContextMenuFromTheKeys() {
        App app(load(framed));
        QVERIFY(app.open());
        app.model.select({"c"});
        app.key(Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QCOMPARE(popup(app.window, "contextMenu")->property("kind").toString(), QString("node"));
        // The first row that can run is current: Rename (#55).
        QTRY_COMPARE(currentRow(app.window, "contextMenuPopup"), QString("menuItem-edit.rename"));
        for (int i = 0; i < 6 && currentRow(app.window, "contextMenuPopup") != "menuItem-node.mute"; ++i)
            app.key(Qt::Key_Down);
        app.key(Qt::Key_Return);
        QTRY_COMPARE(app.model.document().graph.nodes.value("c").mode, NodeMode::Mute);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }
};

QTEST_MAIN(MenusTests)
#include "test_menus.moc"
