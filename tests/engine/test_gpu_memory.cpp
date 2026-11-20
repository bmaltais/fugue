// Free memory for GPU work (spec 8.10), by Vulkan device type: a discrete card's budget minus
// usage, a unified-memory device's MemAvailable capped at the heap; and who holds GPU memory.
//
// Seams: memAvailableMb() on a meminfo file the test writes; freeGpuMemoryMb() on device figures;
// parseGpuProcesses() on nvidia-smi's output; the real probe and readings once, for sanity.

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

    void aDiscreteCardHasItsBudgetMinusWhatIsUsed() {
        GpuDevice card;
        card.kind = GpuDevice::Kind::Discrete;
        card.heapMb = 24576;
        card.budgetMb = 23000;
        card.usageMb = 9000;
        QCOMPARE(freeGpuMemoryMb(card, 100000), std::optional<qint64>(14000)); // system memory plays no part
        card.usageMb = 24000;
        QCOMPARE(freeGpuMemoryMb(card, 100000), std::optional<qint64>(0));
        card.budgetMb.reset(); // a driver without VK_EXT_memory_budget: not measured
        QCOMPARE(freeGpuMemoryMb(card, 100000), std::optional<qint64>());
    }

    void unifiedMemoryIsMemAvailableCappedAtTheHeap() {
        GpuDevice gb10;
        gb10.kind = GpuDevice::Kind::Unified;
        gb10.heapMb = 98304;
        gb10.budgetMb = 98304; // the budget does not see other processes: not used
        gb10.usageMb = 0;
        QCOMPARE(freeGpuMemoryMb(gb10, 40000), std::optional<qint64>(40000));
        QCOMPARE(freeGpuMemoryMb(gb10, 120000), std::optional<qint64>(98304));
        QCOMPARE(freeGpuMemoryMb(gb10, std::nullopt), std::optional<qint64>());
    }

    void aCpuDeviceIsNotMeasured() {
        GpuDevice llvmpipe;
        llvmpipe.kind = GpuDevice::Kind::Cpu;
        llvmpipe.heapMb = 32000;
        QCOMPARE(freeGpuMemoryMb(llvmpipe, 40000), std::optional<qint64>());
    }

    void theProcessesHoldingGpuMemoryAreNamed() {
        // nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader,nounits
        const QList<GpuProcess> processes = parseGpuProcesses("4242, /usr/bin/sd-server, 9876\n"
                                                              "77, python3, [N/A]\n"
                                                              "9, /opt/edge/msedge --type=gpu --x=a,b, 127\n"
                                                              "\n"
                                                              "garbage\n");
        QCOMPARE(processes.size(), 3);
        QCOMPARE(processes.at(2).name, QStringLiteral("msedge")); // its arguments, commas and all, are left out
        QCOMPARE(processes.at(2).usedMb, std::optional<qint64>(127));
        QCOMPARE(processes.at(0).pid, qint64(4242));
        QCOMPARE(processes.at(0).name, QStringLiteral("sd-server")); // the program, not its folder
        QCOMPARE(processes.at(0).usedMb, std::optional<qint64>(9876));
        QCOMPARE(processes.at(1).name, QStringLiteral("python3"));
        QCOMPARE(processes.at(1).usedMb, std::optional<qint64>()); // the GB10 reports no figure
        QCOMPARE(describeGpuProcesses(processes.mid(0, 2)), QStringLiteral("sd-server (pid 4242, 9.6 GB), python3 (pid 77)"));
    }

    void theRealProbeGivesAFigureOrNothing() {
        // Whatever this machine has: a GPU gives a figure no larger than its heap; none gives none.
        GpuMemoryProbe probe;
        const std::optional<GpuDevice> device = probe.device();
        if (!device)
            QSKIP("no Vulkan GPU here");
        QVERIFY(device->heapMb > 0);
        QVERIFY(!device->name.isEmpty());
        const std::optional<qint64> free = probe.freeMb();
        qInfo("%s: heap %lld MB, free %s MB", qPrintable(device->name), device->heapMb,
              free ? qPrintable(QString::number(*free)) : "not measured");
        if (device->kind != GpuDevice::Kind::Cpu && free) {
            QVERIFY(*free >= 0);
            QVERIFY(*free <= device->heapMb);
        }
    }

    void thisMachineHasAFigure() {
        const std::optional<qint64> free = memAvailableMb();
        QVERIFY(free.has_value());
        QVERIFY(*free > 0);
    }
};

QTEST_GUILESS_MAIN(TestGpuMemory)
#include "test_gpu_memory.moc"
