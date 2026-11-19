// Writing a file whole or not at all (engine/atomic_file.h), with an explicit choice about
// replacing a file already at the name.
//
// Seam: PartialFile, on a temporary folder; files are read back from disk.

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

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
        QVERIFY(QFileInfo(first.path()).fileName().startsWith('.'));
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

    void aMissingFolderIsAnError() {
        PartialFile partial(path("no/such/folder"), "a.png");
        const Errors written = partial.write("x", path("no/such/folder/a.png"));
        QVERIFY(hasErrors(written));
        QCOMPARE(written.first().code, QString("permission_denied"));
    }
};

QTEST_GUILESS_MAIN(TestAtomicFile)
#include "test_atomic_file.moc"
