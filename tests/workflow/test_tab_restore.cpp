// Reopening the tabs of the last session (spec 12.10, #212) on the real Workflows: what is kept
// while tabs open, close and switch, what a second start does with it, the setting, and the
// window showing the restored tabs. Offscreen; every file is in a temporary folder.

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/registry.h"
#include "engine/settings.h"
#include "nodes/builtin.h"
#include "qml_app.h"
#include "theme.h"
#include "translator.h"
#include "workflow/tab_session.h"
#include "workflow/workflows.h"

using namespace fugue;

namespace {

const char *textNode = R"({"fugue": 1, "graph": {"nodes": {
    "prompt": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": "a fox"}}}},
  "layout": {"nodes": {"prompt": {"pos": [10, 20]}}}})";

bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QQuickItem *item(QQuickItem *from, const QString &name) {
    for (auto *child : from->childItems()) {
        if (child->objectName() == name)
            return child;
        if (auto *found = item(child, name))
            return found;
    }
    return nullptr;
}

// One start of the app: its own canvas and Workflows, on the folder the previous start used.
struct Start {
    Registry registry;
    CanvasModel canvas;
    CommandRegistry commands;
    Workflows workflows;

    static WorkflowLocations locations(const QTemporaryDir &dir) {
        WorkflowLocations where;
        where.recoveryDir = dir.filePath("state/recovery");
        where.trustPath = dir.filePath("state/trust.json");
        where.tabColorsPath = dir.filePath("state/tab-colors.json");
        where.sessionPath = dir.filePath("state/tabs.json");
        where.recoveryIntervalMs = 3600 * 1000;
        where.sessionDelayMs = 20;
        return where;
    }
    explicit Start(const QTemporaryDir &dir)
        : canvas((nodes::addBuiltinNodeTypes(registry), &registry)),
          workflows(&canvas, &registry, &commands, locations(dir)) {}

    QStringList paths() {
        QStringList out;
        for (const QVariant &tab : workflows.tabs())
            out << tab.toMap().value("path").toString();
        return out;
    }
};

} // namespace

class TestTabRestore : public QObject {
    Q_OBJECT

    static QString flow(const QTemporaryDir &dir, const QString &name) {
        const QString path = dir.filePath(name);
        if (!write(path, textNode))
            qFatal("cannot write %s", qPrintable(path));
        return path;
    }
    static QString sessionFile(const QTemporaryDir &dir) { return dir.filePath("state/tabs.json"); }

private slots:
    void theOpenTabsAreKeptAndComeBack() {
        QTemporaryDir dir;
        const QString a = flow(dir, "a.fugue"), b = flow(dir, "b.fugue"), c = flow(dir, "c.fugue");
        {
            Start first(dir);
            first.workflows.setSessionEnabled(true);
            first.workflows.restoreSession();
            for (const QString &path : {a, b, c})
                QVERIFY(first.workflows.openPath(path));
            first.workflows.setCurrent(1); // b is shown
            first.workflows.flushSession();
        }
        Start second(dir);
        second.workflows.setSessionEnabled(true);
        second.workflows.restoreSession();
        QCOMPARE(second.paths(), (QStringList{a, b, c})); // the blank start tab is replaced
        QCOMPARE(second.workflows.current(), 1);
        QVERIFY(second.workflows.notice().isEmpty());
    }

    void aChangeIsWrittenAfterTheDelayWithoutAQuit() {
        QTemporaryDir dir;
        const QString a = flow(dir, "a.fugue");
        Start start(dir);
        start.workflows.setSessionEnabled(true);
        start.workflows.restoreSession();
        QVERIFY(start.workflows.openPath(a));
        QTRY_COMPARE(loadTabSession(sessionFile(dir)).paths, QStringList{a});
        start.workflows.closeTab(start.workflows.current()); // a crash after this loses nothing
        QTRY_VERIFY(loadTabSession(sessionFile(dir)).paths.isEmpty());
    }

    void nothingIsWrittenBeforeTheRestoreHasRun() {
        QTemporaryDir dir;
        const QString a = flow(dir, "a.fugue");
        QVERIFY(saveTabSession(sessionFile(dir), {{a}, a}).isEmpty());
        Start start(dir); // a window started with a file named never calls restoreSession()
        start.workflows.setSessionEnabled(true);
        start.workflows.newWorkflow();
        QTest::qWait(100);
        QCOMPARE(loadTabSession(sessionFile(dir)).paths, QStringList{a});
        start.workflows.flushSession();
        QCOMPARE(loadTabSession(sessionFile(dir)).paths, QStringList{a});
    }

    void aNewUnsavedTabIsNotKept() {
        QTemporaryDir dir;
        const QString a = flow(dir, "a.fugue");
        {
            Start first(dir);
            first.workflows.setSessionEnabled(true);
            first.workflows.restoreSession();
            QVERIFY(first.workflows.openPath(a));
            first.workflows.newWorkflow(); // Untitled, shown
            first.workflows.flushSession();
        }
        const TabSession kept = loadTabSession(sessionFile(dir));
        QCOMPARE(kept.paths, QStringList{a});
        Start second(dir);
        second.workflows.setSessionEnabled(true);
        second.workflows.restoreSession();
        QCOMPARE(second.paths(), QStringList{a}); // shown: the last one, since the session's tab had no file
        QCOMPARE(second.workflows.current(), 0);
    }

    void aGoneFileIsSkippedWithOneLine() {
        QTemporaryDir dir;
        const QString a = flow(dir, "a.fugue");
        QVERIFY(saveTabSession(sessionFile(dir), {{a, dir.filePath("gone.fugue"), dir.filePath("gone2.fugue")}, a}).isEmpty());
        Start start(dir);
        start.workflows.setSessionEnabled(true);
        start.workflows.restoreSession();
        QCOMPARE(start.paths(), QStringList{a});
        QVERIFY2(start.workflows.notice().contains("2 tabs"), qPrintable(start.workflows.notice()));
        QVERIFY(start.workflows.notice().contains("gone.fugue"));
        start.workflows.flushSession(); // what was skipped is dropped from the kept session
        QCOMPARE(loadTabSession(sessionFile(dir)).paths, QStringList{a});
    }

    void aFileThatCannotBeReadIsSkippedToo() {
        QTemporaryDir dir;
        const QString a = flow(dir, "a.fugue");
        const QString bad = dir.filePath("bad.fugue");
        QVERIFY(write(bad, "{ not a workflow"));
        QVERIFY(saveTabSession(sessionFile(dir), {{bad, a}, a}).isEmpty());
        Start start(dir);
        start.workflows.setSessionEnabled(true);
        start.workflows.restoreSession();
        QCOMPARE(start.paths(), QStringList{a});
        QVERIFY(start.workflows.notice().contains("bad.fugue"));
    }

    void offKeepsNothingAndRemovesWhatWasKept() {
        QTemporaryDir dir;
        const QString a = flow(dir, "a.fugue");
        QVERIFY(saveTabSession(sessionFile(dir), {{a}, a}).isEmpty());
        Start start(dir);
        start.workflows.setSessionEnabled(true);
        start.workflows.setSessionEnabled(false);
        QVERIFY(!QFile::exists(sessionFile(dir)));
        start.workflows.restoreSession(); // off: opens nothing
        QCOMPARE(start.paths(), QStringList{QString()});
        QVERIFY(start.workflows.openPath(a));
        start.workflows.flushSession();
        QTest::qWait(100);
        QVERIFY(!QFile::exists(sessionFile(dir)));
    }

    void turnedOnInASessionKeepsFromThen() {
        QTemporaryDir dir;
        const QString a = flow(dir, "a.fugue");
        Start start(dir);
        start.workflows.setSessionEnabled(false);
        start.workflows.restoreSession();
        QVERIFY(start.workflows.openPath(a));
        QVERIFY(!QFile::exists(sessionFile(dir)));
        start.workflows.setSessionEnabled(true); // kept from now on
        QTRY_COMPARE(loadTabSession(sessionFile(dir)).paths, QStringList{a});
    }

    void settingRestoreTabsIsOnByDefaultAndChecked() {
        QVERIFY(Settings().restoreTabs);
        QCOMPARE(settingDefault("restore_tabs"), QJsonValue(true));
        QCOMPARE(checkSetting("restore_tabs", false), QString());
        QVERIFY(!checkSetting("restore_tabs", "yes").isEmpty());
        QCOMPARE(settingClass("restore_tabs"), SettingClass::Preferences);
    }

    void theWindowShowsTheRestoredTabs() {
        QTemporaryDir dir;
        const QString a = flow(dir, "a.fugue"), b = flow(dir, "b.fugue");
        QVERIFY(saveTabSession(sessionFile(dir), {{a, b}, a}).isEmpty());

        Translator translator(dir.path() + "/language");
        QCoreApplication::installTranslator(&translator);
        QVERIFY(write(dir.filePath("shell.toml"), "[font]\nbase-size = 12\n"));
        Theme theme(dir.path());
        Start start(dir);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        fugue::exposeToQml(engine, {&translator, &theme, &start.commands, &start.canvas, &start.workflows});
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(QTest::qWaitForWindowExposed(window));

        start.workflows.setSessionEnabled(true);
        start.workflows.restoreSession(); // after the window is up, as main() does
        QTRY_VERIFY(item(window->contentItem(), "tab-1"));
        QVERIFY(!item(window->contentItem(), "tab-2"));
        QCOMPARE(start.workflows.current(), 0);
        QCOMPARE(window->title(), QString("a.fugue — Fugue"));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join("\n")));
    }
};

QTEST_MAIN(TestTabRestore)
#include "test_tab_restore.moc"
