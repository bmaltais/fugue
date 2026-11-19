# Spike 03: libvips speed on 1 to 4 megapixel images

Issue: #3. Date: 2026-11-18. Verdict: **keep libvips (C++ API, dynamic, CPU).**

## Question

Is libvips fast enough for the pixel nodes ([spec 14.1](../spec.md#141-m1-the-first-twelve)),
does it give the same pixels as ComfyUI's formulas, and do its thread pool and cache fight the
executor ([spec 9.2](../spec.md#92-scheduling))?

## Answer

- **Speed: yes.** Every M1 pixel node finishes a 4 MP image in under 100 ms (blur radius 31 in
  177 ms), and most in under 50 ms. These are well below the cost of one generation step, so a
  pixel chain never gates a run.
- **Parity: yes, to float rounding** (3e-8 to 9e-8) for resize-free nodes, when blur and sharpen
  use ComfyUI's own kernel (below). Resize methods are not bit-identical to torch; see Resize.
- **Threads: libvips concurrency must be 1 per CPU node** while the executor runs CPU nodes in
  parallel. With eleven workers, concurrency 1 was fastest (9.2 jobs/s) and the default 12 was
  the slowest (5.6 jobs/s, 40% less). The cache is irrelevant here: set it to 0.
- **Evaluation: materialise every node's result.** One lazy pipeline over five nodes was slower
  (288 ms) than materialising each node (207 ms), and the engine's result cache needs the
  materialised image anyway.

## Setup

| | |
| --- | --- |
| libvips | 8.18.7 (Arch), C++ API (`vips-cpp`), built `-O2` with GCC |
| Machine | 12 logical cores (the same box as spike 02); no GPU used |
| Images | float32 RGB in 0..1 (as ComfyUI holds them), synthetic, held in memory, 1024x1024, 1448x1448, 2048x2048 |
| Method | median of 15 runs after one warm-up; each run forces the result into memory; file decode and encode are not measured |
| Code | [spikes/libvips/bench.cpp](../../spikes/libvips/bench.cpp) |

ComfyUI itself was not run (no Python torch on this machine). Its formulas were read from
`comfy_extras/nodes_post_processing.py` and `comfy/utils.py` on master, 2026-11-18, and
re-implemented as scalar C++ for the parity check. Timing against torch is therefore not
measured; the bar used is "far below one generation step", not "faster than torch".

## Speed (ms, vips concurrency 12, cache off)

| Operation | 1 MP | 2 MP | 4 MP |
| --- | ---: | ---: | ---: |
| resize to 50% lanczos3 | 7.9 | 12.6 | 31.0 |
| resize to 50% bicubic | 5.6 | 11.1 | 23.3 |
| resize to 50% bilinear | 4.8 | 8.9 | 18.6 |
| resize to 50% nearest | 2.9 | 6.6 | 16.6 |
| resize to 50% area (`reduce`) | 7.5 | 11.2 | 30.1 |
| resize to 200% lanczos3 | 28.2 | 53.0 | 105.0 |
| crop to 50% | 0.4 | 0.6 | 1.0 |
| brightness/contrast (with clamp) | 10.7 | 18.8 | 37.3 |
| blur r=8 sigma=1, exact kernel, two 1D passes | 20.5 | 31.8 | 49.3 |
| blur r=8 sigma=1, exact kernel, one 2D convolution | 85.4 | 151.2 | 314.1 |
| blur r=8 sigma=1, native `gaussblur` | 41.8 | 78.9 | 147.3 |
| blur r=31 sigma=1, exact kernel, two 1D passes | 51.7 | 93.1 | 177.3 |
| blur r=31 sigma=1, native `gaussblur` | 269.0 | 519.7 | 1021.1 |
| sharpen r=3, exact kernel, one 2D convolution | 29.9 | 45.6 | 71.7 |
| sharpen r=3, native blur + subtract | 29.1 | 43.8 | 75.4 |
| blend normal / difference | 12.8 | 21.5 | 40.4 |
| blend multiply | 15.2 | 25.9 | 45.1 |
| blend screen | 26.8 | 44.4 | 73.7 |
| blend overlay | 32.7 | 54.5 | 86.4 |
| blend soft light | 34.1 | 51.2 | 78.0 |

## Parity with ComfyUI

Max absolute difference from a scalar C++ reference of ComfyUI's formulas, floats in 0..1:

- **Blend**: all six modes (normal, multiply, screen, overlay, soft light, difference), with
  `blend_factor` 0.7 and clamp: 3.6e-8 to 9.1e-8.
- **Blur**: ComfyUI's kernel is not a pixel-sigma Gaussian. It is `exp(-d^2 / 2 sigma^2)` with `d`
  on `linspace(-1, 1)` over `(2r+1)` taps, so the effective pixel sigma is `sigma * r`, truncated
  at the radius, with `reflect` padding (the edge pixel is not repeated). It is separable, so two
  1D passes give the same result as the 2D kernel: 3e-8 for r 3 to 31 and sigma 1 to 3.
  libvips `gaussblur` with sigma `sigma * r` is close (1e-5 to 2.4e-4) but not equal, because it
  does not truncate at `r`, and is slower. **Use the exact two-pass kernel.**
- **Sharpen**: ComfyUI's kernel reduces to `image + 10 * alpha * (image - blur)`, clamped. Exact
  kernel: 2.9e-8. Native blur and subtract: 1.3e-4. Both take about 30 to 75 ms, so use the exact
  one.
- **Resize**: not measured against torch. ComfyUI's `common_upscale` calls
  `torch.nn.functional.interpolate` (nearest-exact, bilinear, bicubic, area) with PIL for
  lanczos. libvips kernels differ in edge handling and, when shrinking, in whether they
  pre-filter (libvips does, `interpolate` bilinear and bicubic do not). Expect small, visible-only-in-diffs
  differences; the node's parity test should set a tolerance per method and record it. This is
  left to the node ticket.
- Brightness/contrast has no ComfyUI core node, so the formula is Fugue's own: `out = clamp(in * contrast + brightness)`.

## Threads and cache

Eleven worker threads (cores minus one), each running four jobs of blur, soft-light blend and
resize on a 2 MP image:

| libvips concurrency | wall time (s) | jobs/s |
| ---: | ---: | ---: |
| 1 | 4.78 | 9.2 |
| 2 | 5.03 | 8.7 |
| 4 | 6.04 | 7.3 |
| 12 (default) | 7.88 | 5.6 |

The cache setting (0 or 100 operations) changed nothing within noise (4.78 vs 4.77 s at
concurrency 1). Nodes never repeat the same operation on the same live image, and the engine has
its own result cache.

## Decision

1. **Keep libvips**, C++ API, dynamically linked, CPU. No node in M1 or M4 needs more.
2. **At startup**: `vips_concurrency_set(1)` for nodes run by the parallel CPU executor
   and `vips_cache_set_max(0)`. A lone CPU node (nothing else ready) may raise its concurrency to
   the number of free cores: at 4 MP the first table (concurrency 12) vs the contention run suggests
   that is a 3x to 10x gain for one node, but measure it with the first real node before wiring it.
3. **Materialise each node's output** into a memory image before it enters the result cache; do
   not chain lazy images between nodes.
4. **Blur and sharpen use ComfyUI's exact kernel** (two 1D passes for blur) so saved workflows
   keep their meaning. Do not use `gaussblur`.
5. **Float32 working format** matches ComfyUI; a smaller 8-bit working format is not part of v0.1.
6. Resize parity tolerances are set in the Resize node ticket.

## Limits

- Synthetic images, not photographs; the operations here are data-independent, so timings should
  hold.
- One machine, one libvips build (8.18.7). A different build (for instance without SIMD kernels) could be slower.
- Single-node timings used the default concurrency 12 and are best-case for a lone node.
- ComfyUI/torch was not timed.
