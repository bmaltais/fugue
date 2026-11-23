#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "menu/menu_commands.h"
#include "menu/menus.h"
#include "qml_app.h"
#include "theme.h"
#include "workflow/workflows.h"
#include "translator.h"

using fugue::CommandRegistry;
using fugue::CommandSpec;

namespace {

CommandSpec spec(const QString &id, const QStringList &keys = {}, const QString &context = "app") {
    CommandSpec s;
    s.id = id;
    s.description = "Test " + id;
    s.keys = keys;
    s.context = context;
    return s;
}

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

} // namespace

class CommandsTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void aCommandIsRegisteredWithItsKeysAndRuns() {
        CommandRegistry registry;
        int ran = 0;
        QVERIFY(registry.add(spec("file.save", {"ctrl+s"}), [&] { ++ran; }).isEmpty());
        QVERIFY(registry.contains("file.save"));
        QCOMPARE(registry.keys("file.save"), QStringList{"Ctrl+S"});
        QVERIFY(registry.run("file.save"));
        QCOMPARE(ran, 1);
        QVERIFY(!registry.run("file.nothing"));
    }

    void badSpecsAreRefused() {
        CommandRegistry registry;
        QVERIFY(!registry.add(spec("Save")).isEmpty());
        QVERIFY(!registry.add(spec("file.")).isEmpty());
        QVERIFY(!registry.add(spec("file.save", {"Ctrl+S, Ctrl+K"})).isEmpty());
        QVERIFY(!registry.add(spec("file.save", {"Hyper+Nope"})).isEmpty());
        QVERIFY(!registry.add(spec("file.save", {}, "nowhere")).isEmpty());
        CommandSpec noDescription = spec("file.save");
        noDescription.description = " ";
        QVERIFY(!registry.add(noDescription).isEmpty());
        QVERIFY(registry.add(spec("file.save")).isEmpty());
        QVERIFY(!registry.add(spec("file.save")).isEmpty()); // twice
        QCOMPARE(registry.ids(), QStringList{"file.save"});
    }

    void aLaterTicketSetsAHandler() {
        CommandRegistry registry;
        QVERIFY(registry.add(spec("edit.undo", {"Ctrl+Z"})).isEmpty());
        QVERIFY(!registry.hasHandler("edit.undo"));
        QVERIFY(!registry.run("edit.undo"));
        QVERIFY(registry.ownerOf("Ctrl+Z").isEmpty());
        int ran = 0;
        QVERIFY(registry.setHandler("edit.undo", [&] { ++ran; }));
        QVERIFY(registry.triggerKey("Ctrl+Z"));
        QCOMPARE(ran, 1);
        QVERIFY(!registry.setHandler("edit.nothing", [] {}));
    }

    void everyDefaultCommandOfTheSpecIsRegistered() {
        CommandRegistry registry;
        fugue::registerDefaultCommands(registry);
        QVERIFY2(registry.problems().isEmpty(), qPrintable(registry.problems().join('\n')));
        for (const char *id : {"run.start", "run.start-first", "run.to-here", "run.cancel", "file.new", "file.open",
                               "file.save", "file.save-as", "file.close", "app.quit", "app.shortcuts", "app.settings",
                               "edit.undo", "edit.redo", "edit.copy", "edit.paste", "edit.paste-connected", "edit.clone",
                               "edit.delete", "edit.select-all", "edit.rename", "node.bypass", "node.mute",
                               "node.collapse", "node.pin", "node.search", "frame.make", "group.make", "group.ungroup",
                               "group.enter", "group.leave", "view.fit", "view.zoom-in", "view.zoom-out",
                               "view.toggle-links", "panel.nodes", "panel.workflows"})
            QVERIFY2(registry.contains(id), id);
        // The overlay lists them in the keymap table's order, Run before Cancel the run.
        const QVariantList listed = registry.list();
        QCOMPARE(listed.first().toMap().value("id").toString(), QString("run.start"));
        QCOMPARE(listed.at(3).toMap().value("id").toString(), QString("run.cancel"));
        QCOMPARE(registry.keys("app.quit"), (QStringList{"Q", "Ctrl+Q"}));
        QCOMPARE(registry.keys("app.shortcuts"), (QStringList{"?", "F1", "Ctrl+?"}));
        QVERIFY(registry.spec("app.quit")->deviates());
        QVERIFY(!registry.spec("file.save")->deviates());
        QVERIFY(registry.hasHandler("app.quit"));
        QVERIFY(!registry.hasHandler("file.save"));
    }

    void aMenuEntryReadsItsNameKeysAndStateFromTheRegistry() {
        CommandRegistry registry;
        CommandSpec quit = spec("app.quit", {"Ctrl+Q"});
        quit.description = "Quit (asks if there is unsaved content)";
        quit.label = "Quit";
        QVERIFY(registry.add(quit).isEmpty());
        QVERIFY(registry.add(spec("edit.undo", {"Ctrl+Z"}, "canvas")).isEmpty());
        QVERIFY(registry.add(spec("view.toggle-links", {"L"}, "canvas")).isEmpty());

        // No handler yet: listed, not available, not enabled.
        QVariantMap entry = registry.entry("app.quit");
        QCOMPARE(entry.value("label").toString(), QString("Quit"));
        QCOMPARE(entry.value("keys").toStringList(), QStringList{"Ctrl+Q"});
        QVERIFY(!entry.value("available").toBool());
        QVERIFY(!entry.value("enabled").toBool());
        // Without a label of its own a command shows its description.
        QCOMPARE(registry.entry("edit.undo").value("label").toString(), QString("Test edit.undo"));
        QVERIFY(registry.entry("nothing.here").isEmpty());

        int undone = 0;
        QVERIFY(registry.setHandler("edit.undo", [&] { ++undone; }));
        // A handler, but its context (canvas) is off: not enabled, and trigger() refuses.
        QVERIFY(registry.entry("edit.undo").value("available").toBool());
        QVERIFY(!registry.entry("edit.undo").value("enabled").toBool());
        QVERIFY(!registry.trigger("edit.undo"));
        registry.setContextActive("canvas", true);
        QVERIFY(registry.entry("edit.undo").value("enabled").toBool());

        // The ticket that owns a command says when it can run, and whether it is on.
        bool canUndo = false;
        QVERIFY(registry.setEnabledWhen("edit.undo", [&] { return canUndo; }));
        QVERIFY(!registry.entry("edit.undo").value("enabled").toBool());
        QVERIFY(!registry.trigger("edit.undo"));
        canUndo = true;
        QVERIFY(registry.trigger("edit.undo"));
        QCOMPARE(undone, 1);
        QVERIFY(!registry.setEnabledWhen("nothing.here", [] { return true; }));

        QVERIFY(!registry.entry("view.toggle-links").value("checkable").toBool());
        bool shown = true;
        QVERIFY(registry.setCheckedWhen("view.toggle-links", [&] { return shown; }));
        QVERIFY(registry.entry("view.toggle-links").value("checkable").toBool());
        QVERIFY(registry.entry("view.toggle-links").value("checked").toBool());
        shown = false;
        QVERIFY(!registry.entry("view.toggle-links").value("checked").toBool());
    }

    void everyMenuItemIsARegisteredCommand() {
        CommandRegistry registry;
        fugue::registerDefaultCommands(registry);
        fugue::registerMenuCommands(registry);
        QVERIFY2(registry.problems().isEmpty(), qPrintable(registry.problems().join('\n')));
        QStringList problems;
        std::function<void(const QVariantList &, const QString &)> walk = [&](const QVariantList &items,
                                                                              const QString &where) {
            for (const QVariant &value : items) {
                const QVariantMap item = value.toMap();
                if (item.contains("items"))
                    walk(item.value("items").toList(), where + " > " + item.value("title").toString());
                else if (item.contains("command") && !registry.contains(item.value("command").toString()))
                    problems << where + ": " + item.value("command").toString();
            }
        };
        for (const QString &name : fugue::menus::names())
            walk(fugue::menus::tree(name), name);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
        // registry.menu() resolves each command to its entry, so QML reads one map per item.
        const QVariantList bar = registry.menu("bar");
        QVERIFY(bar.size() >= 4);
        const QVariantMap file = bar.first().toMap();
        QCOMPARE(file.value("title").toString(), QString("&File"));
        const QVariantMap first = file.value("items").toList().first().toMap();
        QCOMPARE(first.value("command").toString(), QString("file.new"));
        QCOMPARE(first.value("label").toString(), QString("New workflow"));
        QCOMPARE(first.value("keys").toStringList(), QStringList{"Ctrl+N"});
        // One command per color, each with its swatch's theme role (spec 4.5).
        for (const QString &role : fugue::menus::colorRoles())
            QVERIFY2(registry.contains("node.color." + role), qPrintable(role));
        // Every top menu, and every context menu, has something in it. (Resolved, a context menu
        // leaves out commands with no handler, as every one is here: #72.)
        for (const QString &name : fugue::menus::names())
            QVERIFY2(!fugue::menus::tree(name).isEmpty(), qPrintable(name));
        QVERIFY(registry.menu("canvas").isEmpty());
    }

    // The Canvas view's own menu bar (#232 slice B): every row is a registered command or a planned placeholder that is
    // not one; a placeholder is greyed with its issue and owns no key; the new keys belong to the view.
    void theCanvasViewMenuHasCommandsAndHonestPlaceholders() {
        CommandRegistry registry;
        fugue::registerDefaultCommands(registry);
        QVERIFY2(registry.problems().isEmpty(), qPrintable(registry.problems().join('\n')));
        const QVariantList bar = registry.menu("canvas-view");
        QCOMPARE(bar.size(), 5);
        QStringList titles, planned;
        QSet<QString> seen;
        std::function<void(const QVariantList &)> walk = [&](const QVariantList &items) {
            for (const QVariant &value : items) {
                const QVariantMap item = value.toMap();
                if (item.contains("items")) {
                    walk(item.value("items").toList());
                } else if (item.value("placeholder").toBool() || item.contains("placeholder")) {
                    QVERIFY(item.value("issue").toInt() > 0);
                    QVERIFY(!item.value("enabled").toBool());
                    QVERIFY(!item.value("available").toBool());
                    QVERIFY(item.value("command").toString().isEmpty());
                    QVERIFY(item.value("keys").toStringList().isEmpty());
                    QVERIFY(!item.value("label").toString().isEmpty());
                    planned << item.value("label").toString();
                } else if (item.contains("command")) {
                    QVERIFY2(registry.contains(item.value("command").toString()), qPrintable(item.value("command").toString()));
                    QVERIFY(!item.value("label").toString().isEmpty());
                    QVERIFY2(!item.value("label").toString().contains(':'), qPrintable(item.value("label").toString())); // a short label, not the description
                    seen.insert(item.value("command").toString());
                }
            }
        };
        for (const QVariant &menu : bar)
            titles << menu.toMap().value("title").toString();
        walk(bar);
        QCOMPARE(titles, (QStringList{"&Canvas", "&Edit", "&Select", "&Layer", "&View"}));
        // Placeholders are honest (checked above, per item); how many remain is not asserted: each is built in time.
        // Every canvas-view command is reachable from the bar.
        for (const QString &id : registry.ids())
            if (id.startsWith("canvas-view."))
                QVERIFY2(seen.contains(id), qPrintable(id + " is in no menu"));
        // No command id is made for a placeholder.
        for (const QString &id : registry.ids())
            QVERIFY2(!id.contains("planned"), qPrintable(id));
        registry.setContextActive("canvas-view", true);
        QVERIFY(registry.setHandler("canvas-view.deselect", [] {}));
        QCOMPARE(registry.ownerOf("Ctrl+D"), QString("canvas-view.deselect"));
        QCOMPARE(registry.ownerOf("Ctrl+A"), QString("canvas-view.select-all"));
        QCOMPARE(registry.ownerOf("Ctrl+Shift+I"), QString("canvas-view.invert-selection"));
        // A planned row does not show in a context menu list either way: the bar of the view shows all, the others do not.
        QVERIFY(registry.menu("canvas").isEmpty());
    }

    void aContextMenuCommandSeesWhatItWasOpenedOn() {
        CommandRegistry registry;
        QVERIFY(registry.add(spec("node.color.red")).isEmpty());
        QVariantMap seen;
        QVERIFY(registry.setHandler("node.color.red", [&] { seen = registry.target(); }));
        QVERIFY(registry.setEnabledWhen("node.color.red", [&] { return registry.target().contains("frame"); }));
        QVERIFY(!registry.entry("node.color.red").value("enabled").toBool());
        QVERIFY(registry.entry("node.color.red", {{"frame", "f1"}}).value("enabled").toBool());
        QVERIFY(!registry.trigger("node.color.red"));
        QVERIFY(registry.triggerOn("node.color.red", {{"frame", "f1"}}));
        QCOMPARE(seen.value("frame").toString(), QString("f1"));
        QVERIFY(registry.target().isEmpty()); // only while it runs
    }

    void menuMnemonicsDoNotClashWithTheKeymap() {
        // Alt+letter opens a top menu only where 12.4 binds nothing to it (Alt+C collapses).
        CommandRegistry registry;
        fugue::registerDefaultCommands(registry);
        fugue::registerMenuCommands(registry);
        QSet<QString> altKeys;
        for (const QString &id : registry.ids())
            for (const QString &key : registry.keys(id))
                altKeys.insert(key);
        QSet<QChar> seen;
        for (const QVariant &value : fugue::menus::tree("bar")) {
            const QString title = value.toMap().value("title").toString();
            const qsizetype amp = title.indexOf('&');
            QVERIFY2(amp >= 0 && amp + 1 < title.size(), qPrintable(title));
            const QChar letter = title.at(amp + 1).toUpper();
            QVERIFY2(!seen.contains(letter), qPrintable(title));
            seen.insert(letter);
            QVERIFY2(!altKeys.contains(QStringLiteral("Alt+") + letter), qPrintable(title));
        }
    }

    void theMoreSpecificContextOwnsAKey() {
        // Tab enters a group when one is selected and opens node search otherwise (12.4).
        CommandRegistry registry;
        fugue::registerDefaultCommands(registry);
        QString ran;
        QVERIFY(registry.setHandler("node.search", [&] { ran = "search"; }));
        QVERIFY(registry.setHandler("group.enter", [&] { ran = "enter"; }));
        QVERIFY(registry.setHandler("group.leave", [&] { ran = "leave"; }));
        QVERIFY(registry.setHandler("run.cancel", [&] { ran = "cancel"; }));
        QVERIFY(registry.ownerOf("Tab").isEmpty()); // canvas not active yet
        registry.setContextActive("canvas", true);
        QCOMPARE(registry.ownerOf("Tab"), QString("node.search"));
        registry.setContextActive("group-selected", true);
        QCOMPARE(registry.ownerOf("Tab"), QString("group.enter"));
        registry.setContextActive("group", true);
        QCOMPARE(registry.ownerOf("Escape"), QString("group.leave"));
        registry.setContextActive("run", true);
        QCOMPARE(registry.ownerOf("Escape"), QString("run.cancel")); // cancels first, then leaves
        QVERIFY(registry.triggerKey("Escape"));
        QCOMPARE(ran, QString("cancel"));
        registry.setContextActive("run", false);
        QCOMPARE(registry.ownerOf("Escape"), QString("group.leave"));
    }

    // One keymap for a picture and the Graph (#228, docs/ui-ergonomics.md D5): + and - zoom the Graph too, and a key
    // press is told its owner, so a focused Canvas view keeps the Graph's keys (and only those) from the Graph.
    void aPressIsToldItsOwnerAndWhetherItActsOnTheGraph() {
        CommandRegistry registry;
        fugue::registerDefaultCommands(registry);
        for (const char *id : {"view.fit", "view.zoom-in", "view.zoom-out", "edit.delete", "edit.undo", "app.quit", "panel.nodes", "group.leave"})
            QVERIFY(registry.setHandler(id, [] {}));
        registry.setContextActive("canvas", true);
        QCOMPARE(registry.keys("view.zoom-in"), (QStringList{"Alt+=", "Ctrl+=", "+"}));
        QCOMPARE(registry.keys("view.zoom-out"), (QStringList{"Alt+-", "Ctrl+-", "-"}));
        QCOMPARE(registry.ownerOfPress(Qt::Key_Plus, Qt::NoModifier), QString("view.zoom-in"));
        QCOMPARE(registry.ownerOfPress(Qt::Key_Plus, Qt::ShiftModifier), QString("view.zoom-in")); // + typed as Shift+=
        QCOMPARE(registry.ownerOfPress(Qt::Key_Minus, Qt::NoModifier), QString("view.zoom-out"));
        QCOMPARE(registry.ownerOfPress(Qt::Key_Home, Qt::NoModifier), QString("view.fit"));
        QCOMPARE(registry.ownerOfPress(Qt::Key_Period, Qt::NoModifier), QString("view.fit"));
        QCOMPARE(registry.ownerOfPress(Qt::Key_Z, Qt::ControlModifier), QString("edit.undo"));
        QCOMPARE(registry.ownerOfPress(Qt::Key_Q, Qt::NoModifier), QString("app.quit"));
        QVERIFY(registry.ownerOfPress(Qt::Key_0, Qt::NoModifier).isEmpty());
        // The Graph's: its view, its edits, its groups. Not the window's: quit, the document's undo (the region's too),
        // the panels.
        for (const char *id : {"view.fit", "view.zoom-in", "edit.delete", "group.leave"})
            QVERIFY2(registry.actsOnGraph(id), id);
        for (const char *id : {"app.quit", "edit.undo", "edit.redo", "panel.nodes", "view.focus-mode", "run.cancel", "nothing.here"})
            QVERIFY2(!registry.actsOnGraph(id), id);
    }

    // The Canvas view's tools (#230): the `canvas-view` context owns their keys over the view (Q is Quick Mask, not
    // Quit; Ctrl+Z the view's), the layers panel's keys still win over it, no two tools share a key, and none acts on
    // the Graph.
    void theCanvasViewsToolsOwnTheirKeysOverTheView() {
        CommandRegistry registry;
        fugue::registerDefaultCommands(registry);
        QVERIFY2(registry.problems().isEmpty(), qPrintable(registry.problems().join('\n')));
        QVERIFY(registry.setHandler("app.quit", [] {}));
        QVERIFY(registry.setHandler("edit.undo", [] {}));
        registry.setContextActive("canvas", true);
        QCOMPARE(registry.ownerOf("Q"), QString("app.quit"));
        QVERIFY(registry.ownerOf("B").isEmpty());
        registry.setContextActive("canvas-view", true);
        QCOMPARE(registry.ownerOf("Q"), QString("canvas-view.quick-mask"));
        QCOMPARE(registry.ownerOf("B"), QString("canvas-view.tool-brush"));
        QCOMPARE(registry.ownerOf("Ctrl+Z"), QString("canvas-view.undo"));
        QCOMPARE(registry.ownerOf("\\"), QString("canvas-view.mask-overlay"));
        QCOMPARE(registry.ownerOf("X"), QString("canvas-view.brush-swap"));
        registry.setContextActive("canvas-layers", true);
        QCOMPARE(registry.ownerOf("X"), QString("canvas-layers.delete"));
        QCOMPARE(registry.ownerOf("Ctrl+Z"), QString("canvas-layers.undo"));
        QCOMPARE(registry.ownerOf("B"), QString("canvas-view.tool-brush")); // the panel binds no B
        QSet<QString> keys;
        for (const QString &id : registry.ids()) {
            if (!id.startsWith("canvas-view."))
                continue;
            QVERIFY2(!registry.actsOnGraph(id), qPrintable(id));
            for (const QString &key : registry.keys(id)) {
                QVERIFY2(!keys.contains(key), qPrintable(id + " " + key));
                keys.insert(key);
            }
        }
        QVERIFY(keys.contains("Ctrl+Shift+M") && keys.contains("Ctrl+I") && keys.contains("M") && keys.contains("R"));
    }

    void textFieldsOwnBareAndShiftKeys() {
        for (const char *key : {"Q", "?", "N", "Shift+A", "Shift+Return", "Backspace", "Delete", "Tab", "Ctrl+A",
                                "Ctrl+C", "Ctrl+V", "Ctrl+X", "Ctrl+Z", "Ctrl+Y", "Ctrl+Shift+Z", "Shift+Z"})
            QVERIFY2(CommandRegistry::belongsToTextField(key), key);
        for (const char *key : {"Ctrl+Q", "F1", "Ctrl+?", "Ctrl+W", "Ctrl+S", "Escape", "Ctrl+Return", "Alt+C"})
            QVERIFY2(!CommandRegistry::belongsToTextField(key), key);

        CommandRegistry registry;
        int quit = 0;
        QVERIFY(registry.add(spec("app.quit", {"Q", "Ctrl+Q"}), [&] { ++quit; }).isEmpty());
        registry.setTextFieldFocus(true);
        QVERIFY(!registry.triggerKey("Q"));
        QCOMPARE(quit, 0);
        QVERIFY(registry.triggerKey("Ctrl+Q"));
        QCOMPARE(quit, 1);
        registry.setTextFieldFocus(false);
        QVERIFY(registry.triggerKey("Q"));
    }

    void theKeymapIsADiffAgainstTheDefaults() {
        CommandRegistry registry;
        fugue::registerDefaultCommands(registry);
        registry.applyKeymap(R"({"add": [{"command": "view.fit", "key": "f"}],
                                 "unset": [{"command": "app.quit", "key": "Q"}]})");
        QVERIFY2(registry.problems().isEmpty(), qPrintable(registry.problems().join('\n')));
        QCOMPARE(registry.keys("view.fit"), (QStringList{".", "Home", "F"}));
        QCOMPARE(registry.keys("app.quit"), QStringList{"Ctrl+Q"});
        QVERIFY(registry.activeKeys().contains("Ctrl+Q"));
        QVERIFY(!registry.activeKeys().contains("Q"));
        // Applying a new diff starts from the defaults again.
        registry.applyKeymap("{}");
        // The overlay lists them in the keymap table's order, Run before Cancel the run.
        const QVariantList listed = registry.list();
        QCOMPARE(listed.first().toMap().value("id").toString(), QString("run.start"));
        QCOMPARE(listed.at(3).toMap().value("id").toString(), QString("run.cancel"));
        QCOMPARE(registry.keys("app.quit"), (QStringList{"Q", "Ctrl+Q"}));
        QCOMPARE(registry.keys("view.fit"), (QStringList{".", "Home"}));
    }

    void theUsersBindingBeatsADefaultInTheSameContext() {
        CommandRegistry registry;
        QVERIFY(registry.add(spec("a.one", {"X"}), [] {}).isEmpty());
        QVERIFY(registry.add(spec("a.two"), [] {}).isEmpty());
        registry.applyKeymap(R"({"add": [{"command": "a.two", "key": "X"}]})");
        QCOMPARE(registry.ownerOf("X"), QString("a.two"));
        QCOMPARE(registry.problems().size(), 1); // and the user is told
    }

    void aMalformedKeymapKeepsTheDefaultsAndSaysWhy_data() {
        QTest::addColumn<QByteArray>("json");
        QTest::newRow("not json") << QByteArray("{add: [");
        QTest::newRow("not an object") << QByteArray("[1, 2]");
        QTest::newRow("empty") << QByteArray("");
        QTest::newRow("add not a list") << QByteArray(R"({"add": 7})");
        QTest::newRow("entry not an object") << QByteArray(R"({"add": [3]})");
        QTest::newRow("key not text") << QByteArray(R"({"add": [{"command": "view.fit", "key": 5}]})");
        QTest::newRow("key not a key") << QByteArray(R"({"add": [{"command": "view.fit", "key": "Ctrl+"}]})");
        QTest::newRow("chord") << QByteArray(R"({"add": [{"command": "view.fit", "key": "Ctrl+K, Ctrl+F"}]})");
        QTest::newRow("empty key") << QByteArray(R"({"add": [{"command": "view.fit", "key": ""}]})");
        QTest::newRow("unknown command") << QByteArray(R"({"add": [{"command": "no.such", "key": "F"}]})");
        QTest::newRow("unset not bound") << QByteArray(R"({"unset": [{"command": "app.quit", "key": "Z"}]})");
        QTest::newRow("unknown entry") << QByteArray(R"({"remove": []})");
    }
    void aMalformedKeymapKeepsTheDefaultsAndSaysWhy() {
        QFETCH(QByteArray, json);
        CommandRegistry registry;
        fugue::registerDefaultCommands(registry);
        registry.applyKeymap(json);
        QVERIFY(!registry.problems().isEmpty());
        QCOMPARE(registry.keys("view.fit"), (QStringList{".", "Home"}));
        // The overlay lists them in the keymap table's order, Run before Cancel the run.
        const QVariantList listed = registry.list();
        QCOMPARE(listed.first().toMap().value("id").toString(), QString("run.start"));
        QCOMPARE(listed.at(3).toMap().value("id").toString(), QString("run.cancel"));
        QCOMPARE(registry.keys("app.quit"), (QStringList{"Q", "Ctrl+Q"}));
    }

    void theKeymapFileIsRead() {
        QTemporaryDir dir;
        CommandRegistry registry;
        fugue::registerDefaultCommands(registry);
        registry.loadKeymap(dir.filePath("missing.json")); // no file is fine
        QVERIFY(registry.problems().isEmpty());

        QVERIFY(write(dir.filePath("keybindings.json"), R"({"add": [{"command": "view.fit", "key": "F"}]})"));
        registry.loadKeymap(dir.filePath("keybindings.json"));
        QVERIFY(registry.problems().isEmpty());
        QVERIFY(registry.keys("view.fit").contains("F"));

        QVERIFY(write(dir.filePath("keybindings.json"), "garbage"));
        registry.loadKeymap(dir.filePath("keybindings.json"));
        QVERIFY(!registry.problems().isEmpty());
        QCOMPARE(registry.keys("view.fit"), (QStringList{".", "Home"}));

        // A folder, and a huge file, are not read.
        registry.loadKeymap(dir.path());
        QVERIFY(!registry.problems().isEmpty());
        QVERIFY(write(dir.filePath("big.json"), QByteArray(2 * 1024 * 1024, ' ')));
        registry.loadKeymap(dir.filePath("big.json"));
        QVERIFY(!registry.problems().isEmpty());
    }

    void theKeymapFileLivesInTheFugueConfigFolder() {
        QVERIFY(CommandRegistry::keymapPath().endsWith("/fugue/keybindings.json"));
    }

    void quitAsksFirstWithUnsavedWork() {
        CommandRegistry registry;
        fugue::registerDefaultCommands(registry);
        QSignalSpy asked(&registry, &CommandRegistry::quitConfirmationRequested);
        registry.setUnsavedWork(true);
        QVERIFY(registry.run("app.quit"));
        QCOMPARE(asked.size(), 1);
        QVERIFY(registry.run("file.close")); // no workflow open: closing is leaving, so the same question
        QCOMPARE(asked.size(), 2);
        registry.setWorkflowOpen(true);
        QSignalSpy closed(&registry, &CommandRegistry::closeWorkflowRequested);
        QVERIFY(registry.run("file.close"));
        QCOMPARE(closed.size(), 1);
        QCOMPARE(asked.size(), 2);
    }

    void theWindowListsEveryCommandAndAnswersTheKeys() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language");
        QCoreApplication::installTranslator(&translator);
        Theme theme(dir.path());
        CanvasModel canvasModel(nullptr);
        CommandRegistry commands;
        fugue::registerDefaultCommands(commands);
        commands.applyKeymap("not json");
        fugue::Workflows workflows(&canvasModel, nullptr, &commands,
                                   {dir.path() + "/recovery", dir.path() + "/trust.json"});

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        fugue::exposeToQml(engine, {&translator, &theme, &commands, &canvasModel, &workflows});
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));

        // The bad keymap file is visible.
        auto *notice = window->findChild<QObject *>("keymapNotice");
        QVERIFY(notice);
        QVERIFY(notice->property("visible").toBool());

        auto *help = window->findChild<QObject *>("help");
        QVERIFY(help);
        QTest::keyClick(window, Qt::Key_F1);
        QTRY_VERIFY(help->property("opened").toBool());
        // The overlay lists every registered command, deviations included.
        const QVariantList entries = help->property("entries").toList();
        QCOMPARE(entries.size(), commands.ids().size());
        bool sawDeviation = false, sawUnavailable = false;
        for (const QVariant &entry : entries) {
            sawDeviation |= entry.toMap().value("deviates").toBool();
            sawUnavailable |= !entry.toMap().value("available").toBool();
        }
        QVERIFY(sawDeviation && sawUnavailable);
        QTest::keyClick(window, '?');
        QTRY_VERIFY(!help->property("visible").toBool());
        QTest::keyClick(window, '?', Qt::ControlModifier);
        QTRY_VERIFY(help->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!help->property("visible").toBool());

        // Unsaved work: Q asks, and does not quit.
        commands.setUnsavedWork(true);
        auto *ask = window->findChild<QObject *>("quitAsk");
        QVERIFY(ask);
        QTest::keyClick(window, 'Q');
        QTRY_VERIFY(ask->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!ask->property("visible").toBool());

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
    }
};

QTEST_MAIN(CommandsTests)
#include "commands_tests.moc"
