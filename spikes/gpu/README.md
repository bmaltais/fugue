# Spike 04 probes (GPU)

Results and the decision: [docs/spikes/04-gpu.md](../../docs/spikes/04-gpu.md).

- `vkprobe`: raw Vulkan, no Qt. Lists devices, heaps and `VK_EXT_memory_budget`, runs one compute
  dispatch per device. `vkprobe --hold <MiB>` holds that much device memory for 20 s (touching the
  pages when it can), to check that a second run sees another process's use.
- `rhiprobe vulkan|opengl|null [shader.qsb]`: Qt RHI compute with no window. Pick the platform
  with `QT_QPA_PLATFORM`.

Build. The generated shader files are checked in; regenerate them after editing a `.comp` file:

```
glslc -mfmt=c square.comp -o square.spv.inc                    # for vkprobe
/usr/lib/qt6/bin/qsb --glsl 430,310es -c double.comp -o double.comp.qsb   # for rhiprobe
```

Vulkan headers are not installed by default on Arch (`vulkan-headers` package). Either install
it, or point at a checkout of KhronosGroup/Vulkan-Headers (tag v1.4.363 was used):

```
cmake -S . -B build -G Ninja -DVULKAN_HEADERS=<dir>/include -DVulkan_INCLUDE_DIR=<dir>/include
cmake --build build
```
