#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "theme.h"
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
        QCOMPARE(registry.keys("app.quit"), (QStringList{"Q", "Ctrl+Q"}));
        QCOMPARE(registry.keys("app.shortcuts"), (QStringList{"?", "F1", "Ctrl+?"}));
        QVERIFY(registry.spec("app.quit")->deviates());
        QVERIFY(!registry.spec("file.save")->deviates());
        QVERIFY(registry.hasHandler("app.quit"));
        QVERIFY(!registry.hasHandler("file.save"));
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
        Backend backend;
        CanvasModel canvasModel(nullptr);
        CommandRegistry commands;
        fugue::registerDefaultCommands(commands);
        commands.applyKeymap("not json");

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("canvasModel", &canvasModel);
        engine.rootContext()->setContextProperty("commands", &commands);
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
