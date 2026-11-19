# Spike 02: splitting stable-diffusion.cpp into encode, sample and decode

Issue: #2. Date: 2026-11-18. Verdict: **go.**

## Question

Can text encode, sampling and VAE decode run as separate steps of stable-diffusion.cpp, in
separate processes, with conditioning and latents crossing between them as files? This decides
whether the component-level nodes of M5 ship in v0.1 ([spec 8.10](../spec.md#810-gpu-memory-and-the-helper),
[14.3](../spec.md#143-m5-component-level)).

## Answer

Yes. A 309-line patch against stable-diffusion.cpp, with no change to its sampling or model
code, runs the three stages as three processes. The image is **pixel-identical** to the one a
single process makes from the same prompt and seed, in every case tried. Conditioning and
latents are small plain tensor files. A saved latent also works as the start of a second
sampling pass without ever becoming pixels.

The split costs about 1.5 seconds per image when each stage is its own short-lived process,
which a long-lived helper does not pay, and it lowers peak GPU memory because only one
component is loaded at a time.

## Setup

| | |
| --- | --- |
| stable-diffusion.cpp | commit `532d2c15c430a785a712b2d94281f04e733008a4` (2026-11-15), CUDA build |
| Machine | NVIDIA RTX 3090, 24 GiB, driver 615.78.08, CUDA 13.5, 12 cores |
| Model | Z-Image Turbo: `z_image_turbo-Q8_0.gguf`, text encoder `Qwen3-4B-Instruct-2507-Q8_0.gguf`, VAE `ae.safetensors` (the files Omarchy's `sdcpp` recipe pins) |
| Settings | 1024x1024, 8 steps, guidance 1.0, seed 42, unless stated |
| Patch | [02-sdcpp-split.patch](02-sdcpp-split.patch) |

Build note: on Arch, `nvcc` looks for `g++-15`, which is not installed beside GCC 16. Building
with `NVCC_CCBIN=/usr/bin/clang++` and `-DCMAKE_CUDA_HOST_COMPILER=/usr/bin/clang++` works.

## What the patch does

`generate_image` in `src/pipeline/image.cpp` already runs four steps in order: prepare the
initial latent, encode the prompts, sample, decode. The patch adds hooks at those seams, driven
by environment variables so the stock `sd-cli` can exercise them:

| Hook | Effect |
| --- | --- |
| `FUGUE_SAVE_COND`, `FUGUE_LOAD_COND` | Write the conditioning after the encode step, or read it and skip that step. |
| `FUGUE_SAVE_LATENT`, `FUGUE_LOAD_LATENT` | Write the sampled latent, or read it and skip sampling. |
| `FUGUE_SAVE_INIT_LATENT`, `FUGUE_LOAD_INIT_LATENT` | Write the VAE-encoded start latent of an image-to-image run, or replace it from a file. |
| `FUGUE_STOP_AFTER=cond` or `sample` | Return after that stage. |

Files are in the safetensors layout: a JSON header and raw tensors. Conditioning is the fields
of the library's `SDCondition` (positive, negative and image-guidance sets); a latent is one
float tensor. This is throwaway code that proves the seams. The real helper would expose the
stages as its own operations, not as environment variables.

## Results

Each row is one process, measured from outside: wall time, and peak GPU memory of that process
from `nvidia-smi`.

**Text to image, 1024x1024, guidance 1.0**

| Run | Wall | Peak GPU memory | Output |
| --- | --- | --- | --- |
| One process | 12.9 s | 13.7 GiB | image |
| Encode only | 2.5 s | 4.1 GiB | conditioning, 205 kB |
| Sample only | 10.1 s | 7.1 GiB | latent, 1.05 MB |
| Decode only | 1.8 s | 3.6 GiB | image, **identical pixels** |
| Three processes, total | 14.3 s | 7.1 GiB at most | |

**With a negative prompt, guidance 2.5, 832x1216** (two conditioning sets)

| Run | Wall | Peak GPU memory | Output |
| --- | --- | --- | --- |
| One process | 18.9 s | 13.6 GiB | image |
| Encode only | 2.7 s | 4.2 GiB | conditioning, 338 kB |
| Sample only | 16.3 s | 7.1 GiB | latent, 1.01 MB |
| Decode only | 1.9 s | 3.5 GiB | image, **identical pixels** |

**Reusing conditioning.** The conditioning saved by the seed 42 run, sampled with seed 7 and
decoded, is pixel-identical to a single process run with seed 7. The second image skipped the
text encoder: 11.9 s in two processes against 13.0 s, and no 4 GiB encoder load.

**Chaining a latent.** A second pass (strength 0.5, seed 9) over the first result:

| Start of the second pass | Result |
| --- | --- |
| The PNG, encoded by the VAE in the same process (reference) | image |
| That same encoded latent, saved and loaded from a file | identical pixels to the reference |
| The first sampler's own latent, loaded from its file, never decoded | a correct image; differs from the reference (PSNR 37 dB) |

The last row is the point of latents as a port type: the pixel round trip is lossy, and the
latent path avoids it. It also shows the sampler's output and the VAE encoder's output are the
same kind of value, so one latent type serves both.

**Where the time goes** (from the library's log, single process): text encoder load 1.6 s and
encode under 0.1 s; diffusion model load 2.6 s and 8 steps about 6.6 s; VAE load 0.2 s and decode
0.6 s. Saving or loading a conditioning or latent file took 1 ms or less. Each process
spends about 0.9 s outside its stage (start-up, reading model headers, exit).

The library already loads each component's weights only when its stage first runs. That is why
the encode-only process never touched the 6.1 GiB diffusion model.

## What this means for Fugue

- **M5 is in v0.1**, as the plan said it would be if this passed.
- The helper keeps the model components loaded between jobs, so it pays neither the 1.5 s of
  process starts nor the reloads. Fugue's residency planner then decides what stays loaded.
- `fugue/conditioning` and `fugue/latent` can be the plain tensor files the spec describes
  ([spec 2.6](../spec.md#26-data-values-latents-and-conditioning)). Conditioning is a few hundred
  kilobytes and a 1024x1024 latent is 1 MB, so hashing and caching them is cheap.
- The cache works as designed: changing only the seed or the sampler reuses the conditioning.
- Results are deterministic on one machine and one build. Nothing here says anything about
  matching another engine.

## Risks and what was not tested

- **Upstream churn is the real cost.** The patch touches one function in a file that changes
  often, and the library's public header changed 30 times in ten weeks. The helper must pin a
  commit and move the pin on purpose. Upstream has not yet been asked whether it would take
  stage entry points in its C API; a yes would remove this cost. The question is written and
  will be posted with this result.
- **One model family.** Only Z-Image Turbo was run. Its conditioning is one tensor per prompt.
  Models with CLIP encoders (SD 1.5, SDXL), reference-image editing (FLUX.2 Klein) and video
  fill more fields of `SDCondition`. The patch serializes all of its tensor fields, but two
  fields used only by MiniMax H3 (reference audio and reference blocks) are not covered.
- **Not run:** LoRA applied in the sample stage, ControlNet, inpainting masks, hires fix, batch
  counts above one, the Vulkan backend, AMD and unified-memory machines.
- **Not built:** the persistent helper and its protocol. The spike used one process per stage.
- A component that is skipped still has to be named when the context is created (the text
  encoder path is given to the sample-only process even though it is never loaded). The helper
  should create contexts per component set, or keep one context and rely on lazy loading as
  the spike did.

## Reproducing

1. Check out stable-diffusion.cpp at the commit above and apply the patch.
2. Build with `-DSD_CUDA=ON` (see the build note).
3. Run `sd-cli` three times with the same arguments, setting in turn
   `FUGUE_SAVE_COND=c.safetensors FUGUE_STOP_AFTER=cond`, then
   `FUGUE_LOAD_COND=c.safetensors FUGUE_SAVE_LATENT=l.safetensors FUGUE_STOP_AFTER=sample`, then
   `FUGUE_LOAD_LATENT=l.safetensors`. The runs that stop early exit nonzero because `sd-cli`
   finds no image to save; the files are written.
4. Compare the last image with one from a plain run: decode both to raw RGB and compare bytes.
