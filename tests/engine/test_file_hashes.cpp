// The remembered file hashes of spec 1.5: a file's hash is computed once and
// remembered by path, size and modification time.
//
// Seam: FileHashes (hash, remember, save, and a new instance over the same file).
// A remembered entry is made observable by remembering a hash that is not the
// file's: while path, size and time match, that is what comes back.

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

#include "engine/file_hashes.h"
#include "engine/hash.h"

using namespace fugue;

namespace {

const QString planted = QStringLiteral("sha256:0000000000000000000000000000000000000000000000000000000000000000");

void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
}

void setTime(const QString &path, const QDateTime &time) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(time, QFileDevice::FileModificationTime));
}

} // namespace

class TestFileHashes : public QObject {
    Q_OBJECT

private slots:
    void hashIsTheFilesContentHash() {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.bin");
        writeFile(path, "hello");
        FileHashes hashes(dir.filePath("hashes.json"));
        QCOMPARE(hashes.hash(path), sha256("hello"));
        QCOMPARE(hashes.hash(dir.filePath("missing")), QString());
    }

    void anUnchangedFileIsNotHashedAgain() {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.bin");
        writeFile(path, "hello");
        FileHashes hashes(dir.filePath("hashes.json"));
        hashes.remember(path, planted);
        QCOMPARE(hashes.hash(path), planted);
    }

    void aChangedSizeOrTimeIsHashedAgain() {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.bin");
        const QDateTime time = QDateTime::fromSecsSinceEpoch(1700000000);
        writeFile(path, "hello");
        setTime(path, time);
        FileHashes hashes(dir.filePath("hashes.json"));

        hashes.remember(path, planted);
        writeFile(path, "hello!");
        setTime(path, time);
        QCOMPARE(hashes.hash(path), sha256("hello!"));

        hashes.remember(path, planted);
        writeFile(path, "HELLO!"); // same size
        setTime(path, time.addMSecs(1));
        QCOMPARE(hashes.hash(path), sha256("HELLO!"));
    }

    void savedHashesAreRememberedByTheNextProcess() {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.bin");
        writeFile(path, "hello");
        const QString store = dir.filePath("state/fugue/hashes.json");
        {
            FileHashes hashes(store);
            hashes.remember(path, planted);
            QVERIFY(hashes.save());
        }
        FileHashes later(store);
        QCOMPARE(later.hash(path), planted);
    }

    void savingKeepsWhatAnotherProcessSaved() {
        QTemporaryDir dir;
        const QString a = dir.filePath("a.bin");
        const QString b = dir.filePath("b.bin");
        writeFile(a, "a");
        writeFile(b, "b");
        const QString store = dir.filePath("hashes.json");
        FileHashes first(store);
        FileHashes second(store);
        first.remember(a, planted);
        second.remember(b, planted);
        QVERIFY(first.save());
        QVERIFY(second.save());
        FileHashes later(store);
        QCOMPARE(later.hash(a), planted);
        QCOMPARE(later.hash(b), planted);
    }

    void aHashAnotherProcessSavedIsSeenByOneAlreadyOpen() {
        // The window's run thread keeps its store open for the life of the window; a model the
        // canvas hashed meanwhile must not be read again when a run reaches it.
        QTemporaryDir dir;
        const QString path = dir.filePath("model.safetensors");
        writeFile(path, "weights");
        const QString store = dir.filePath("hashes.json");
        FileHashes running(store);
        QCOMPARE(running.known(path), QString());
        FileHashes canvas(store);
        canvas.remember(path, planted);
        QVERIFY(canvas.save());
        QCOMPARE(running.known(path), planted);
        QCOMPARE(running.hash(path), planted);
    }

    void hashingALargeFileReportsProgressAndCanStop() {
        QTemporaryDir dir;
        const QString path = dir.filePath("model.safetensors");
        const QByteArray bytes(5 * 1024 * 1024 + 7, 'x');
        writeFile(path, bytes);
        FileHashes hashes(dir.filePath("hashes.json"));
        QList<qint64> seen;
        qint64 total = 0;
        const QString hash = hashes.hash(path, [&](qint64 done, qint64 of) {
            seen << done;
            total = of;
            return true;
        });
        QCOMPARE(hash, sha256(bytes));
        QCOMPARE(total, qint64(bytes.size()));
        QVERIFY(seen.size() > 1);
        QCOMPARE(seen.last(), qint64(bytes.size()));
        QVERIFY(std::is_sorted(seen.begin(), seen.end()));

        // Stopped part way: no hash, and nothing remembered.
        const QString other = dir.filePath("other.safetensors");
        writeFile(other, bytes + "y");
        QCOMPARE(hashes.hash(other, [](qint64 done, qint64) { return done == 0; }), QString());
        QCOMPARE(hashes.known(other), QString());
    }

    void aDamagedStoreIsIgnored() {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.bin");
        writeFile(path, "hello");
        const QString store = dir.filePath("hashes.json");
        writeFile(store, "{not json");
        FileHashes hashes(store);
        QCOMPARE(hashes.hash(path), sha256("hello"));
        QVERIFY(hashes.save());
    }

    // Inheriting the Default's remembered hashes (spec 1.4.2, models): read-only, only for files
    // under the inherited roots, and only while size and time still match.
    void anInheritedHashIsUsedWithoutReadingTheFile() {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("models")));
        const QString path = dir.filePath("models/a.bin");
        writeFile(path, "hello");
        {
            FileHashes base(dir.filePath("base.json"));
            base.remember(path, planted);
            QVERIFY(base.save());
        }
        FileHashes own(dir.filePath("own.json"), {dir.filePath("base.json"), {dir.filePath("models")}});
        QCOMPARE(own.known(path), planted);
        QCOMPARE(own.hash(path), planted);
    }

    void anInheritedHashOfAChangedFileIsNotTrusted() {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("models")));
        const QString path = dir.filePath("models/a.bin");
        writeFile(path, "hello");
        FileHashes base(dir.filePath("base.json"));
        base.remember(path, planted);
        QVERIFY(base.save());
        FileHashes sameSize(dir.filePath("own.json"), {dir.filePath("base.json"), {dir.filePath("models")}});
        setTime(path, QDateTime::currentDateTime().addDays(-3));
        QCOMPARE(sameSize.known(path), QString());
        QCOMPARE(sameSize.hash(path), sha256("hello"));
        writeFile(path, "hello world");
        FileHashes bigger(dir.filePath("own2.json"), {dir.filePath("base.json"), {dir.filePath("models")}});
        QCOMPARE(bigger.hash(path), sha256("hello world"));
    }

    void anInheritedHashOutsideTheInheritedRootsIsIgnored() {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("models")));
        QVERIFY(QDir().mkpath(dir.filePath("modelsX")));
        const QString outside = dir.filePath("modelsX/a.bin"); // a sibling whose name starts like the root
        const QString elsewhere = dir.filePath("input.png");
        writeFile(outside, "hello");
        writeFile(elsewhere, "image");
        FileHashes base(dir.filePath("base.json"));
        base.remember(outside, planted);
        base.remember(elsewhere, planted);
        QVERIFY(base.save());
        FileHashes own(dir.filePath("own.json"), {dir.filePath("base.json"), {dir.filePath("models")}});
        QCOMPARE(own.known(outside), QString());
        QCOMPARE(own.known(elsewhere), QString());
    }

    void inheritedHashesAreNeverWrittenAndOwnOnesWin() {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("models")));
        const QString path = dir.filePath("models/a.bin");
        const QString fresh = dir.filePath("models/b.bin");
        writeFile(path, "hello");
        writeFile(fresh, "fresh");
        FileHashes base(dir.filePath("base.json"));
        base.remember(path, planted);
        QVERIFY(base.save());
        const QByteArray before = [&] { QFile f(dir.filePath("base.json")); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }();
        FileHashes own(dir.filePath("own.json"), {dir.filePath("base.json"), {dir.filePath("models")}});
        QCOMPARE(own.hash(fresh), sha256("fresh")); // a new hash goes to the project's own store
        QVERIFY(own.save());
        QCOMPARE(FileHashes(dir.filePath("own.json")).known(fresh), sha256("fresh"));
        QCOMPARE(FileHashes(dir.filePath("own.json")).known(path), QString()); // the inherited one was not copied
        QFile f(dir.filePath("base.json"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), before);
        const QString other = QStringLiteral("sha256:1111111111111111111111111111111111111111111111111111111111111111");
        own.remember(path, other);
        QCOMPARE(own.hash(path), other);
    }

    void noInheritedRootsMeansNoInheritance() {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.bin");
        writeFile(path, "hello");
        FileHashes base(dir.filePath("base.json"));
        base.remember(path, planted);
        QVERIFY(base.save());
        FileHashes own(dir.filePath("own.json"), {dir.filePath("base.json"), {}});
        QCOMPARE(own.known(path), QString());
    }

    void defaultStoreIsUnderTheStateFolder() {
        QVERIFY(FileHashes::defaultPath().endsWith(QStringLiteral("/fugue/hashes.json")));
    }
};

QTEST_GUILESS_MAIN(TestFileHashes)
#include "test_file_hashes.moc"
