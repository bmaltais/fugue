// Writing a file whole or not at all (engine/atomic_file.h), with an explicit choice about
// replacing a file already at the name.
//
// Seam: PartialFile, on a temporary folder; files are read back from disk.

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <fcntl.h>
#include <unistd.h>

#include "engine/atomic_file.h"

using namespace fugue;

namespace {

QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void writeAll(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
}

} // namespace

class TestAtomicFile : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }
    QStringList entries() const { return QDir(m_dir.path()).entryList(QDir::Files | QDir::Hidden); }

private slots:
    void init() {
        for (const QString &name : entries())
            QFile::remove(path(name));
    }

    void aCommittedFileHasTheContentAndNoPartialIsLeft() {
        PartialFile partial(m_dir.path(), "a.png");
        QVERIFY(!hasErrors(partial.write("content", path("a.png"))));
        const Result<bool> committed = partial.commit(path("a.png"), Overwrite::Never);
        QVERIFY(committed.ok() && *committed);
        QCOMPARE(readAll(path("a.png")), QByteArray("content"));
        QCOMPARE(entries(), QStringList{"a.png"});
    }

    void neverOverwritingKeepsTheFileAtTheNameAndAllowsAnotherName() {
        writeAll(path("a.png"), "the user's");
        PartialFile partial(m_dir.path(), "a.png");
        QVERIFY(!hasErrors(partial.write("new", path("a.png"))));
        const Result<bool> taken = partial.commit(path("a.png"), Overwrite::Never);
        QVERIFY(taken.ok());
        QVERIFY(!*taken);
        QCOMPARE(readAll(path("a.png")), QByteArray("the user's"));
        const Result<bool> other = partial.commit(path("a-2.png"), Overwrite::Never);
        QVERIFY(other.ok() && *other);
        QCOMPARE(readAll(path("a-2.png")), QByteArray("new"));
        QCOMPARE(readAll(path("a.png")), QByteArray("the user's"));
    }

    void replacingReplacesTheFileAtTheName() {
        writeAll(path("a.png"), "old");
        PartialFile partial(m_dir.path(), "a.png");
        QVERIFY(!hasErrors(partial.write("new", path("a.png"))));
        const Result<bool> committed = partial.commit(path("a.png"), Overwrite::Replace);
        QVERIFY(committed.ok() && *committed);
        QCOMPARE(readAll(path("a.png")), QByteArray("new"));
        QCOMPARE(entries(), QStringList{"a.png"});
    }

    // Two writers of one name at once each write their own partial file.
    void twoPartialFilesForOneNameAreTwoFiles() {
        const PartialFile first(m_dir.path(), "a.png");
        const PartialFile second(m_dir.path(), "a.png");
        QVERIFY(first.path() != second.path());
        QVERIFY(QFileInfo(first.path()).fileName().startsWith(".a.png.fugue-"));
        QVERIFY(QFileInfo(first.path()).fileName().endsWith(".part"));
        QCOMPARE(QFileInfo(first.path()).absolutePath(), QDir(m_dir.path()).absolutePath());
    }

    void aPartialFileThatIsNotCommittedIsRemoved() {
        QString written;
        {
            PartialFile partial(m_dir.path(), "a.png");
            QVERIFY(!hasErrors(partial.write("half", path("a.png"))));
            written = partial.path();
            QVERIFY(QFile::exists(written));
        }
        QVERIFY(!QFile::exists(written));
        QVERIFY(entries().isEmpty());
    }

    // A name as long as the filesystem allows (255 bytes) still gets a partial file: the
    // partial's own name is kept within the limit.
    void aNameAtTheLengthLimitIsWritten() {
        const QString name = QString(251, 'n') + ".png";
        PartialFile partial(m_dir.path(), name);
        QVERIFY(QFile::encodeName(QFileInfo(partial.path()).fileName()).size() <= 255);
        QVERIFY(!hasErrors(partial.write("long", path(name))));
        const Result<bool> committed = partial.commit(path(name), Overwrite::Never);
        QVERIFY2(committed.ok(), qPrintable(committed.ok() ? QString() : committed.error().message));
        QVERIFY(*committed);
        QCOMPARE(readAll(path(name)), QByteArray("long"));
        // Two at once still differ.
        const PartialFile a(m_dir.path(), name), b(m_dir.path(), name);
        QVERIFY(a.path() != b.path());
    }

    // A writer that died mid-save leaves its partial file; the next save to the folder removes
    // it. Only ours, and only when its writer is gone and it is old: a live writer's file,
    // a fresh one and anyone else's file stay.
    void staleOnesAreRemovedAndNothingElse() {
        const QDateTime old = QDateTime::currentDateTimeUtc().addSecs(-3600);
        const auto make = [&](const QString &name, const QDateTime &time) {
            writeAll(path(name), "x");
            QFile file(path(name));
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(time, QFileDevice::FileModificationTime));
        };
        const QString live = QString::number(QCoreApplication::applicationPid());
        make(".shot.png.fugue-999999999-3.part", old);                          // writer gone, old: stale
        make(".shot.png.fugue-999999999-4.part", QDateTime::currentDateTimeUtc()); // writer gone, fresh
        make(".shot.png.fugue-" + live + "-5.part", old);                       // writer alive
        make(".shot.png.999999999-6.part", old);                                // another program's form
        make(".notes.part", old);                                          // not ours
        make("shot.png", old);
        PartialFile::removeStale(m_dir.path());
        QStringList left = entries();
        left.sort();
        QStringList expected{".notes.part", ".shot.png.fugue-" + live + "-5.part", ".shot.png.fugue-999999999-4.part",
                             ".shot.png.999999999-6.part", "shot.png"};
        expected.sort();
        QCOMPARE(left, expected);
    }

    // A settings file a dotfile manager links in stays a link: its target gets the content.
    void replacingASymlinkedFileWritesItsTargetAndKeepsTheLink() {
        writeAll(path("real-providers.json"), "old");
        QVERIFY(QFile::link(path("real-providers.json"), path("providers.json")));
        QVERIFY(!hasErrors(replaceFile(path("providers.json"), "new", "providers.json", FileMode::OwnerOnly)));
        QVERIFY(QFileInfo(path("providers.json")).isSymLink());
        QCOMPARE(readAll(path("real-providers.json")), QByteArray("new"));
        QCOMPARE(readAll(path("providers.json")), QByteArray("new"));
        QStringList left = entries();
        left.sort();
        QCOMPARE(left, (QStringList{"providers.json", "real-providers.json"}));
    }

    void aMissingFolderIsAnError() {
        PartialFile partial(path("no/such/folder"), "a.png");
        const Errors written = partial.write("x", path("no/such/folder/a.png"));
        QVERIFY(hasErrors(written));
        QCOMPARE(written.first().code, QString("file_not_found"));
    }

    // #360: a folder swapped for a link to an outside folder after the check is refused, and
    // nothing is written outside.
    void aFolderSwappedForALinkAfterTheCheckIsRefused() {
        QTemporaryDir outside;
        QVERIFY(QDir(path("")).mkpath("out/sub"));
        static QString swap, away, outsidePath;
        swap = path("out/sub");
        away = path("out/sub-moved");
        outsidePath = outside.path();
        beforeFolderOpen = [](const QString &) {
            QVERIFY(QDir().rename(swap, away));
            QVERIFY(QFile::link(outsidePath, swap));
        };
        const Result<FolderFd> dir = FolderFd::open(path("out/sub"), true);
        beforeFolderOpen = nullptr;
        QVERIFY(!dir.ok());
        QCOMPARE(dir.errors().first().code, QString("permission_denied"));
        QVERIFY(QDir(outside.path()).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot));
    }

    // A link swapped in for a subfolder after the base was opened cannot lead the write out
    // either, and a write through the open folder stays in the folder that was opened.
    void aWriteThroughTheOpenFolderStaysInIt() {
        QTemporaryDir outside;
        QVERIFY(QDir(path("")).mkpath("o2/sub"));
        const Result<FolderFd> base = FolderFd::open(path("o2"), true);
        QVERIFY(base.ok());
        QVERIFY(QDir().rename(path("o2/sub"), path("o2/sub-moved")));
        QVERIFY(QFile::link(outside.path(), path("o2/sub")));
        QVERIFY(!base->below("sub", true).ok());

        const Result<FolderFd> opened = base->below("made/deeper", true);
        QVERIFY(opened.ok());
        QVERIFY(QDir().rename(path("o2/made"), path("o2/made-moved")));
        QVERIFY(QFile::link(outside.path(), path("o2/made")));
        PartialFile partial(opened.value(), "a.png");
        QVERIFY(!hasErrors(partial.write("content", "a.png")));
        const Result<bool> committed = partial.commit("a.png", Overwrite::Never);
        QVERIFY(committed.ok() && *committed);
        QVERIFY(QFile::exists(path("o2/made-moved/deeper/a.png")));
        QVERIFY(QDir(outside.path()).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot));
    }

    // A link (or file) planted at the partial file's name is never written through: the partial file is
    // made exclusively, under another name when that one is taken, for write() and for a program that writes path() (ffmpeg).
    void aLinkPlantedAtThePartialNameIsNotWrittenThrough() {
        QTemporaryDir outside;
        writeAll(outside.filePath("victim"), "keep");
        const Result<FolderFd> dir = FolderFd::open(path("o3"), true);
        QVERIFY(dir.ok());
        for (const bool viaCreate : {false, true}) {
            PartialFile partial(dir.value(), "a.png");
            QVERIFY(QFile::link(outside.filePath("victim"), path("o3/" + partial.name())));
            const Errors made = viaCreate ? partial.create() : partial.write("x", "a.png");
            QVERIFY(!hasErrors(made)); // another name was taken
            QCOMPARE(readAll(outside.filePath("victim")), QByteArray("keep"));
            QVERIFY(!QFileInfo::exists(path("o3/" + partial.name())) || !QFileInfo(path("o3/" + partial.name())).isSymLink());
        }
    }

    void partsThatAreNotPlainNamesAreRefused() {
        QVERIFY(QDir(path("")).mkpath("o4/a"));
        const Result<FolderFd> base = FolderFd::open(path("o4"), false);
        QVERIFY(base.ok());
        for (const char *bad : {"..", "a/../..", ".", "a//b", ""})
            QVERIFY2(!base->below(bad, true).ok(), bad);
        QVERIFY(!FolderFd::open(path("o4/a/.."), false).ok());
    }

    // Walking needs only the search permission of the folders above, and a write-only folder takes a file.
    void aFolderBehindSearchOnlyAndWriteOnlyFoldersIsWritten() {
        QVERIFY(QDir(path("")).mkpath("o5/x/t"));
        QVERIFY(QFile::setPermissions(path("o5/x/t"), QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QVERIFY(QFile::setPermissions(path("o5/x"), QFileDevice::ExeOwner));
        const Result<FolderFd> dir = FolderFd::open(path("o5/x/t"), true);
        bool written = false;
        if (dir.ok()) {
            PartialFile partial(dir.value(), "a.png");
            written = !hasErrors(partial.write("c", "a.png")) && partial.commit("a.png", Overwrite::Never).ok();
        }
        QFile::setPermissions(path("o5/x"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QFile::setPermissions(path("o5/x/t"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QVERIFY(written);
        QCOMPARE(readAll(path("o5/x/t/a.png")), QByteArray("c"));
    }

    // #414: where a rename that never replaces is missing (NFS), the partial file is closed before its
    // name is unlinked, so no `.nfsXXXX` is left behind. The order is observed through a seam, as the
    // test has no NFS.
    void theLinkFallbackClosesThePartialFileBeforeUnlinkingIt() {
        const Result<FolderFd> dir = FolderFd::open(path("nfs"), true);
        QVERIFY(dir.ok());
        PartialFile partial(dir.value(), "a.png");
        QVERIFY(!hasErrors(partial.write("c", "a.png")));
        static int closed = 0, seen = -2;
        closed = 0;
        seen = -2;
        beforeUnlinkPartial = [](int fd) {
            seen = fd;
            errno = 0;
            closed = (fd >= 0 && ::fcntl(fd, F_GETFD) == -1 && errno == EBADF) ? 1 : 0;
        };
        forceLinkFallback = true;
        const Result<bool> committed = partial.commit("a.png", Overwrite::Never);
        forceLinkFallback = false;
        beforeUnlinkPartial = nullptr;
        QVERIFY(committed.ok() && *committed);
        QVERIFY(seen >= 0);
        QCOMPARE(closed, 1);
        QCOMPARE(QDir(path("nfs")).entryList(QDir::Files | QDir::Hidden), QStringList{"a.png"});
    }

    // #414: in a folder with no read permission the stale partial files cannot be listed, so none is removed
    // (stated in the spec); the save itself still works.
    void staleFilesStayInAWriteOnlyFolder() {
        QVERIFY(QDir(path("")).mkpath("wo"));
        const QString stale = path("wo/.a.png.fugue-999999-1.part");
        writeAll(stale, "x");
        QProcess::execute("touch", {"-d", "2020-01-01", stale});
        QVERIFY(QFile::setPermissions(path("wo"), QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const Result<FolderFd> dir = FolderFd::open(path("wo"), false);
        bool saved = false;
        if (dir.ok()) {
            PartialFile::removeStale(dir.value());
            PartialFile partial(dir.value(), "a.png");
            saved = !hasErrors(partial.write("c", "a.png")) && partial.commit("a.png", Overwrite::Never).ok();
        }
        QFile::setPermissions(path("wo"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QVERIFY(saved);
        QVERIFY(QFileInfo::exists(stale));
    }

    void foldersAreMadeAndAnExistingNameIsKept() {
        const Result<FolderFd> dir = FolderFd::open(path("a/b"), true);
        QVERIFY(dir.ok());
        QVERIFY(QFileInfo(path("a/b")).isDir());
        PartialFile first(dir.value(), "x.png"), second(dir.value(), "x.png");
        QVERIFY(!hasErrors(first.write("1", "x.png")) && !hasErrors(second.write("2", "x.png")));
        QVERIFY(*first.commit("x.png", Overwrite::Never));
        QVERIFY(!*second.commit("x.png", Overwrite::Never));
        QCOMPARE(readAll(path("a/b/x.png")), QByteArray("1"));
        QVERIFY(!FolderFd::open(path("a/b/x.png"), false).ok());
        QVERIFY(!FolderFd::open(path("none/there"), false).ok());
    }
};

QTEST_GUILESS_MAIN(TestAtomicFile)
#include "test_atomic_file.moc"
