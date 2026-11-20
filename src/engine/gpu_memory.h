#pragma once

// Free memory for GPU work, measured by Fugue itself (spec 8.10, spikes/04), and who holds it.
//
// Which figure applies depends on the Vulkan device type: on a discrete card it is the
// device-local heap's budget minus usage (VK_EXT_memory_budget); on an integrated or
// unified-memory device it is MemAvailable from /proc/meminfo, capped at the heap size (on the
// GB10 the Vulkan budget does not see other processes). A CPU device is not measured.
//
// Vulkan is only asked about devices: no device is created, so nothing is loaded on the GPU.

#include <QList>
#include <QString>

#include <memory>
#include <optional>

namespace fugue {

// MemAvailable in MB, as the kernel estimates what a new workload can take without
// swapping. Nothing when the file cannot be read or has no such line.
std::optional<qint64> memAvailableMb(const QString &meminfoPath = QStringLiteral("/proc/meminfo"));

// A GPU as Vulkan describes it, with its memory figures at the time it was read.
struct GpuDevice {
    enum class Kind { Discrete, Unified, Cpu };
    QString name;
    Kind kind = Kind::Cpu;
    qint64 heapMb = 0; // the largest device-local heap
    // That heap's budget and usage (VK_EXT_memory_budget); none without the extension.
    std::optional<qint64> budgetMb;
    std::optional<qint64> usageMb;
};

// The free memory for GPU work on `device` in MB (8.10). Nothing when it cannot be told: a CPU
// device, a discrete card without a budget, unified memory without MemAvailable.
std::optional<qint64> freeGpuMemoryMb(const GpuDevice &device, std::optional<qint64> memAvailableMb);

// The GPU that GPU work runs on, read through Vulkan: the first discrete card, else the first
// integrated or virtual one. Vulkan is set up on first use and kept: the figures change, the
// device does not. Thread-safe.
class GpuMemoryProbe {
public:
    GpuMemoryProbe();
    ~GpuMemoryProbe();
    GpuMemoryProbe(const GpuMemoryProbe &) = delete;
    GpuMemoryProbe &operator=(const GpuMemoryProbe &) = delete;

    // The device with its figures now; nothing without Vulkan or a GPU.
    std::optional<GpuDevice> device() const;
    // freeGpuMemoryMb of device() now.
    std::optional<qint64> freeMb() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// A process holding GPU memory, as nvidia-smi names it.
struct GpuProcess {
    qint64 pid = 0;
    QString name;                 // the program's file name
    std::optional<qint64> usedMb; // none where the driver reports none (the GB10)
};

// Parses `nvidia-smi --query-compute-apps=pid,process_name,used_memory
// --format=csv,noheader,nounits`. Lines that do not read are left out.
QList<GpuProcess> parseGpuProcesses(const QByteArray &csv);
// Runs nvidia-smi (NVIDIA cards and the GB10); none when it is not there or fails.
QList<GpuProcess> gpuProcesses();
// "sd-server (pid 4242, 9.6 GB), python3 (pid 77)", for an error message.
QString describeGpuProcesses(const QList<GpuProcess> &processes);

} // namespace fugue
