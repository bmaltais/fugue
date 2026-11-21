// The recent files of the workflows panel (#56): newest first, only files that exist, not those
// open in a tab, kept between runs.

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "workflow/recent_files.h"

using namespace fugue;

class TestRecentFiles : public QObject {
    Q_OBJECT

    static QStringList paths(const RecentFiles &recent) {
        QStringList out;
        for (const QVariant &entry : recent.entries())
            out << entry.toMap().value("path").toString();
        return out;
    }
    static QString touch(const QTemporaryDir &dir, const QString &name) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::WriteOnly))
            qFatal("cannot write %s", qPrintable(file.fileName()));
        return file.fileName();
    }

private slots:
    void newestFirstAndKeptBetweenRuns() {
        QTemporaryDir dir;
        const QString a = touch(dir, "a.fugue"), b = touch(dir, "b.fugue");
        {
            RecentFiles recent(dir.filePath("recent.json"));
            recent.remember(a);
            recent.remember(b);
            QCOMPARE(paths(recent), (QStringList{b, a}));
            recent.remember(a);
            QCOMPARE(paths(recent), (QStringList{a, b}));
        }
        RecentFiles again(dir.filePath("recent.json"));
        QCOMPARE(paths(again), (QStringList{a, b}));
    }
    void aMissingFileIsNotListed() {
        QTemporaryDir dir;
        const QString a = touch(dir, "a.fugue");
        RecentFiles recent(dir.filePath("recent.json"));
        recent.remember(a);
        recent.remember(dir.filePath("gone.fugue"));
        QCOMPARE(paths(recent), (QStringList{a}));
    }
    void anOpenFileIsLeftOut() {
        QTemporaryDir dir;
        const QString a = touch(dir, "a.fugue"), b = touch(dir, "b.fugue");
        RecentFiles recent(dir.filePath("recent.json"));
        recent.remember(a);
        recent.remember(b);
        QSignalSpy spy(&recent, &RecentFiles::entriesChanged);
        recent.setOpen({b});
        QCOMPARE(paths(recent), (QStringList{a}));
        QCOMPARE(spy.count(), 1);
    }
    void theLimitDropsTheOldest() {
        QTemporaryDir dir;
        RecentFiles recent(dir.filePath("recent.json"), 2);
        const QString a = touch(dir, "a.fugue"), b = touch(dir, "b.fugue"), c = touch(dir, "c.fugue");
        recent.remember(a);
        recent.remember(b);
        recent.remember(c);
        QCOMPARE(paths(recent), (QStringList{c, b}));
    }
    void forgetDropsOne() {
        QTemporaryDir dir;
        const QString a = touch(dir, "a.fugue");
        RecentFiles recent(dir.filePath("recent.json"));
        recent.remember(a);
        recent.forget(a);
        QVERIFY(recent.entries().isEmpty());
    }
    void anEntryIsNamedWithoutItsSuffix() {
        QTemporaryDir dir;
        RecentFiles recent(dir.filePath("recent.json"));
        recent.remember(touch(dir, "my flow.fugue"));
        QCOMPARE(recent.entries().first().toMap().value("name").toString(), QString("my flow"));
    }
};

QTEST_MAIN(TestRecentFiles)
#include "test_recent_files.moc"
