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

    // A `file` input's relative path is taken from the workflow's own folder, walked as the
    // kernel walks it; an absolute one (or "~/...") is not; an unsaved workflow has no folder.
    void aRelativeFilePathIsTakenFromTheWorkflowsFolder() {
        const QString workflow = path("root/flow.fugue");
        const Result<QString> beside = inputFilePath("inputs/a.png", workflow, "file");
        QVERIFY(beside.ok());
        QCOMPARE(beside.value(), canonicalPath(path("root")) + "/inputs/a.png");
        QCOMPARE(inputFilePath("  ./inside/../a.png ", workflow, "file").value(), canonicalPath(path("root")) + "/a.png");
        // `..` and links are resolved one component at a time, so the roots judge where it leads.
        QCOMPARE(inputFilePath("out/../x.png", workflow, "file").value(),
                 QFileInfo(QDir(m_elsewhere.path()).canonicalPath()).path() + "/x.png");
        QCOMPARE(inputFilePath("../rootless/x.png", workflow, "file").value(), canonicalPath(path("rootless")) + "/x.png");
        // Not relative: as written, "~" expanded.
        QCOMPARE(inputFilePath(path("rootless/x.png"), workflow, "file").value(), path("rootless/x.png"));
        QCOMPARE(inputFilePath("~/x.png", workflow, "file").value(), QDir::homePath() + "/x.png");
        QCOMPARE(inputFilePath("~/x.png", {}, "file").value(), QDir::homePath() + "/x.png");
        // No folder to start from: an error that says so, on the input.
        const Result<QString> unsaved = inputFilePath("inputs/a.png", {}, "file");
        QVERIFY(!unsaved.ok());
        QCOMPARE(unsaved.error().code, QString("file_not_found"));
        QCOMPARE(unsaved.error().input, QString("file"));
        QVERIFY2(unsaved.error().message.contains("inputs/a.png") && unsaved.error().message.contains("not saved"),
                 qPrintable(unsaved.error().message));
        QVERIFY(!unsaved.error().hint.isEmpty());
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
