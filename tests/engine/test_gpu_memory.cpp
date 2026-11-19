// Free memory for GPU work on a unified-memory device (spec 8.10): MemAvailable.
//
// Seam: memAvailableMb() on a meminfo file the test writes, and once on the real one.

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/gpu_memory.h"

using namespace fugue;

class TestGpuMemory : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QString meminfo(const QByteArray &text) const {
        const QString path = m_dir.filePath("meminfo");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            qFatal("cannot write the meminfo fixture");
        file.write(text);
        return path;
    }

private slots:
    void memAvailableIsReadInMegabytes() {
        const QString path = meminfo("MemTotal:       131811768 kB\n"
                                     "MemFree:         2097152 kB\n"
                                     "MemAvailable:   104857600 kB\n"
                                     "Buffers:          123456 kB\n");
        QCOMPARE(memAvailableMb(path), std::optional<qint64>(102400)); // 100 GiB, not MemFree's 2
    }

    void aFileWithoutTheFigureGivesNothing() {
        QCOMPARE(memAvailableMb(meminfo("MemTotal: 1024 kB\nMemFree: 512 kB\n")), std::optional<qint64>());
        QCOMPARE(memAvailableMb(meminfo("MemAvailable: lots kB\n")), std::optional<qint64>());
        QCOMPARE(memAvailableMb(meminfo("MemAvailable: 1024\n")), std::optional<qint64>()); // no unit: not trusted
        QCOMPARE(memAvailableMb(m_dir.filePath("missing")), std::optional<qint64>());
    }

    void thisMachineHasAFigure() {
        const std::optional<qint64> free = memAvailableMb();
        QVERIFY(free.has_value());
        QVERIFY(*free > 0);
    }
};

QTEST_GUILESS_MAIN(TestGpuMemory)
#include "test_gpu_memory.moc"
