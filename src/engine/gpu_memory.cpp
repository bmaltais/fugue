#include "gpu_memory.h"

#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QProcess>
#include <QStringList>

#include <algorithm>
#include <vector>

#include <vulkan/vulkan.h>

namespace fugue {

std::optional<qint64> memAvailableMb(const QString &meminfoPath) {
    QFile file(meminfoPath);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    // "MemAvailable:   123456789 kB"
    for (const QByteArray &line : file.readAll().split('\n')) {
        if (!line.startsWith("MemAvailable:"))
            continue;
        const QList<QByteArray> fields = line.mid(int(sizeof("MemAvailable:")) - 1).simplified().split(' ');
        bool ok = false;
        const qint64 kilobytes = fields.value(0).toLongLong(&ok);
        if (!ok || kilobytes < 0 || fields.value(1) != "kB")
            return std::nullopt;
        return kilobytes / 1024;
    }
    return std::nullopt;
}

std::optional<qint64> freeGpuMemoryMb(const GpuDevice &device, std::optional<qint64> memAvailable) {
    switch (device.kind) {
    case GpuDevice::Kind::Discrete:
        if (!device.budgetMb || !device.usageMb)
            return std::nullopt;
        return std::max<qint64>(0, *device.budgetMb - *device.usageMb);
    case GpuDevice::Kind::Unified:
        if (!memAvailable)
            return std::nullopt;
        return std::min(*memAvailable, device.heapMb);
    case GpuDevice::Kind::Cpu:
        break;
    }
    return std::nullopt;
}

namespace {

constexpr qint64 kMiB = 1024 * 1024;

bool hasExtension(VkPhysicalDevice device, const char *name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()) != VK_SUCCESS)
        return false;
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](const VkExtensionProperties &e) { return qstrcmp(e.extensionName, name) == 0; });
}

GpuDevice::Kind kindOf(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return GpuDevice::Kind::Discrete;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return GpuDevice::Kind::Unified;
    default:
        return GpuDevice::Kind::Cpu;
    }
}

} // namespace

struct GpuMemoryProbe::Impl {
    QMutex mutex;
    bool tried = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    bool budget = false;

    // Under `mutex`. Once: a machine without Vulkan or a GPU is not asked again.
    void setUp() {
        if (tried)
            return;
        tried = true;
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "fugue";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
            instance = VK_NULL_HANDLE;
            return;
        }
        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS)
            return;
        std::vector<VkPhysicalDevice> devices(count);
        if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS)
            return;
        VkPhysicalDevice integrated = VK_NULL_HANDLE;
        for (VkPhysicalDevice each : devices) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(each, &properties);
            const GpuDevice::Kind kind = kindOf(properties.deviceType);
            if (kind == GpuDevice::Kind::Discrete) {
                device = each;
                break;
            }
            if (!integrated && kind == GpuDevice::Kind::Unified)
                integrated = each;
        }
        if (!device)
            device = integrated;
        if (device) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(device, &properties);
            budget = properties.apiVersion >= VK_API_VERSION_1_1 &&
                     hasExtension(device, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        }
    }

    std::optional<GpuDevice> read() {
        QMutexLocker lock(&mutex);
        setUp();
        if (!device)
            return std::nullopt;
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);
        GpuDevice out;
        out.name = QString::fromUtf8(properties.deviceName);
        out.kind = kindOf(properties.deviceType);

        VkPhysicalDeviceMemoryBudgetPropertiesEXT budgets{};
        budgets.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        VkPhysicalDeviceMemoryProperties2 memory{};
        memory.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        if (budget) {
            memory.pNext = &budgets;
            vkGetPhysicalDeviceMemoryProperties2(device, &memory);
        } else {
            vkGetPhysicalDeviceMemoryProperties(device, &memory.memoryProperties);
        }
        const VkPhysicalDeviceMemoryProperties &heaps = memory.memoryProperties;
        int largest = -1;
        for (uint32_t i = 0; i < heaps.memoryHeapCount; ++i)
            if ((heaps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
                (largest < 0 || heaps.memoryHeaps[i].size > heaps.memoryHeaps[largest].size))
                largest = int(i);
        if (largest < 0)
            return std::nullopt;
        out.heapMb = qint64(heaps.memoryHeaps[largest].size / kMiB);
        if (budget) {
            out.budgetMb = qint64(budgets.heapBudget[largest] / kMiB);
            out.usageMb = qint64(budgets.heapUsage[largest] / kMiB);
        }
        return out;
    }
};

GpuMemoryProbe::GpuMemoryProbe() : d(new Impl) {}

// The instance is not destroyed: the probe lives as long as its process, and NVIDIA's driver can
// crash in its teardown (#82, helper/src/main.cpp). The process's end gives it back.
GpuMemoryProbe::~GpuMemoryProbe() = default;

std::optional<GpuDevice> GpuMemoryProbe::device() const { return d->read(); }

std::optional<qint64> GpuMemoryProbe::freeMb() const {
    const std::optional<GpuDevice> now = device();
    if (!now)
        return std::nullopt;
    return freeGpuMemoryMb(*now, now->kind == GpuDevice::Kind::Unified ? memAvailableMb() : std::nullopt);
}

QList<GpuProcess> parseGpuProcesses(const QByteArray &csv) {
    QList<GpuProcess> out;
    for (const QByteArray &line : csv.split('\n')) {
        // The name is the command line, which may hold commas: pid first, memory last.
        const int first = line.indexOf(',');
        const int last = line.lastIndexOf(',');
        if (first < 0 || last == first)
            continue;
        bool ok = false;
        GpuProcess process;
        process.pid = line.left(first).trimmed().toLongLong(&ok);
        if (!ok)
            continue;
        const QString command = QString::fromUtf8(line.mid(first + 1, last - first - 1).trimmed());
        process.name = QFileInfo(command.section(QLatin1Char(' '), 0, 0)).fileName();
        const qint64 used = line.mid(last + 1).trimmed().toLongLong(&ok);
        if (ok)
            process.usedMb = used;
        out.append(process);
    }
    return out;
}

QList<GpuProcess> gpuProcesses() {
    QProcess smi;
    smi.start(QStringLiteral("nvidia-smi"), {QStringLiteral("--query-compute-apps=pid,process_name,used_memory"),
                                             QStringLiteral("--format=csv,noheader,nounits")});
    if (!smi.waitForFinished(5000) || smi.exitStatus() != QProcess::NormalExit || smi.exitCode() != 0) {
        smi.kill();
        smi.waitForFinished(1000);
        return {};
    }
    return parseGpuProcesses(smi.readAllStandardOutput());
}

QString describeGpuProcesses(const QList<GpuProcess> &processes) {
    QStringList named;
    for (const GpuProcess &process : processes)
        named << (process.usedMb ? QStringLiteral("%1 (pid %2, %3 GB)")
                                       .arg(process.name)
                                       .arg(process.pid)
                                       .arg(double(*process.usedMb) / 1024, 0, 'f', 1)
                                 : QStringLiteral("%1 (pid %2)").arg(process.name).arg(process.pid));
    return named.join(QStringLiteral(", "));
}

} // namespace fugue
