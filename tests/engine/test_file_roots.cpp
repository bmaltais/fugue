// The file roots of spec 13.6 (engine/file_roots.h): where a run's nodes may read and write.
//
// Seam: FileRoots and canonicalPath on a temporary folder with real links.

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/file_roots.h"

using namespace fugue;

class TestFileRoots : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QTemporaryDir m_elsewhere;

    QString path(const QString &name) const { return m_dir.filePath(name); }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid() && m_elsewhere.isValid());
        QVERIFY(QDir().mkpath(path("root/inside")));
        QVERIFY(QDir().mkpath(path("rootless")));
        QVERIFY(QFile::link(m_elsewhere.path(), path("root/out")));
    }

    void aPathInsideARootIsAllowedAndOneBesideItIsNot() {
        const FileRoots roots{{path("root")}, {path("root")}};
        QVERIFY(roots.allowsWrite(path("root")));
        QVERIFY(roots.allowsWrite(path("root/inside/new/file.png"))); // need not exist yet
        QVERIFY(!roots.allowsWrite(path("rootless/file.png")));       // a shared prefix is not inside
        QVERIFY(!roots.allowsRead(m_elsewhere.filePath("a.png")));
    }

    void dotDotAndLinksAreResolvedBeforeTheyAreJudged() {
        const FileRoots roots{{path("root")}, {path("root")}};
        QVERIFY(!roots.allowsWrite(path("root/inside/../../rootless/x.png")));
        QVERIFY(roots.allowsWrite(path("root/inside/../x.png")));
        QVERIFY(!roots.allowsWrite(path("root/out/x.png")));     // a link to elsewhere
        QVERIFY(!roots.allowsWrite(path("root/out/new/x.png"))); // and below it, not made yet
        QCOMPARE(canonicalPath(path("root/out/new")), QDir(m_elsewhere.path()).canonicalPath() + "/new");
    }

    void aRefusalNamesTheInputAndTheRoots() {
        const FileRoots roots{{path("root")}, {path("root"), path("other")}};
        const Errors refused = roots.checkWrite(path("rootless/x"), "folder");
        QCOMPARE(refused.size(), 1);
        QCOMPARE(refused.first().code, QString("permission_denied"));
        QCOMPARE(refused.first().input, QString("folder"));
        QVERIFY(refused.first().hint.contains(path("root")) && refused.first().hint.contains(path("other")));
        QVERIFY(roots.checkRead(path("root/inside"), "file").isEmpty());
    }

    void theDefaultsAreThoseOf13_6() {
        const FileRoots roots = FileRoots::defaults(path("cwd"));
        QCOMPARE(roots.read, QStringList{QDir::homePath()});
        QVERIFY(roots.write.contains(path("cwd")));
        QVERIFY(roots.write.contains(QDir::homePath() + "/Documents/Fugue"));
        QCOMPARE(roots.write.size(), 3); // and the output folder
    }

    void theyRoundTripThroughJson() {
        const FileRoots roots{{"/a"}, {"/b", "/c"}};
        const Result<FileRoots> back = FileRoots::fromJson(roots.toJson());
        QVERIFY(back.ok());
        QCOMPARE(back->read, roots.read);
        QCOMPARE(back->write, roots.write);
        QVERIFY(!FileRoots::fromJson(QJsonObject{{"read", 1}}).ok());
    }
};

QTEST_GUILESS_MAIN(TestFileRoots)
#include "test_file_roots.moc"
