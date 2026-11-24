# fugue-helper

stable-diffusion.cpp at a pinned commit with Fugue's stage patch, so encode, sample and decode
run as separate calls ([spec 8.10](../docs/spec.md#810-gpu-memory-and-the-helper)).

| File | What it is |
| --- | --- |
| `CMakeLists.txt` | The pin: stable-diffusion.cpp, ggml and CUTLASS commits with their SHA-256 |
| `sdcpp.patch` | The stage patch: `fugue.*` image and video options, the stage file format (`src/api/fugue_stages.h`), the stage runs (`src/api/fugue_stage.cpp`: the cut of a rendered program, the `fugue::` built-ins, the reader of a clip's YUV4MPEG2), one hook each in `generate_images` and `generate_video`, and the opt-in soft-mask blend (see The seam), and `ref_image_args` (`resize_before_vae=false` keeps each edit reference at its own size; `generation_image.cpp`) |
| `src/main.cpp` | `fugue-helper <request.json>` (one operation, then exit) `fugue-helper --serve` (the provider protocol over stdio, for Fugue's `fugue-helper` provider) and `fugue-helper --serve-http` (the same over HTTP, for a paired host); the formats are at its top |
| `smoke-http.sh` | `curl` checks of `--serve-http` that need no model |
| `fugue-helper.service` | A systemd user unit for a paired host |

## Restore (`image.restore`, SeedVR2)

The pinned stable-diffusion.cpp has no `vid_upscale` call: SeedVR2 runs through DGL (`docs/models/seedvr2.md`
there). `fugue-helper` builds a DGL program for the `restore` op (`restoreScript` in `main.cpp`) from the request's
two model paths, `tile_size`, `overlap` and `seed`, and runs it with `sd_dgl_run` on a context made for the job
(`sd_dgl_ctx_create_once`), the picture as the one tensor input. No patch to the engine is needed. It is the
upstream script for one frame with the two additions the ComfyUI workflow has: the VAE encodes and decodes in tiles
(`tiling(tile_size_w, tile_size_h, target_overlap)`: pixels, and the overlap as a share of the tile, at most 0.5) and the
size has no scaling step (the caller resizes first). When the pin moves, re-run a restore and re-read that script and
`seedvr2::positive_embedding`: a checkpoint without `positive_conditioning` (the GGUF and numz fp8 files) is not
recognised by the engine.

`helper/src/restore.h` holds what needs no engine (the program, the size and tiling limits, the tensor-name checks, the
progress bar); `tests/engine/test_helper_restore.cpp` tests it in the app's suite, and `helper/test/restore_test.cpp` feeds the
program to `sd_dgl_check` (`cmake --build build-helper-<backend> --target fugue-helper-restore-test && ctest --test-dir build-helper-<backend>`;
CI runs it). A cancel reaches a running restore through `sd_dgl_ctx_cancel`.

## The seam

Since upstream's Diffusion Graph Language (6cd0ef1) a generation is a DGL program: C++ renders one from
`src/api/templates/*.dgl` for the model family and runs its statements in source order. The patch leaves
the rendering alone and cuts the rendered program (`src/api/fugue_stage.cpp`, reached only by a request
with a `fugue.*` option or the noise-level reading of strength):

1. A value the stage reads from a file has its statement replaced by a `fugue::` built-in.
2. The program returns what the stage writes.
3. Only the statements that result needs are kept (a backward slice over DGL's own parser), so an encode
   loads no diffusion model and a decode no text encoder.

| Stage | Replaced | Returned |
| --- | --- | --- |
| `vae_encode` | nothing (a clip: `base`) | `image_base`, or `base` for a pixel-space model; a clip's latent |
| `encode` | nothing | `positive`, `negative`, `unconditional_text` when the program has it |
| `sample` | `positive`, `negative`, `unconditional_text`; `image_base` or `base` from `init_latent` | the program's result latent |
| `decode` | the result latent | the program's own return |

These names, with `sigmas`, `full_sigmas`, `pos`, `mask_pixels`, `split` and the `latent` field of the
`return`, are the contract with the templates. A name that is gone is an error naming it, not a wrong
picture. A start latent with no picture binds a blank `init_image`, so the template takes its image-to-image
flow, and the cut drops the blank with the encode it would go through.

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

A failed device allocation inside stable-diffusion.cpp (`cudaMalloc failed: out of memory`) is not returned as an
error: the library asserts and the helper aborts (seen with two helpers on one 24 GB card, #130). Fugue types a helper
that died with such a line last in its log as `out_of_memory` (`HelperProvider::finished`); the helper itself cannot.

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
"sample.guidance.txt_cfg", "sample.flow_shift"}`, the same in all three. Starting from a clip is
video to video (below); a video model cannot start from an image latent.

An image-to-video model (Wan 2.2 I2V, TI2V) takes its first frame as `"image": "start.png"` in the
`encode` and the `sample` request (job input `image` through `--serve`; data over HTTP), the same
picture in both, with the clip's `width`, `height` and `video_frames` in `params`: the encode makes the model's extra input
from it. An image model given an `image` and no mask answers `input_invalid` (with a mask, see Masks).

### Video to video

`vae_encode` of a video model takes `"video": "in.y4m"` (job input `video` through `--serve`; data with format `y4m`
over HTTP) instead of an `image`, and writes the clip's latent: a YUV4MPEG2 file in the layout `decode`
writes, 4:2:0, full range BT.601, a size that is a multiple of 16 and 4 n + 1 frames. The size, rate and
length are the file's and replace `width`, `height`, `fps` and `video_frames` of the request (the other stages of
the generation must be given the same values; Fugue's latent descriptor carries them). A `sample` takes that
latent as `"init_latent"` with `params.strength` (0 to 1: the part of the sampling schedule that is run, as for
an image; Wan 2.2's high-noise pass is skipped when `strength` leaves only the low-noise steps), and a
`generate` takes the clip as `"init_video"`; the staged run and the `generate` run give the same pixels. A
latent or clip of another size or length than the request's is `input_invalid`; so is `init_video` for an image
model, and a model that does audio (LTX, MiniMax H3, S2V) or a latent upscale.

```sh
fugue-helper-cuda vae.json   # {"op": "vae_encode", "video": "src.y4m", "latent": "init.bin", "context": {...}, "params": {...}}
fugue-helper-cuda smp.json   # {"op": "sample", "conditioning": "c.bin", "init_latent": "init.bin", "latent": "l.bin",
                             #  "context": {...}, "params": {..., "strength": 0.6}}
```

## Masks (inpainting)

`sample` and `generate` of an image model take `"mask": "mask.png"` (job input `mask` through `--serve`,
data `{"data", "format": "png"}` over HTTP; in a `generate` request the picture is `params.init_image`,
`{"image": PATH}`). The mask is read with one channel (`stbi_load(..., 1)`: a color PNG becomes gray)
and must be the request's `width` and `height`; it is checked before any model loads. White is redrawn;
the library rounds it to 0 or 1 and scales it to the latent's size, so a soft mask is hard here, unless
`fugue.soft_mask` is set (see Opt-in hunks).

What the helper does with it depends on the model, by `sd_get_model_version_name`:

- An inpaint model (`SD 1.x Inpaint`, `SD 2.x Inpaint`, `SDXL Inpaint`, `Flux Fill`, `Flex.2`): the
  `sample` needs `"image"` (the picture, as `init_image`) and ignores `init_latent`. The model's
  masked picture is a predictor input (`concat = inpaint_condition`), which the `sample` program builds
  from its own picture and mask, as a single run does; the `encode` has neither.
- Any other image model (FLUX.2 Klein, Qwen-Image Edit, FLUX.1 dev, Z-Image, SDXL, SD 1.5): the
  `sample` needs `init_latent` (the one `vae_encode` made); `image` is ignored. The library puts the
  start latent back outside the mask at every step.

`input_invalid`: a mask for a video model, for `encode`, `decode` or `vae_encode`, one of another size
than the request, an unreadable one, an inpaint model without `image`, a mask with neither
`init_latent` nor `init_image`. `smoke-http.sh` checks the size and the unreadable mask without a
model; the others need a loaded one (see the pin-move checklist).

## Step previews (`latent.sample`)

`{"op": "submit", "capability": "latent.sample", ..., "preview": "proj"}` (a key of the message, not of
`params`) makes the helper register the library's `sd_set_preview_callback` for that job. `proj` is the
cheap latent-to-RGB projection (a picture of the latent's size, a few ms), `vae` decodes the denoised latent
with the full VAE each time, `tae` uses the tiny autoencoder of the context's `taesd_path` (without one it is
`proj`); anything else is none. The interval is every Nth step so that about 8 pictures come (`ceil(steps / 8)`, from
`sample.steps`). Each picture is written beside `<output_dir>/preview.png` and renamed over it, and a status line
`{"op": "status", ..., "preview": "<output_dir>/preview.png"}` says it is new. Video models, the one-shot
mode and `--serve-http` make none. The key and the status field are ignored by a helper and a Fugue that do not
know them. Pin move: `sd_set_preview_callback`, `sd_get_preview_info` and the `U8 [C,W,H]` frames are in
`stable-diffusion.h` of the pin (checked at `3b12781`); a `proj` mode that does not know the model's latent
(it logs "No latent to RGB projection known") gives no pictures, and the run is not affected.

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

## Strength (image to image)

`params.strength` below 1 is the library's step fraction by default: it keeps the last `int(steps * strength) + 1` sigmas
of the schedule. On a shifted flow schedule (FLUX.2 at 4 steps: 1.0, 0.96, 0.88, 0.72) that is a coarse and misleading
scale (0.5 starts at 0.96, 0.75 is a full redraw), so for every model that is not of the Stable Diffusion family the helper
adds `strength_as_noise_level=true,force_first_sigma=true` to `sample.args`: strength is the sigma the start picture is
noised to, the same at any step count (`src/strength_args.h`, tested by `tests/engine/test_helper_strength.cpp`; numbers in
`docs/inpaint-outpaint-research.md` 4.7). A request that sends either key itself is not changed; a video model is not
touched. Upstream's DGL engine dropped both options, so the patch keeps them: `fugue_stage.cpp` reads them from
`sample.args` and replaces the `sigmas` statement with `fugue::noise_level_sigmas`, the old algorithm on the denoiser's
`noise_level_to_sigma`. Re-check at a pin move that `slice_sigmas_by_strength` is still how the templates trim, and that
new version names in `src/model/integrations/` are not step-fraction models.

## Opt-in hunks (not stage seams)

The patch is stage hooks, plus these hunks that change behaviour only when an option is set and leave
upstream's output byte-identical when it is off (spec 8.10). Each is listed here with its upstream PR
and checked at every pin move.

| Option | What it does | Hunks | Upstream PR |
| --- | --- | --- | --- |
| `fugue.soft_mask` (bool, image) | Differential diffusion ([arXiv 2306.00950](https://arxiv.org/abs/2306.00950), written from the paper: its repository has no licence). A mask's gray is a change strength: a latent cell with value `m` is redrawn from step `i` of `k` when `m > (k - 1 - i) / k` and held to the start latent before | `config.h`, `api/options.cpp` (the option); `api/fugue_stage.cpp` (no threshold on `mask_pixels` when on, kept for an inpaint model; `soft_mask = true` on `sample`); `dgl/models/diffusion_prediction.h` (`blend_mask`), `dgl/ops/sampling.*`, `dgl/bindings/arguments.cpp`, `registration.cpp` (a defaulted `soft_mask` argument of `sample`) | not sent yet: [the description](../docs/differential-diffusion-upstream.md) is ready for the owner (#179) |
| `fugue.init_video` (string, video; with `fugue.init_latent_in` / `_out` for a video) | Video to video: a YUV4MPEG2 clip is encoded with the VAE (or its latent is read) and replaces the start latent, and `strength` below 1 drops the first sigmas of the schedule (the high-noise pass of Wan 2.2 first), as image to image does. Not for LTX, MiniMax H3, S2V or a latent upscale | `config.h`, `api/options.cpp` (the option); `api/fugue_stage.cpp` (`read_y4m`, `fugue::load_clip`, `base` and `sigmas` of the Wan program) | not sent yet; library `strength` for a video is read by LTX only upstream |

When an upstream PR is merged and the pin reaches it, drop the hunks from the patch.

## Performance hunks (#382)

The last group of the patch makes sampling faster and leaves the pictures alone, except
SageAttention, which the helper turns on (below). Each hunk is commented in the source. Numbers are
Klein 9B INT8 ConvRot on an RTX 3090, warm (models loaded), 6 steps at 1024x1024; an edit has three
1024x1024 references.

| Hunk | What it does | Pixels | Upstreamable |
| --- | --- | --- | --- |
| `model_loader.*`, `dgl/ops/sampling.cpp` | A lazy weight load during sampling is logged on its own line (`sampling loaded the model weights first`) and left out of `sampling completed` | identical | yes |
| `core/cuda/i8_gemm.cu`, `i8_gated_residual.cu` | No Stream-K for the int8 GEMMs on Ampere (sm_80/86); Ada and newer keep it from 8192 rows. Edit sampling 19.4 s -> 18.5 s | identical | yes |
| `core/ggml_extend.cpp` | With SageAttention, V is permuted and cast to F16 in one copy | identical | yes |
| `dgl/models/vae.*` | A VAE keeps its encodes of the same pixels (content hashes, shape, posterior, tiling; at most 32 latents and 256 MB, dropped when its weights change or the context goes), only where the encode does not use the RNG (the FLUX.2 VAE). An edit's references are encoded once, not for every seed: -0.7 s a warm edit job | identical | yes |

SageAttention: on CUDA the helper adds `"sage_attn": true` to every context that does not set it
(`withDefaults` in `src/main.cpp`), so the DiT uses SageAttention where its kernel applies and flash
attention elsewhere. Text to image 3.38 s -> 3.20 s, edit 18.5 s -> 16.2 s; against flash attention
the pictures are 33-40 dB PSNR apart and look the same. A request that sends `"sage_attn": false` in
its context keeps flash attention.

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
   Keep the patch to the cut and its hooks: no change to sampling or model code, except the opt-in
   hunks and the performance hunks above. Re-read each one against the new commit: the template names the cut depends on (The
   seam), `blend_mask` and the `sample` arguments may have moved or changed. `SDCondition` is guarded by
   a `static_assert` in `fugue_stages.h`.
2. Update the commits and hashes in `CMakeLists.txt` and `packaging/PKGBUILD` together.
   `git ls-tree <new commit> ggml thirdparty/cutlass` gives the ggml and CUTLASS commits;
   `curl -sL <tarball URL> | sha256sum` gives each hash.
3. Build, then check where the patch can break. The model families are stable-diffusion.cpp's
   to test. Run each stage as its own process:
   - Z-Image Turbo, plus one `generate` run with the same request: the pixels must be the same;
   - SDXL with a negative prompt (CLIP conditioning, two sets);
   - image-to-image from a latent (`vae_encode`, then `sample` with `init_latent`);
   - IP-Adapter, PhotoMaker or PuLID: at 3b12781 the library's generation API refuses their options (they are DGL-only
     upstream) and no Fugue node sends them; a conditioning file that holds their state is refused. If a node comes to
     need one, it needs its own stage work;
   - Wan (T2V) staged: encode, sample and decode as separate processes, and one `generate`
     run of the same request: the frames must be the same;
   - Wan 2.2 I2V the same way, with `"image"` in the encode and the sample and `params.init_image`
     (`{"image": PATH}`) in the `generate` run;
   - Wan video to video: `vae_encode` of a clip (`"video"`), `encode`, `sample` with `"init_latent"` and
     `strength` 0.6, `decode`, against one `generate` with `"init_video"` and the same `strength`: the
     frames must be the same, and differ from the clip (`run_video` in `fugue_stage.cpp` reads the clip,
     encodes it, replaces `base` and drops the first sigmas);

   - an inpaint model staged equals one `generate` run: `encode`, `vae_encode` (not used by an inpaint
     model, but the stage runs), `sample` with `"mask"`, `"image"` and `"init_latent"`, `decode`,
     against one `generate` with `params.init_image` and `"mask"`; the pixels must be the same (FLUX.1 Fill
     `fluxFillFP8_v10.safetensors`, SDXL Inpaint as `model_path` plus its `diffusion_model_path`).
     If the sample stops rebuilding `inpaint_condition` from its picture and mask, the FLUX.1 Fill pictures
     differ: that is the check that the cut keeps those statements. Also staged FLUX.2 Klein 4B with a mask and `init_latent` against `generate`;
   - soft mask (`fugue.soft_mask`, #179), FLUX.2 Klein 4B with a gradient mask: (1) off, the picture is
     the same as with the hunks' original expression, and a hard mask gives the same picture on or off;
     (2) on, with a gray mask: the picture changes smoothly with the gray (white redrawn, black kept),
     and staged (`encode`, `vae_encode`, `sample` with `"mask"`, `decode`) equals one `generate` run:
     the inpainting guard test below with `FUGUE_REAL_HELPER_SOFT=1` and a gray `FUGUE_REAL_HELPER_MASK`
     (`build/test_helper_provider aRealHelperInpaintsStagedAsInOneRun`);
   - the version names: `grep -rn 'definition.name' src/model/integrations/` at the new commit, and
     which of them set `traits.is_inpaint` (`sd.cpp`, `flux.cpp` at the pin): `isInpaintVersion` in
     `src/main.cpp` and the list above must be the same set.

   These need a GPU and model files, so they are run by hand, not in CI.

   Record of the move to 3b12781 (#356, RTX 3090, CUDA). Staged equals `generate`, byte for byte: Z-Image Turbo,
   Klein 9B, SDXL with a negative prompt, image-to-image (Klein 4B, SDXL), masked Klein 4B and 9B, soft mask, FLUX.1 Fill,
   SDXL Inpaint, Wan 2.2 T2V, I2V and video to video (which also differs from its clip). Through `--serve`: stages, `proj` and `vae` previews, cancel, upscale.
   **Not run:** Wan video to video on the old helper (so no old-against-new picture for it); the two `aRealHelper*` provider tests (so the soft-mask guard through them);
   `smoke-http.sh` and any `--serve-http` job; a `tae` preview with a real `taesd_path` (no such model here); a real
   out-of-memory; any picture from the Vulkan helper (built only); IP-Adapter, PhotoMaker, PuLID; the refusal of a
   conditioning file with adapter state (added after the GPU runs).
   **Accepted by the owner, old against new pictures:** Wan 2.2 T2V (22.0 dB) and I2V (32.3 dB) and SDXL Inpaint
   (54.1 dB) differ, cause not investigated. SDXL (37.3 dB) and SDXL image-to-image (43.6 dB) differ too. The likely cause, not
   isolated: upstream now applies CFG after scaling each branch (one SDXL step is byte-identical at CFG 1 and off by 2e-5 at CFG 7). Z-Image, Klein, the masks, FLUX.1 Fill
   and upscaling are byte-identical to the old helper.
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
