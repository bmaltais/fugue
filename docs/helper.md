# Portable helper tarballs

The generation helper (`fugue-helper`, stable-diffusion.cpp with Fugue's changes, [helper/README.md](../helper/README.md))
reaches a machine in three ways: the Arch packages (`fugue` holds the Vulkan helper, `fugue-helper-cuda` the CUDA one),
the aarch64 CUDA tarball for a DGX Spark ([helper/README.md](../helper/README.md#aarch64-cuda-asset-a-dgx-spark)), and, for the
AppImage and every other x86_64 distribution, these tarballs (issue #334):

| Release asset | For | Size (compressed) |
|---|---|---|
| `fugue-helper-cuda-x86_64-linux.tar.zst` | an NVIDIA GPU from the RTX 20 series on, with the host's driver | 586 MiB |
| `fugue-helper-vulkan-x86_64-linux.tar.zst` | any GPU with a Vulkan driver (AMD, Intel, NVIDIA) | 22 MiB |
| `fugue-helper-cpu-x86_64-linux.tar.zst` | no usable GPU (slow); `install.sh --helper cpu` | 16 MiB |

Each has its line in the release's `SHA256SUMS`.

## Getting one

`install.sh` does it on a host without pacman (it picks CUDA when an NVIDIA driver is present, else Vulkan when a Vulkan loader
is) and the in-app notice copies `install.sh --helper-only` for a host that has the AppImage already:

```sh
curl -fsSL https://raw.githubusercontent.com/bmaltais/fugue/main/install.sh | bash -s -- --helper-only
```

`--no-helper` skips it, `--helper cuda|vulkan|cpu` chooses, `--dry-run` shows the plan. By hand, from the release page:

```sh
sha256sum -c --ignore-missing SHA256SUMS   # in the folder with the download
mkdir -p ~/.local/share/fugue/helper
tar --zstd --strip-components=1 -xf fugue-helper-cuda-x86_64-linux.tar.zst -C ~/.local/share/fugue/helper
```

The AppImage's `AppRun` looks for `fugue-helper-cuda` (on an NVIDIA machine), then `-vulkan`, then `-cpu` in that folder
(`~/.local/lib/fugue`, `/usr/local/lib/fugue` and `/usr/lib/fugue` after it; [appimage.md](appimage.md)). Restart Fugue.

## What each holds

One folder, `fugue-helper-<backend>-x86_64-linux/`:

- `fugue-helper-<backend>`: the binary, stripped, built with `-DGGML_NATIVE=OFF` (no `-march=native`; ggml's AVX2, FMA and F16C
  CPU code, so a CPU from 2013 on). Its RUNPATH is `$ORIGIN/lib`.
- `fugue-helper.service`: the systemd user unit for a paired host.
- `lib/libgomp.so.1` (GCC's OpenMP runtime, which a minimal host lacks; GPLv3 with the GCC Runtime Library Exception) and
  `licenses/libgomp-copyright.txt`.
- CUDA only: `lib/libcudart.so.12`, `lib/libcublas.so.12`, `lib/libcublasLt.so.12` (CUDA 12.9.1, unmodified), and the NVIDIA licence
  with a note in `licenses/`. NCCL is not used (`-DGGML_CUDA_NCCL=OFF`: multi-GPU, and a library hosts lack).

Not inside, on purpose: `libcuda.so.1` and every other NVIDIA driver library (the host's driver provides them; the driver must be
525 or newer for CUDA 12 and 570 or newer for Blackwell), the Vulkan loader (`libvulkan.so.1`) and the GPU's Vulkan driver.
`bin/package-helper` fails the build when `ldd` finds any other library missing, or resolves a driver library on the build machine.

## How they are built

`bin/package-helper [--container] cuda|vulkan|cpu [outdir]` is the one script, for CI and for a local run (`--container` starts
podman, else docker; `FUGUE_JOBS` caps the compiles; the output goes to `~/.cache/fugue-package-helper/dist` (or the `outdir` argument) with its `SHA256SUMS` line).
The compiler cache and the downloaded tools live in `~/.cache/fugue-package-helper/ccache` for every build, native aarch64 included (`CCACHE_DIR` overrides; `XDG_CACHE_HOME` moves
both), outside the checkout, so removing a worktree keeps a 50-minute build's cache and tarball. Worktrees share the output folder, its `SHA256SUMS` and the downloads, so each is changed under a `flock` on a `.lock` file in it (tested by `tests/package_helper_lock.sh`): two runs at once wait for each other, neither loses a line. The build folder is
`build-package-<backend>/` in the checkout (`FUGUE_HELPER_BUILD_DIR`, read by `bin/build-helper`), so it never overwrites the native
`build-helper-<backend>/` that local GPU runs use. It runs `bin/build-helper` with the portable settings inside a container pinned in the script, then bundles, strips, checks
and packs:

| Backend | Image | Build tools |
|---|---|---|
| CUDA | `nvidia/cuda:12.9.1-devel-ubuntu22.04`, by digest | the image's `nvcc` 12.9 and GCC 11; CMake 3.31.12 (Kitware's tarball, sha256 pinned; Ubuntu 22.04 has 3.22 and the helper needs 3.25) |
| Vulkan | `ubuntu:22.04`, by digest | the LunarG Vulkan SDK 1.4.363.1 (sha256 pinned) for `glslc` and the headers (Ubuntu 22.04's are from 2022, older than ggml-vulkan wants), Ubuntu's `libvulkan-dev` to link the loader |
| CPU | `ubuntu:22.04`, by digest | CMake as above |

- **glibc floor 2.35.** The images are Ubuntu 22.04, the AppImage's own base ([appimage.md](appimage.md)), and the script fails if the
  binary needs a newer glibc. A host with an older one (Ubuntu 20.04, Debian 11, RHEL 8) cannot run them, nor the AppImage.
- **GPU architectures (CUDA).** `75-real;86-real;89-real;120-real;75-virtual`: machine code for Turing (RTX 20, GTX 16), Ampere
  (RTX 30), Ada (RTX 40) and Blackwell (RTX 50, which is why CUDA 12.8 or newer is needed), and PTX of 75 that the driver compiles
  for any other card on first use (an A100, an H100: it works, with a slow first start). Pascal and older are not supported
  (CUDA 12.9 can build for them, but each architecture adds compile time and the file grows). Adding 80 and 90 for the data-centre
  cards is a one-line change to `cuda_archs`.
- **CI.** `.github/workflows/helper-tarballs.yml` runs the script on a hosted runner for each of the three backends. A `v*` tag
  adds the files and their `.sha256` files to the release `package.yml` makes (`release-sums.yml` writes `SHA256SUMS`, [release.md](release.md)); a pull request that changes the script or the
  workflow builds them without uploading; by hand (Run workflow) keeps them as artifacts. The CPU and Vulkan tarballs are also
  unpacked in a clean `ubuntu:22.04` container and checked with `helper/smoke-http.sh`. The compiler cache (`.ccache`, with the
  downloaded tools; the workflow sets `CCACHE_DIR` and the output folder `dist` itself) is cached per backend.
- **Locally**: `bin/package-helper --container cuda` needs about 16 GB of disk for the image and a cold build takes the time under
  Sizes; later builds hit ccache. Nothing needs a GPU to compile.

## The CUDA libraries and their licence

The CUDA tarball redistributes NVIDIA's runtime and cuBLAS under the CUDA Toolkit EULA (`licenses/NVIDIA-CUDA-EULA.txt` is
the `copyright` file of the toolkit's packages, the same text). Section 2.2 of it says:

> The portions of the SDK that are distributable under the Agreement are listed in Attachment A.

Section 2.3:

> Those portions of the SDK designed exclusively for use on the Linux or FreeBSD operating systems [...] may be copied and
> redistributed for use in accordance with this Agreement, provided that the object code files are not modified in any way
> (except for unzipping of compressed files).

and Attachment A (2.6) lists, for Linux, "CUDA Runtime: libcudart.so, libcudart_static.a, libcudadevrt.a" and
"CUDA BLAS Library: libcublas.so, libcublasLt.so, libcublas_static.a, libcublasLt_static.a", "including certain variations of
these files that have version number or architecture specific information embedded in the file name" (`libcudart.so.12`).
So the three files are copied byte for byte (`cp -L`, no `patchelf`, no strip) and the helper binary is the application
that carries them; the tarball is not the toolkit as a stand-alone product (1.2, limitation 2). `libcuda.so` is not on the list
and is never shipped. The CUDA image's own licence (NGC Deep Learning Container License) covers the image, not the files taken
from it.

## Sizes and times

First local build, 2026-11-26 (12 cores shared with other jobs, `-j8` under `nice`, cold ccache, rootless podman):

| Tarball | Compressed | Unpacked | Cold build |
|---|---|---|---|
| CUDA | 614 MB (586 MiB) | about 1.05 GB (`libcublasLt` alone is 750 MB, `fugue-helper-cuda` 200 MB) | about 50 minutes (209 CUDA files, 4 architectures); the same on a 4-core hosted runner is untimed and expected near 2 hours, inside the 6 hour limit |
| Vulkan | 23 MB | about 89 MB | about 10 minutes |
| CPU | 16 MB | about 46 MB | under 1 minute after the Vulkan build (the engine's objects hit ccache) |

A rebuild with a warm ccache takes seconds. Changing a compile flag (an architecture, `GGML_CUDA_NCCL`) misses every CUDA object.
Adding 80 and 90 to `cuda_archs` adds about a third to the CUDA build and about as much to the download, for data-centre cards that
the PTX already runs; they were left out.

## Checked

CUDA tarball unpacked into a clean `ubuntu:22.04` container's `~/.local/share/fugue/helper` with the AppImage's host libraries
installed (the list in [appimage.md](appimage.md)) and the GPU passed through (`podman run --device nvidia.com/gpu=all`): the
v0.1.0-test5 AppImage found `fugue-helper-cuda` through `AppRun` and ran the FLUX.2 Klein 4B template to a 1024 x 1024 picture in
about 50 s (models loaded from the folder, RTX 3090). The Vulkan and CPU helpers start in a clean `ubuntu:22.04` and pass
`helper/smoke-http.sh`.
