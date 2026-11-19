#pragma once

// Free memory for GPU work, measured by Fugue itself (spec 8.10, spikes/04).
//
// Which figure applies depends on the Vulkan device type: on an integrated or
// unified-memory device it is MemAvailable from /proc/meminfo, capped at the heap
// size; on a discrete card it is the device-local heap's budget minus usage
// (VK_EXT_memory_budget). The engine links no Vulkan yet, so only the first figure
// is read here, uncapped. Telling the device type, the cap and the discrete figure
// come with the GPU helper (#32), which is also what first sets
// RunEnvironment::freeGpuMemoryMb: until then nothing is assumed about the device.

#include <QString>

#include <optional>

namespace fugue {

// MemAvailable in MB, as the kernel estimates what a new workload can take without
// swapping. Nothing when the file cannot be read or has no such line.
std::optional<qint64> memAvailableMb(const QString &meminfoPath = QStringLiteral("/proc/meminfo"));

} // namespace fugue
