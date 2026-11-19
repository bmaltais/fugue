// The file roots of spec 13.6 (engine/file_roots.h): where a run's nodes may read and write.
//
// Seam: FileRoots and canonicalPath on a temporary folder with real links.

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/file_roots.h"
#include "engine/paths.h"

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

    // `..` after a link goes up from where the link leads, as the kernel follows it, not
    // back to the folder holding the link; nor does a missing folder before `..` hide a link.
    void dotDotAfterALinkIsJudgedWhereTheLinkLeads() {
        const FileRoots roots{{path("root")}, {path("root")}};
        const QString up = QFileInfo(QDir(m_elsewhere.path()).canonicalPath()).path();
        for (const char *name : {"root/out/../x", "root/new/../out/x", "root/out/../out/../x", "root/new/../out/../x"}) {
            QVERIFY2(!roots.allowsWrite(path(name)), name);
            QVERIFY2(!roots.allowsRead(path(name)), name);
        }
        QCOMPARE(canonicalPath(path("root/out/../x")), up + "/x");
        QCOMPARE(canonicalPath(path("root/new/../out/x")), QDir(m_elsewhere.path()).canonicalPath() + "/x");
        QVERIFY(roots.allowsWrite(path("root/new/../x"))); // a missing folder, then up: still inside
        QVERIFY(roots.allowsWrite(path("root/./inside/./x")));
    }

    // A link to a link, and a relative link, are followed to their end; a loop is not.
    void chainsOfLinksAreFollowedAndLoopsStop() {
        QVERIFY(QFile::link("out", path("root/relative"))); // relative to its own folder
        QVERIFY(QFile::link(path("root/relative"), path("root/chain")));
        QVERIFY(QFile::link(path("root/loop-b"), path("root/loop-a")));
        QVERIFY(QFile::link(path("root/loop-a"), path("root/loop-b")));
        const FileRoots roots{{path("root")}, {path("root")}};
        QVERIFY(!roots.allowsWrite(path("root/relative/x")));
        QVERIFY(!roots.allowsWrite(path("root/chain/x")));
        QVERIFY(!roots.allowsWrite(path("root/chain/../x")));
        QVERIFY(!roots.allowsWrite(path("root/loop-a/x"))); // cannot be resolved: not inside
    }

    void aRefusalNamesTheInputAndTheRoots() {
        const FileRoots roots{{path("root")}, {path("root"), path("other")}};
        const Result<QString> refused = roots.checkWrite(path("rootless/x"), "folder");
        QVERIFY(!refused.ok());
        QCOMPARE(refused.errors().size(), 1);
        QCOMPARE(refused.error().code, QString("permission_denied"));
        QCOMPARE(refused.error().input, QString("folder"));
        QVERIFY(refused.error().hint.contains(path("root")) && refused.error().hint.contains(path("other")));
        // Inside: the path to use, canonical.
        const Result<QString> readable = roots.checkRead(path("root/inside/./new/../x"), "file");
        QVERIFY(readable.ok());
        QCOMPARE(readable.value(), canonicalPath(path("root")) + "/inside/x");
    }

    void theDefaultsAreThoseOf13_6() {
        const FileRoots roots = FileRoots::defaults(path("cwd"));
        QVERIFY(roots.read.contains(QDir::homePath()));
        QVERIFY(roots.read.contains(inputsDir())); // the inputs folder, wherever the pictures folder is
        QVERIFY(roots.write.contains(path("cwd")));
        QVERIFY(roots.write.contains(QDir::homePath() + "/Documents/Fugue"));
        QCOMPARE(roots.write.size(), 3); // and the output folder, which holds the inputs folder
        QVERIFY(roots.allowsWrite(inputsDir() + "/x.png"));
        QVERIFY(FileRoots::defaults(path("cwd"), "/mnt/flows/a.fugue").allowsRead("/mnt/flows/inputs/x.png"));
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
