// The remembered file hashes of spec 1.5: a file's hash is computed once and
// remembered by path, size and modification time.
//
// Seam: FileHashes (hash, remember, save, and a new instance over the same file).
// A remembered entry is made observable by remembering a hash that is not the
// file's: while path, size and time match, that is what comes back.

#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

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

    void defaultStoreIsUnderTheStateFolder() {
        QVERIFY(FileHashes::defaultPath().endsWith(QStringLiteral("/fugue/hashes.json")));
    }
};

QTEST_GUILESS_MAIN(TestFileHashes)
#include "test_file_hashes.moc"
