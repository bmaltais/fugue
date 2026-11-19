# Spike 04: what a Fugue node may use on the GPU

Issue: #4. Date: 2026-11-18. Verdict: **raw Vulkan compute works with no window on every machine
tried; Qt RHI does not, without a display-owning platform plugin. Free memory needs a different
source on unified-memory machines.**

## Question

1. Does Qt RHI compute run in a CLI process with no window?
2. Can Fugue list Vulkan devices and measure free GPU memory on NVIDIA, AMD and unified-memory
   machines ([spec 8.10](../spec.md#810-gpu-memory-and-the-helper))?
3. What do the CUDA redistribution terms allow?

## Machines

| Machine | GPU | Memory | OS, driver |
| --- | --- | --- | --- |
| desktop | NVIDIA RTX 3090, discrete | 24 GiB VRAM | Arch, NVIDIA 615.78.08, Vulkan 1.4.351 |
| gx10 | NVIDIA GB10 (DGX Spark class), unified | 124.5 GiB shared | Ubuntu 24.04 aarch64, Vulkan 1.4.312 |
| surface | Intel HD Graphics 620, integrated | 3.8 GiB shared | Arch, Mesa 26.2.5 |

No AMD machine was available.

Code: [spikes/gpu](../../spikes/gpu/) (`vkprobe`: raw Vulkan, no Qt; `rhiprobe`: Qt RHI). Each
dispatches one compute shader over 1M integers, reads the buffer back and checks every value.

## 1. Qt RHI compute without a window

Qt 6.12.0. "ok" means the compute result was read back and correct.

| Platform plugin | Vulkan backend | OpenGL backend |
| --- | --- | --- |
| `offscreen` | fails (desktop, surface) | fails (desktop, surface) |
| `minimal` | fails | fails |
| `wayland` (inside the session) | ok | ok |
| `eglfs` | fails | ok (desktop; surface over SSH) |
| `vkkhrdisplay` | ok (desktop; surface over SSH) | fails |

`offscreen` and `minimal` give Qt neither a Vulkan instance nor a GL context: `QVulkanInstance::create`
fails before any Vulkan call, so this is the plugin, not the driver. `eglfs` and `vkkhrdisplay`
worked, including over SSH with no session, but they are plugins built to own a display; what
they do beside a running compositor or on another seat was not checked, so they are not a safe
default for a background engine.

**Raw Vulkan compute (no Qt, no window, no platform plugin) worked on all three machines**,
including the GB10 over SSH.

Pitfall found on the way: the `QRhi` must be destroyed before the `QVulkanInstance` and
`QOffscreenSurface` it was created with. The reverse order crashes at exit ("pure virtual method
called" or a segfault) and, with buffered stdout, hides everything the program printed.

## 2. Devices and free memory

Each machine was measured idle, then again while a second process held memory (`vkprobe --hold`:
6 GiB on the 3090, 8 GiB on the GB10, 512 MiB on the Intel iGPU).

| Machine | Vulkan heap (device-local) | `VK_EXT_memory_budget` | Follows another process? | nvidia-smi free | `MemAvailable` |
| --- | --- | --- | --- | --- | --- |
| RTX 3090 | 24576 MiB | 19591 → 13429 MiB | **yes** | 19591 → 13430 MiB, matches within 1 MiB | not relevant |
| GB10 | 93408 MiB (75% of RAM) | 93408 → 93408 MiB, equal to the heap size | **no** | `[N/A]` | 115901 → 107504 MiB, **follows** |
| Intel HD 620 | 1916 MiB (half of RAM) | 1209 → 769 MiB | **yes** | n/a | 1364 → 860 MiB, follows |

- On the **GB10**, nvidia-smi gives `[N/A]` for total, used and free memory, and the Vulkan
  budget is static: it counts only the calling process's own use. Neither sees other processes.
  `/proc/meminfo` `MemAvailable` does.
- `nvidia-smi --query-compute-apps=pid,process_name,used_memory` **does** list each process and
  its memory on the GB10 as on the 3090, so Fugue can still name who holds memory. It lists only
  compute (C, C+G) processes, not graphics-only ones (quickshell's 509 MiB on the desktop was
  missing from it).
- On the Intel iGPU an allocation counts only once its pages are touched: an untouched 512 MiB
  allocation moved neither figure. Both figures count committed memory, not reservations.
- On the GB10 Vulkan also lists llvmpipe, a CPU device, with its own heap; it must be skipped.
- **AMD: not measured.** The kernel documents `mem_info_vram_total`, `mem_info_vram_used`,
  `mem_info_vis_vram_*` and `mem_info_gtt_total`/`_used` under `/sys/class/drm/cardN/device/`
  (amdgpu, docs.kernel.org/gpu/amdgpu/driver-misc.html). RADV implements `VK_EXT_memory_budget`,
  but whether its budget follows other processes is unverified.

## 3. CUDA terms

From the CUDA Toolkit EULA (v13.5, last updated 2026-01-26, docs.nvidia.com/cuda/eula):

- Only the files in **Attachment A** may be redistributed, "incorporated in object code format
  into a software application" that has "material additional functionality" (1.1.1, 1.1.2). On
  Linux these include `libcudart.so`, `libcublas.so`, `libcublasLt.so`, `libcufft.so`,
  `libnvrtc.so`, and also `libcuda.so` and `libnvidia-ptxjitcompiler.so` under "NVIDIA CUDA
  Driver Libraries". The SDK may not be distributed as a stand-alone product.
- 1.2: the SDK may not be used "in any manner that would cause it to become subject to an open
  source software license" (disclosed as source, licensed for derivative works, redistributable
  at no charge). An MIT app may ship the binaries, but those files stay under NVIDIA's terms, not
  MIT, and the package must say so.
- `libcuda.so` has to match the installed kernel driver, so shipping it is useless even though it
  is allowed.

This is a reading of the licence, not legal advice.

## Decision

1. **GPU nodes run outside the Qt GUI stack.** A native GPU node in the engine, and the CLI, use
   Vulkan compute directly (instance, device, compute queue; no surface, no platform plugin), or
   run in the helper. Qt RHI stays in the canvas only. `gpu.api` values in [6.3](../spec.md#63-the-needs-block) stay as they are.
2. **Free GPU memory, by Vulkan device type** (skip `cpu` devices):
   - discrete: the device-local heap's `heapBudget - heapUsage` from `VK_EXT_memory_budget`
     (matches nvidia-smi on NVIDIA, no subprocess; the likely AMD path, unverified);
   - integrated (unified memory): `/proc/meminfo` `MemAvailable`, capped at the device-local heap
     size. Never the Vulkan budget alone, which is static on the GB10.
3. **Naming the holders:** `nvidia-smi --query-compute-apps` on NVIDIA (works on the GB10);
   `/proc/<pid>/fdinfo` DRM entries on other drivers (not tried here).
4. **CUDA:** Fugue links no CUDA. The helper is built against the system `cuda` package and the
   Fugue package depends on it; nothing NVIDIA is bundled. If a self-contained bundle is ever
   made, it ships only Attachment A files, never `libcuda.so`, with a third-party notice saying
   they are under NVIDIA's licence.

## Limits

- No AMD hardware. The AMD rows are from the documentation only.
- Measurements were taken with real other processes running (Superwhisper, browsers, Omarchy's
  `tensorfold-native` on the GB10), so the absolute free figures vary between runs; only the
  changes are compared.
- Qt RHI on the GB10 was not tried (it runs Ubuntu's older Qt).
