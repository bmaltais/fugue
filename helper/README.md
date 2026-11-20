# fugue-helper

stable-diffusion.cpp at a pinned commit with Fugue's stage patch, so encode, sample and decode
run as separate calls ([spec 8.10](../docs/spec.md#810-gpu-memory-and-the-helper)).

| File | What it is |
| --- | --- |
| `CMakeLists.txt` | The pin: stable-diffusion.cpp, ggml and CUTLASS commits with their SHA-256 |
| `sdcpp.patch` | The stage patch: `fugue.*` image options, the stage file format (`src/pipeline/fugue_stages.h`), hooks in `generate_image` |
| `src/main.cpp` | `fugue-helper <request.json>`: one operation per run; the request format is at its top |

## Build

```sh
bin/build-helper cuda      # or vulkan, cpu; into build-helper-<backend>/
bin/build-helper cuda -DCMAKE_CUDA_ARCHITECTURES=native -DGGML_NATIVE=ON   # this machine only, faster
```

A cold CUDA build takes about 15 minutes; ccache makes later ones short. The package builds the
Vulkan helper into `fugue` and the CUDA one into `fugue-helper-cuda` (`packaging/PKGBUILD`).

Editing the patch: the fetched source is patched once, when the build folder is made. Either
start a fresh `build-helper-<backend>/`, or make the same edit in
`build-helper-<backend>/_deps/sdcpp-src` and rebuild.

## Moving the pin

The pin moves only on purpose (spec 8.10).

1. Carry the patch to the new commit with git's three-way merge:
   ```sh
   git clone https://github.com/leejet/stable-diffusion.cpp sdcpp && cd sdcpp
   git checkout <old commit> && patch -p1 < ../fugue/helper/sdcpp.patch
   git add -A && git commit -m "Fugue stage patch"
   git rebase --onto <new commit> <old commit>   # fix conflicts as in any rebase
   git diff <new commit> > ../fugue/helper/sdcpp.patch
   ```
   Keep the patch to hooks at the stage seams: no change to sampling or model code.
2. Update the commits and hashes in `CMakeLists.txt` and `packaging/PKGBUILD` together.
   `git ls-tree <new commit> ggml thirdparty/cutlass` gives the ggml and CUTLASS commits;
   `curl -sL <tarball URL> | sha256sum` gives each hash.
3. Build, then check where the patch can break. The model families are stable-diffusion.cpp's
   to test. Run each stage as its own process:
   - Z-Image Turbo, plus one `generate` run with the same request: the pixels must be the same;
   - SDXL with a negative prompt (CLIP conditioning, two sets);
   - image-to-image from a latent (`vae_encode`, then `sample` with `init_latent`);
   - IP-Adapter, PhotoMaker or PuLID, only when the move changes their code.

   These need a GPU and model files, so they are run by hand, not in CI.
