// The tabs that were open when a session ended (spec 12.10): what is written, what is read back,
// and which of the paths are opened again. The rules are pure; no window.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "workflow/tab_session.h"

using namespace fugue;

class TestTabSession : public QObject {
    Q_OBJECT

    static QString touch(const QTemporaryDir &dir, const QString &name) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::WriteOnly))
            qFatal("cannot write %s", qPrintable(file.fileName()));
        return file.fileName();
    }

private slots:
    void savedAndReadBack() {
        QTemporaryDir dir;
        const QString path = dir.filePath("state/tabs.json");
        QVERIFY(saveTabSession(path, {{"/a/one.fugue", "/a/two.fugue"}, "/a/two.fugue"}).isEmpty());
        const TabSession read = loadTabSession(path);
        QCOMPARE(read.paths, (QStringList{"/a/one.fugue", "/a/two.fugue"}));
        QCOMPARE(read.active, QString("/a/two.fugue"));
    }
    void aMissingOrBrokenFileIsNoSession() {
        QTemporaryDir dir;
        QVERIFY(loadTabSession(dir.filePath("none.json")).paths.isEmpty());
        QFile bad(dir.filePath("bad.json"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("{not json");
        bad.close();
        QVERIFY(loadTabSession(dir.filePath("bad.json")).paths.isEmpty());
        QFile other(dir.filePath("other.json"));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.write(R"({"format": 99, "tabs": ["/x.fugue"]})");
        other.close();
        QVERIFY(loadTabSession(dir.filePath("other.json")).paths.isEmpty());
    }
    void wrongTypesAreIgnored() {
        QTemporaryDir dir;
        QFile file(dir.filePath("t.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"format": 1, "tabs": ["/a.fugue", 3, "", null, "/b.fugue"], "active": 7})");
        file.close();
        const TabSession read = loadTabSession(dir.filePath("t.json"));
        QCOMPARE(read.paths, (QStringList{"/a.fugue", "/b.fugue"}));
        QVERIFY(read.active.isEmpty());
    }
    void theWriteIsAtomicAndReplaces() {
        QTemporaryDir dir;
        const QString path = dir.filePath("tabs.json");
        QVERIFY(saveTabSession(path, {{"/a.fugue"}, "/a.fugue"}).isEmpty());
        QVERIFY(saveTabSession(path, {{}, {}}).isEmpty());
        QVERIFY(loadTabSession(path).paths.isEmpty());
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{"tabs.json"});
    }
    void everyExistingFileIsKeptInOrder() {
        QTemporaryDir dir;
        const QString a = touch(dir, "a.fugue"), b = touch(dir, "b.fugue"), c = touch(dir, "c.fugue");
        const TabRestore plan = planRestore({{a, b, c}, b});
        QCOMPARE(plan.paths, (QStringList{a, b, c}));
        QCOMPARE(plan.active, b);
        QVERIFY(plan.skipped.isEmpty());
    }
    void aGoneFileIsSkippedAndNamed() {
        QTemporaryDir dir;
        const QString a = touch(dir, "a.fugue");
        const QString gone = dir.filePath("gone.fugue");
        const TabRestore plan = planRestore({{a, gone}, a});
        QCOMPARE(plan.paths, QStringList{a});
        QCOMPARE(plan.skipped, QStringList{gone});
    }
    void aFolderOrARelativePathIsSkipped() {
        QTemporaryDir dir;
        const QString a = touch(dir, "a.fugue");
        QVERIFY(QDir(dir.path()).mkdir("sub.fugue"));
        const TabRestore plan = planRestore({{a, dir.filePath("sub.fugue"), "rel/x.fugue"}, a});
        QCOMPARE(plan.paths, QStringList{a});
        QCOMPARE(plan.skipped.size(), 2);
    }
    void aRepeatedPathOpensOnce() {
        QTemporaryDir dir;
        const QString a = touch(dir, "a.fugue");
        const TabRestore plan = planRestore({{a, a}, a});
        QCOMPARE(plan.paths, QStringList{a});
        QVERIFY(plan.skipped.isEmpty());
    }
    void theActiveTabIsTheLastWhenItsFileIsGone() {
        QTemporaryDir dir;
        const QString a = touch(dir, "a.fugue"), b = touch(dir, "b.fugue");
        QCOMPARE(planRestore({{a, b, dir.filePath("gone.fugue")}, dir.filePath("gone.fugue")}).active, b);
        // An active tab with no file (an untitled one was shown) is the same: the last.
        QCOMPARE(planRestore({{a, b}, QString()}).active, b);
    }
    void nothingToRestoreIsAnEmptyPlan() {
        const TabRestore plan = planRestore({});
        QVERIFY(plan.paths.isEmpty());
        QVERIFY(plan.active.isEmpty());
        QVERIFY(plan.skipped.isEmpty());
    }
};

QTEST_MAIN(TestTabSession)
#include "test_tab_session.moc"
