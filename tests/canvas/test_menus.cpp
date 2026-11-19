// The menu bar and the context menus in the real window (#50, Main.qml offscreen): each top
// menu opens with the mouse and with Alt+letter, the keys move through them, items run their
// commands with the registry's state, the hamburger takes over when narrow, and right-clicks
// open the canvas, node and Frame menus with their colors.

#include <QClipboard>
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

// No menu is showing, its closing fade done (keys pressed during the fade are its own).
bool allClosed(QQuickWindow *window) {
    return !isOpen(window, "menuDropdown") && !isOpen(window, "menuEverything") && !isOpen(window, "contextMenuPopup");
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
        QCOMPARE(rename->opacity(), 0.5);
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

        // On the Frame's empty part: the Frame menu, its colors go to the Frame.
        app.model.select({});
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(320, 30));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QCOMPARE(popup(app.window, "contextMenu")->property("kind").toString(), QString("frame"));
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(NEED("subMenu-Colors")));
        QTRY_VERIFY(find(app.window, "menuItem-node.color.blue"));
        QQuickItem *blue = find(app.window, "menuItem-node.color.blue");
        QVERIFY(blue->isEnabled());
        QTest::mouseClick(app.window, Qt::LeftButton, {}, centerOf(blue));
        QTRY_COMPARE(app.model.frames().first().toMap().value("color").toString(), QString("blue"));
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
        QVERIFY(app.window->grabWindow().save(name + "-edit.png"));
        app.key(Qt::Key_Escape);
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(allClosed(app.window));
        QTest::mouseClick(app.window, Qt::RightButton, {}, app.screen(620, 90));
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QTest::mouseMove(app.window, centerOf(NEED("menuItem-node.mute")));
        QTest::qWait(400);
        QVERIFY(app.window->grabWindow().save(name + "-node.png"));
    }

    void shiftF10OpensTheContextMenuFromTheKeys() {
        App app(load(framed));
        QVERIFY(app.open());
        app.model.select({"c"});
        app.key(Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(isOpen(app.window, "contextMenuPopup"));
        QCOMPARE(popup(app.window, "contextMenu")->property("kind").toString(), QString("node"));
        // Rename is not built yet, so the first row that can run is current.
        QTRY_COMPARE(currentRow(app.window, "contextMenuPopup"), QString("menuItem-node.bypass"));
        for (int i = 0; i < 6 && currentRow(app.window, "contextMenuPopup") != "menuItem-node.mute"; ++i)
            app.key(Qt::Key_Down);
        app.key(Qt::Key_Return);
        QTRY_COMPARE(app.model.document().graph.nodes.value("c").mode, NodeMode::Mute);
        QVERIFY2(app.warnings.isEmpty(), qPrintable(app.warnings.join('\n')));
    }
};

QTEST_MAIN(MenusTests)
#include "test_menus.moc"
