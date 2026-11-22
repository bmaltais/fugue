# fugue-helper

stable-diffusion.cpp at a pinned commit with Fugue's stage patch, so encode, sample and decode
run as separate calls ([spec 8.10](../docs/spec.md#810-gpu-memory-and-the-helper)).

| File | What it is |
| --- | --- |
| `CMakeLists.txt` | The pin: stable-diffusion.cpp, ggml and CUTLASS commits with their SHA-256 |
| `sdcpp.patch` | The stage patch: `fugue.*` image and video options, the stage file format (`src/pipeline/fugue_stages.h`) and its reading and writing (`fugue_stage_io.h`), hooks in `generate_image` and `generate_video` (for an inpaint model, `generate_image` also refreshes `c_concat` when a sample reads its conditioning) |
| `src/main.cpp` | `fugue-helper <request.json>` (one operation, then exit) `fugue-helper --serve` (the provider protocol over stdio, for Fugue's `fugue-helper` provider) and `fugue-helper --serve-http` (the same over HTTP, for a paired host); the formats are at its top |
| `smoke-http.sh` | `curl` checks of `--serve-http` that need no model |
| `fugue-helper.service` | A systemd user unit for a paired host |

## Build

```sh
bin/build-helper cuda      # or vulkan, cpu; into build-helper-<backend>/
bin/build-helper cuda -DCMAKE_CUDA_ARCHITECTURES=native -DGGML_NATIVE=ON   # this machine only, faster
```

A cold CUDA build takes about 15 minutes; ccache makes later ones short. The package builds the
Vulkan helper into `fugue` and the CUDA one into `fugue-helper-cuda` (`packaging/PKGBUILD`).

### aarch64 CUDA asset (a DGX Spark)

The Arch package cannot serve a paired host that runs Ubuntu on aarch64 (CUDA 13, sm_121). A release
carries `fugue-helper-cuda-aarch64-linux.tar.zst` for it: the binary (it links the host's CUDA
runtime and cuBLAS dynamically; unsigned) and `fugue-helper.service`, with its line in `SHA256SUMS`.
`package.yml` builds it on an arm runner (`helper-aarch64`) for each `v*` tag, next to the
aarch64 Arch package (`package-aarch64`; it holds the Vulkan helper). By hand, on the host,
from a checkout of the tag, with the same script (portable settings: no `-march=native`; sm_121 code plus PTX):

```sh
git clone --branch vX https://github.com/bmaltais/fugue && cd fugue
bin/package-helper                       # about 40 minutes cold, ccache after; writes dist/
gh release upload vX dist/fugue-helper-cuda-aarch64-linux.tar.zst dist/SHA256SUMS --clobber
```

Fetch SHA256SUMS first (`gh release download vX -p SHA256SUMS -D dist`) so the upload keeps the other
lines. On the host: unpack, put the binary at `~/.local/lib/fugue/fugue-helper-cuda`, and follow
the paired-host steps below.

Editing the patch: the fetched source is patched once, when the build folder is made. Either
start a fresh `build-helper-<backend>/`, or make the same edit in
`build-helper-<backend>/_deps/sdcpp-src` and rebuild.

## Paired host (`--serve-http`)

The same operations over HTTP, run on a host such as the DGX Spark and reached at its tailnet
address (spec 8.8, `http` transport; the host runs it, Fugue does not start it):

```sh
fugue-helper-cuda --serve-http --listen 100.x.y.z --port 8765 --key-file ~/.config/fugue/helper.key \
                  --models ~/models:~/ComfyUI/models
```

`fugue-helper.service` is a systemd user unit for it. The key is the first line of the key file,
trimmed; the helper refuses to start with no key file, an empty one, or one that group or others can
read (mode 0600). Bind to the tailnet address, not `0.0.0.0`. The key travels in clear over `http`: use it only inside a tunnel such as the tailnet. Anyone who has the key can make the host read any file under its `--models` folders by name (a hash check tells whether a file equals a given one) and use its disk and memory for what they send; keep the key as secret as a password. `helper/smoke-http.sh <binary>` checks
auth, `describe` and the error paths with `curl`.

Every request carries `Authorization: Bearer <key>`, else 401. JSON bodies up to 512 MB. Errors are
`{"error": {"code", "message"}}`, HTTP 400 (`input_invalid`, `model_missing`), 401, 404 (no such job
or file) or 500 (`out_of_memory`, `helper_failed`).

| Request | Reply |
| --- | --- |
| `POST /fugue/v1/describe` `{}` | `{"protocol": 1, "engine": "sdcpp", "shares_filesystem": false, "backend", "capabilities", "free_memory_bytes", "host"}`; free memory is `MemAvailable` |
| `POST /fugue/v1/submit` `{"capability", "params", "inputs"}` | `{"job"}`; no `output_dir`, each job has a private work dir |
| `POST /fugue/v1/status`, `/cancel` `{"job"}` | as over stdio |
| `POST /fugue/v1/result` `{"job"}` | `{"outputs": {NAME: {"file": i, "format": "png\|conditioning\|latent", "size"} or a list}, "used"}` |
| `GET /fugue/v1/result/<job>/<i>` | the bytes of output file `i`, `application/octet-stream` |
| `POST /fugue/v1/shutdown` `{}` | `{}`; does nothing |

Jobs run one at a time, in order. A finished job's files are kept for an hour or until 20 newer
jobs have finished.

- **Inputs** are `{"data": base64, "format": "png|conditioning|latent"}` (or a list), written into the
  job's work dir. An `{"image": PATH}` option reads a host file and is refused.
- **Models:** a value in `params.context` whose key ends in `_path`, a `_path` param, or a
  `params.loras[].path` is a string path (used only if inside a `--models` root, else
  `input_invalid`) or a handle `{"name": "flux/x.gguf", "sha256", "size"}`. A handle is looked up as
  `<root>/<name>`, `<root>/<type folder>/<name>` (`vae`, `text_encoders`, `diffusion_models`, ...),
  then any file under the roots (recursive, symlinks followed, dot-names skipped) whose relative
  path ends with `name`, then one whose basename matches. `size`, when given, must match (checked
  first, it is cheap). `sha256` (`sha256:<hex>` or bare hex), when given, makes the content the
  identity (spec 8.9): of the files that match by name, the first whose SHA-256 equals it is taken;
  a file of the same name and another content is never used. A hash is computed once per (path,
  size, mtime) and kept in memory and in `model-hashes.json` under `$XDG_STATE_HOME/fugue/`
  (`~/.local/state/fugue/`), so a restart does not read a 10 GB file again. A handle without
  `sha256` is taken by name as before. The roots are indexed at start and once more on a miss. A
  name with `..`, an absolute one or a malformed `sha256` is `input_invalid`; no file is
  `model_missing`, naming the file and the roots searched, or, when files had the name but not the
  content, listing them ("the sha256 differs"). Hashing can take a minute on first use, so a
  handle with a `sha256` is resolved in the job, not in the submit request: `submit` replies at
  once, the job is `running` with stage `models`, and a missing model ends it in state `error`
  with `model_missing`. Handles without `sha256` are still resolved in the request (HTTP 400).

## Video (Wan)

A context that makes video is run with the same operations and capabilities as an image model
(spec 8.10): `text.encode` writes the conditioning, `latent.sample` the latent (both Wan 2.2 passes
when the context has `high_noise_diffusion_model_path`), `latent.decode` the video. A video's
generation options are the library's (`video_frames`, `fps`, `width`, `height`, `sample.*`). The
decoded video is `video.y4m` (YUV4MPEG2, format `y4m` over HTTP); `ffmpeg -i video.y4m out.mp4` makes
it a clip. A request for a video model, one operation per process:

```sh
fugue-helper-cuda enc.json   # {"op": "encode", "conditioning": "c.bin", "params": {...}, "context": {...}}
fugue-helper-cuda smp.json   # {"op": "sample", "conditioning": "c.bin", "latent": "l.bin", "params": {...}}
fugue-helper-cuda dec.json   # {"op": "decode", "latent": "l.bin", "video": "out.y4m", "params": {...}}
```

with `"context": {"diffusion_model_path": "…LowNoise….gguf", "high_noise_diffusion_model_path": "…HighNoise….gguf",
"t5xxl_path": "umt5_xxl….safetensors", "vae_path": "wan_2.1_vae.safetensors"}` and `"params"`
`{"prompt", "negative_prompt", "width", "height", "video_frames", "fps", "seed", "sample.steps",
"sample.guidance.txt_cfg", "sample.flow_shift"}`, the same in all three. A video has no `vae_encode`
and cannot start from a latent; the helper answers `input_invalid`.

An image-to-video model (Wan 2.2 I2V, TI2V) takes its first frame as `"image": "start.png"` in the
`encode` and the `sample` request (job input `image` through `--serve`; data over HTTP), the same
picture in both, with the clip's `width`, `height` and `video_frames` in `params`: the encode makes the model's extra input
from it. An image model given an `image` and no mask answers `input_invalid` (with a mask, see Masks).

## Masks (inpainting)

`sample` and `generate` of an image model take `"mask": "mask.png"` (job input `mask` through `--serve`,
data `{"data", "format": "png"}` over HTTP; in a `generate` request the picture is `params.init_image`,
`{"image": PATH}`). The mask is read with one channel (`stbi_load(..., 1)`: a color PNG becomes gray)
and must be the request's `width` and `height`; it is checked before any model loads. White is redrawn;
the library rounds it to 0 or 1 and scales it to the latent's size, so a soft mask is hard here.

What the helper does with it depends on the model, by `sd_get_model_version_name`:

- An inpaint model (`SD 1.x Inpaint`, `SD 2.x Inpaint`, `SDXL Inpaint`, `Flux Fill`, `Flex.2`): the
  `sample` needs `"image"` (the picture, as `init_image`) and ignores `init_latent`. The model's
  masked picture is part of the conditioning (`c_concat`), which the `encode` made without a picture
  or mask, so the patch refreshes it from the sample's own latents after it reads the conditioning
  (`generate_image`, after `fugue_load_cond`), as a single run does.
- Any other image model (FLUX.2 Klein, Qwen-Image Edit, FLUX.1 dev, Z-Image, SDXL, SD 1.5): the
  `sample` needs `init_latent` (the one `vae_encode` made); `image` is ignored. The library puts the
  start latent back outside the mask at every step.

`input_invalid`: a mask for a video model, for `encode`, `decode` or `vae_encode`, one of another size
than the request, an unreadable one, an inpaint model without `image`, a mask with neither
`init_latent` nor `init_image`. `smoke-http.sh` checks the size and the unreadable mask without a
model; the others need a loaded one (see the pin-move checklist).

## Upscaling (`image.upscale`)

An image through an upscale model, with the library's own ESRGAN and MoSR upscaler (spec 8.1). No
sd context is loaded. The upscaler stays loaded between jobs, keyed by the model file and the tile
size, and is freed before a diffusion context loads or when memory runs out, like the contexts. The
tiling is the library's (`tile_size`, a quarter of overlap), which reports each tile as progress. A
request, one operation per process:

```sh
fugue-helper-cuda up.json   # {"op": "upscale", "image": "in.png", "output": "out.png",
                            #  "context": {"model_path": "RealESRGAN_x4plus.safetensors"}, "params": {"tile_size": 512}}
```

The reply is `{"outputs": [{"image": "out.png"}], "scale": 4}`. Through `--serve` the capability is
`image.upscale`, the input is `inputs.image` and the model is `params.context.model_path` (a handle
over HTTP, like any `_path`). The input is 8-bit RGB; Fugue keeps an alpha channel itself. The
supported models are ESRGAN RRDBNet (4x) and MoSR, as `.safetensors`, `.pth` or GGUF; a 2x model
(Real-ESRGAN x2plus, whose first convolution takes 12 channels) is refused with `model_unsupported`.

A video comes as a folder of frames, a chunk at a time (Upscale Video with Model, #138): `"frames":
"in/"` and `"frames_output": "out/"` instead of `image` and `output` (through `--serve`,
`inputs.frames` and the job's `frames-out/` folder). Every PNG of the folder goes through in file-name
order, one at a time, into the other folder under the same name (written as `.part`, then renamed);
progress is the frames done; the reply is `{"outputs": [{"frames": "out/"}], "scale": 4, "frames": N}`.
The folder is on the helper's disk, so a paired host (`--serve-http`) does not take it. A cancel is
heard between frames (`cancelled`); inside one image the library has no hook, and Fugue ends the
helper after its grace period. No change to the stage patch: the upscaler API is the library's own.

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
   - IP-Adapter, PhotoMaker or PuLID, only when the move changes their code;
   - Wan (T2V) staged: encode, sample and decode as separate processes, and one `generate`
     run of the same request: the frames must be the same;
   - Wan 2.2 I2V the same way, with `"image"` in the encode and the sample and `params.init_image`
     (`{"image": PATH}`) in the `generate` run.

   - an inpaint model staged equals one `generate` run: `encode`, `vae_encode` (not used by an inpaint
     model, but the stage runs), `sample` with `"mask"`, `"image"` and `"init_latent"`, `decode`,
     against one `generate` with `params.init_image` and `"mask"`; the pixels must be the same (FLUX.1 Fill
     `fluxFillFP8_v10.safetensors`, SDXL Inpaint as `model_path` plus its `diffusion_model_path`).
     Without the `c_concat` hook the FLUX.1 Fill pictures differ: that is the check that the hook is
     still reached. Also staged FLUX.2 Klein 4B with a mask and `init_latent` against `generate`;
   - the version names: `grep -rn 'definition.name' src/model/integrations/` at the new commit, and
     which of them set `traits.is_inpaint` (`sd.cpp`, `flux.cpp` at the pin): `isInpaintVersion` in
     `src/main.cpp` and the list above must be the same set.

   These need a GPU and model files, so they are run by hand, not in CI.
4. Run the stages once through Fugue's provider, as the app does (`--serve`, models kept loaded):
   ```sh
   FUGUE_REAL_HELPER=$PWD/build-helper-cuda/fugue-helper-cuda \
   FUGUE_REAL_HELPER_REQUEST=params.json \
   build/test_helper_provider aRealHelperRunsTheStagesWarm
   ```
   `params.json` holds the generation options with their `context`, for example
   `{"prompt": "…", "width": 1024, "height": 1024, "sample.steps": 8, "sample.guidance.txt_cfg": 1,
   "context": {"diffusion_model_path": "…", "llm_path": "…", "vae_path": "…"}}`. Set
   `FUGUE_REAL_HELPER_KEEP=out.png` to keep the image: it is the same as the one-shot staged run's.
