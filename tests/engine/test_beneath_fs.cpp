// File operations by descriptor on folders a sandboxed program can write in (engine/beneath_fs.h, spec 6.7.1, #731): what a
// hostile program does to its exchange folder (links, FIFOs, a folder swapped for a link while Fugue copies) never makes Fugue
// read, write or delete anything outside it.
//
// Seam: the functions of engine/beneath_fs.h on a scratch folder; the race runs a thread that swaps a folder and a link with
// renameat2(RENAME_EXCHANGE) while the copies and removals run.

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <cstdio>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine/beneath_fs.h"

using namespace fugue;

namespace {

void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        qFatal("cannot write %s", qPrintable(path));
    file.write(bytes);
}

QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

beneath::Fd folder(const QString &path) { return beneath::Fd(::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)); }

} // namespace

class TestBeneathFs : public QObject {
    Q_OBJECT

    QTemporaryDir m_root;
    QString path(const QString &name) const { return QFileInfo(m_root.path()).canonicalFilePath() + "/" + name; }

private slots:
    void initTestCase() {
        QVERIFY(m_root.isValid());
        if (!beneath::available())
            QSKIP("openat2 is not available here (Linux 5.6 or newer)");
    }

    void anOpenNeverFollowsALinkNorLeavesTheFolder() {
        write(path("open/in/file"), "inside");
        write(path("open-outside/secret"), "SECRET");
        QVERIFY(QFile::link(path("open-outside"), path("open/in/link")));
        QVERIFY(QFile::link(path("open-outside/secret"), path("open/in/filelink")));
        const beneath::Fd root = folder(path("open"));
        QVERIFY(beneath::openBeneath(root.get(), "in/file", O_RDONLY));
        for (const char *bad : {"in/link/secret", "in/filelink", "../open-outside/secret", "in/../../open-outside/secret", "/etc/passwd", ""})
            QVERIFY2(!beneath::openBeneath(root.get(), bad, O_RDONLY), bad);
        QVERIFY(beneath::openNoLinks(path("open/in/file"), O_RDONLY));
        QVERIFY(!beneath::openNoLinks(path("open/in/link/secret"), O_RDONLY));
        QVERIFY(!beneath::openNoLinks("relative/path", O_RDONLY));
        const beneath::Fd opened = beneath::openNoLinks(path("open/in/file"), O_PATH);
        QCOMPARE(beneath::realPathOf(opened.get()), path("open/in/file"));
    }

    void aRandomFolderIsNewAndPrivate() {
        QDir().mkpath(path("random"));
        const beneath::Fd root = folder(path("random"));
        const QString first = beneath::mkdirBeneathRandom(root.get());
        const QString second = beneath::mkdirBeneathRandom(root.get());
        QCOMPARE(first.size(), 32); // 128 bits in hex
        QVERIFY(first != second);
        QVERIFY(QRegularExpression("^[0-9a-f]{32}$").match(first).hasMatch());
        struct stat info {};
        QVERIFY(::stat(QFile::encodeName(path("random/" + first)).constData(), &info) == 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0700));
        QVERIFY(beneath::mkdirBeneath(root.get(), "work"));
        QVERIFY(!beneath::mkdirBeneath(root.get(), "work")); // never one that is there
        QVERIFY(!beneath::mkdirBeneath(root.get(), "a/b"));
    }

    // Copy-in makes a new file only; copy-out takes a regular file beneath the folder only (never a link, a folder or a FIFO,
    // which would hang a blocking open).
    void copiesTakeAndMakeRegularFilesOnly() {
        write(path("copy/src.png"), "picture");
        write(path("copy/ex/work/out.png"), "made");
        write(path("copy/ex/work/sub/deep.png"), "deep");
        write(path("copy-outside/secret"), "SECRET");
        QDir().mkpath(path("copy/dest"));
        QVERIFY(QFile::link(path("copy-outside/secret"), path("copy/ex/work/link.png")));
        QVERIFY(QFile::link(path("copy-outside"), path("copy/ex/work/away")));
        QVERIFY(::mkfifo(QFile::encodeName(path("copy/ex/work/fifo.png")).constData(), 0600) == 0);
        const beneath::Fd ex = folder(path("copy/ex"));
        const beneath::Fd dest = folder(path("copy/dest"));
        const beneath::Fd source = beneath::openRegularFile(path("copy/src.png"));
        QVERIFY(source);
        QVERIFY(beneath::mkdirBeneath(ex.get(), "in"));
        QVERIFY(beneath::copyInBeneath(source.get(), ex.get(), "in/1-src.png"));
        QCOMPARE(readAll(path("copy/ex/in/1-src.png")), QByteArray("picture"));
        QVERIFY(!beneath::copyInBeneath(source.get(), ex.get(), "in/1-src.png")); // never over a file that is there
        QVERIFY(!beneath::openRegularFile(path("copy/ex/work/fifo.png")));
        QVERIFY(!beneath::openRegularFile(path("copy/ex/work/link.png")));

        const beneath::Fd work = beneath::openBeneath(ex.get(), "work", O_RDONLY | O_DIRECTORY);
        QVERIFY(beneath::copyOutBeneath(work.get(), "out.png", dest.get(), "out.png"));
        QCOMPARE(readAll(path("copy/dest/out.png")), QByteArray("made"));
        QVERIFY(beneath::copyOutBeneath(work.get(), "sub/deep.png", dest.get(), "deep.png"));
        for (const char *bad : {"link.png", "away/secret", "fifo.png", "sub", "../in/1-src.png", "/etc/passwd"})
            QVERIFY2(!beneath::copyOutBeneath(work.get(), bad, dest.get(), "bad"), bad);
        QVERIFY(!beneath::copyOutBeneath(work.get(), "out.png", dest.get(), "out.png")); // never over a file that is there
        QVERIFY(!beneath::copyOutBeneath(work.get(), "out.png", dest.get(), "../escape"));
        QVERIFY(!QFileInfo::exists(path("copy/dest/bad")));
        QCOMPARE(readAll(path("copy-outside/secret")), QByteArray("SECRET"));
    }

    // A tree with links in it goes, links as links: what they point at stays.
    void removingATreeNeverFollowsALink() {
        write(path("remove/tree/a/b/c.txt"), "c");
        write(path("remove-outside/keep/file"), "KEEP");
        QVERIFY(QFile::link(path("remove-outside"), path("remove/tree/a/away")));
        QVERIFY(QFile::link(path("remove-outside/keep/file"), path("remove/tree/filelink")));
        QVERIFY(::mkfifo(QFile::encodeName(path("remove/tree/fifo")).constData(), 0600) == 0);
        const beneath::Fd root = folder(path("remove"));
        QVERIFY(beneath::removeTreeBeneath(root.get(), "tree"));
        QVERIFY(!QFileInfo::exists(path("remove/tree")));
        QCOMPARE(readAll(path("remove-outside/keep/file")), QByteArray("KEEP"));
        QVERIFY(beneath::removeTreeBeneath(root.get(), "tree")); // nothing there: nothing left
        QVERIFY(!beneath::removeTreeBeneath(root.get(), "../remove-outside"));
        QVERIFY(QFileInfo::exists(path("remove-outside/keep/file")));
    }

    // The race the review found: while Fugue copies `d/f` out and removes trees, a program swaps the folder `d` and a link to a
    // folder outside, over and over. A copy either reads the file inside or fails; it never reads the one outside, and a
    // removal never deletes outside.
    void aFolderSwappedForALinkMidwayNeverLeadsOut() {
        write(path("race/ex/d/f"), "inside");
        write(path("race-outside/f"), "SECRET");
        write(path("race-outside/keep"), "KEEP");
        QVERIFY(QFile::link(path("race-outside"), path("race/ex/l")));
        QDir().mkpath(path("race/dest"));
        const beneath::Fd ex = folder(path("race/ex"));
        const beneath::Fd dest = folder(path("race/dest"));
        std::atomic<bool> stop{false};
        std::atomic<int> swaps{0};
        const int exFd = ex.get();
        std::thread swapper([&] {
            while (!stop.load()) {
                if (::renameat2(exFd, "d", exFd, "l", RENAME_EXCHANGE) == 0)
                    ++swaps;
            }
        });
        int copied = 0;
        QElapsedTimer clock;
        clock.start();
        for (int i = 0; clock.elapsed() < 700; ++i) {
            const QString name = QStringLiteral("c%1").arg(i);
            if (beneath::copyOutBeneath(ex.get(), "d/f", dest.get(), name)) {
                ++copied;
                QCOMPARE(readAll(path("race/dest/" + name)), QByteArray("inside"));
            }
            // A tree made and removed through the swapping name: nothing outside goes.
            beneath::removeTreeBeneath(ex.get(), "d/never-there");
        }
        stop = true;
        swapper.join();
        QVERIFY2(swaps.load() > 100, qPrintable(QString::number(swaps.load())));
        QVERIFY2(copied > 0, "no copy got through while the folder was in place");
        QCOMPARE(readAll(path("race-outside/f")), QByteArray("SECRET"));
        QCOMPARE(readAll(path("race-outside/keep")), QByteArray("KEEP"));

        // The whole tree removed while the swapping goes on: the link goes as a link, what it points at stays.
        stop = false;
        std::thread again([&] {
            while (!stop.load())
                ::renameat2(exFd, "d", exFd, "l", RENAME_EXCHANGE);
        });
        const beneath::Fd root = folder(path("race"));
        QTRY_VERIFY_WITH_TIMEOUT(beneath::removeTreeBeneath(root.get(), "ex"), 2000);
        stop = true;
        again.join();
        QCOMPARE(readAll(path("race-outside/f")), QByteArray("SECRET"));
        QCOMPARE(readAll(path("race-outside/keep")), QByteArray("KEEP"));
    }
};

QTEST_GUILESS_MAIN(TestBeneathFs)
#include "test_beneath_fs.moc"
