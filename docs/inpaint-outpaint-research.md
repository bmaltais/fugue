# Inpainting and outpainting: research, benchmark and the router

For [#233](https://github.com/bmaltais/fugue/issues/233), feeding the canvas actions of
[canvas-actions-design.md](canvas-actions-design.md) (#231: Generative fill, Remove, Expand, Refine). Written
2026-11-22 against `main` at 6a59d9b5. Claims marked **[U]** could not be checked against a source or a
measurement. Related: [masking.md](masking.md), [spec 14.4](spec.md#144-masking-and-inpainting-167),
[spec 14.5](spec.md#145-the-canvas-revision-store-206-208), [canvas-research.md](canvas-research.md).

## In brief

- **What makes it good** (prior art, section 1): redraw a context box around the selection at the model's native
  size, pre-fill what is under the mask when the old content should not show, soften the change and composite the
  original back outside it, and give outpainting enough of the old picture to continue.
- **What Fugue can run here** (3): FLUX.2 Klein 4B and 9B (and their base, KV and turbo variants) and Qwen-Image-Edit
  2511 run; FLUX.1 Fill and Kontext, SDXL inpaint, BrushNet, the Fooocus patch and MAT do not (Load Models has one
  text encoder input, gap G2).
- **What the benchmark found** (4; 261 runs on the RTX 3090, every pixel outside the blend unchanged):
  - Klein needs the crop as its **reference**: without it, removal leaves a flat patch.
  - An edit model needs an **instruction**: an empty or descriptive prompt changes nothing (Klein and Qwen both),
    so Remove brings its own ("Remove the object and fill in what is behind it, matching the surroundings.") and
    uses the blurred crop as its reference.
  - **Outpaint needs context**: 64 px duplicated the subject, 256 px never did. Pre-fill the new area by stretching
    its edge and blurring; keep the reference.
  - **Small regions need scale**: Canvas Region redraws at the crop's own size; scaling towards 1 MP hides a
    rectangle's box.
  - Blend 16 suffices; Feather, the soft mask and more steps added nothing; Color Match ruins a replacement.
  - At 4 steps strength moves in steps: Refine 0.5 is a light pass, 0.75 a restyle.
  - **Klein 4B is as good as 9B by eye** at half the time and memory: better on the small rectangle and on Expand
    (9B handed back the pre-fill on the portrait), a little behind on the seam number for the edge removal and the
    background. Qwen-Image-Edit gives the cleanest seams at 10 to 20 times the time.
- **Recommended** (5): Klein 4B (Apache-2.0) for Generative fill, Remove, Expand and Refine, with the workflows in
  [inpaint-workflows/](inpaint-workflows/README.md); Klein 9B (non-commercial) and Qwen-Image-Edit as options.
- **The router** (6): crop frame past the canvas or a selection past the edge → Expand; no selection → Refine of the
  whole picture with a prompt, else nothing; strength below 1 or 90% selected → Refine; no prompt → Remove; else
  Generative fill. It also gives padding, region, scale, strength and Expand's overlap, context and passes.
- **Gaps** (7): thirteen proposed issues, first of all the canvas resize revision (G1), Load Models for FLUX.1 and
  SDXL (G2), a Region target size (G3), outpaint pre-fill modes (G4), a LaMa removal pack (G5) and a fallback prompt
  in the action block (G8).

1. [Prior art](#1-prior-art-what-makes-inpainting-and-outpainting-good)
2. [Known failure modes](#2-known-failure-modes)
3. [What Fugue can run on this machine](#3-what-fugue-can-run-on-this-machine)
4. [The benchmark](#4-the-benchmark)
5. [Recommendations](#5-recommendations)
6. [The router](#6-the-router)
7. [Gaps in Fugue](#7-gaps-in-fugue-proposed-issues-not-filed)
8. [Not verified](#8-not-verified)

## 1. Prior art: what makes inpainting and outpainting good

Method: documentation, model cards, issues and discussions only for GPL projects (AUTOMATIC1111, Fooocus, Krita
AI Diffusion, ComfyUI and its GPL node packs); source code only for InvokeAI (Apache-2.0). Tags: a claim with a
link was read on that page; **[S2]** rests on a search snippet or a secondary page; **[U]** could not be checked.

### 1.1 Licences of what was read or could be used

| Thing | Licence | Can Fugue use it? |
| --- | --- | --- |
| InvokeAI | Apache-2.0 | Ideas and code, with notices |
| ComfyUI-Inpaint-CropAndStitch, Acly's comfyui-inpaint-nodes, Krita AI Diffusion, Fooocus, A1111 | GPL-3.0 / AGPL | Ideas from their docs only |
| LaMa (big-lama) | Apache-2.0 ([repo](https://github.com/advimman/lama)) | Yes: the removal model to add (gap G5) |
| MAT (Places512) | "research purposes only" in its [README](https://github.com/fenglinglwb/MAT); CC BY-NC-SA [U] | Not for a default |
| FLUX.2 Klein 4B | Apache-2.0 ([card](https://huggingface.co/black-forest-labs/FLUX.2-klein-4B)) | Yes: the recommended default |
| FLUX.2 Klein 9B | FLUX Non-Commercial ([card](https://huggingface.co/black-forest-labs/FLUX.2-klein-9B)) | Opt-in, with its licence shown |
| Qwen-Image-Edit (2509, 2511) | Apache-2.0 ([card](https://huggingface.co/Qwen/Qwen-Image-Edit)) | Yes, but heavy (see 3) |
| FLUX.1 Fill dev, FLUX.1 Kontext dev | FLUX.1 dev Non-Commercial ([Fill card](https://huggingface.co/black-forest-labs/FLUX.1-Fill-dev)) | Not loadable today (gap G2) |
| SDXL Inpainting 0.1 | openrail++, "research purposes" on the [card](https://huggingface.co/diffusers/stable-diffusion-xl-1.0-inpainting-0.1) | Not loadable today (gap G2) |
| Flex.2-preview | Apache-2.0 ([card](https://huggingface.co/ostris/Flex.2-preview)), experimental inpainting | Not on this machine |

### 1.2 The recipe every good tool shares

1. **Crop a context box around the selection** and redraw only that box. A1111's *Inpaint area: Only masked*
   redraws the masked region at the target resolution and pastes it back
   ([A1111 wiki](https://github.com/AUTOMATIC1111/stable-diffusion-webui/wiki/Features)); CropAndStitch grows the box
   by a factor of the mask's size (`context_from_mask_extend_factor`, 2 = the mask's size again on each side) and
   keeps it inside the picture, shifting rather than centring
   ([README](https://github.com/lquesada/ComfyUI-Inpaint-CropAndStitch)); Krita picks the context automatically,
   with Selection Bounds, Entire Image and Custom Layer as overrides, and more context gives more coherence at more
   cost ([docs](https://docs.interstice.cloud/selections/)).
2. **Redraw at the model's native size.** Models are trained near a megapixel. InvokeAI scales the box until its
   *area* reaches the optimal size squared (1024 for SDXL, FLUX, FLUX.2 and Qwen-Image, on a grid of 16 for FLUX
   and Qwen), keeping the shape, and leaves a box alone when it is within 0.8 to 1.2 of that area
   (`optimalDimension.ts`, `graphBuilderUtils.ts`). CropAndStitch recommends 1024 for SDXL and Flux. A1111's
   only-masked does the same by its target size.
3. **Pre-fill what is under the mask** when the model starts from the picture. A1111 offers *fill / original /
   latent noise / latent nothing* (no recommendation on the wiki; "original" for small changes and "fill" for new
   content is folklore [U]). Krita: *None, Neutral, Blur, Border, Inpaint* (a small LaMa-class model)
   ([docs](https://docs.interstice.cloud/selections/)). Acly's nodes: *Fill Masked Area* (neutral, Telea,
   Navier-Stokes), *Blur Masked Area*, and *Inpaint (using Model)* with LaMa or MAT "for outpainting and object
   removal" ([README](https://github.com/Acly/comfyui-inpaint-nodes)). InvokeAI infills with LaMa by default today
   (also PatchMatch, tile 32, solid colour, cv2) and pastes the original back over it (`infill.py`).
4. **Soften the change in two separate places.** InvokeAI keeps a *denoise* gradient (`create_gradient_mask`,
   edge radius 16 px, Gaussian blur by default, minimum denoise 0) apart from the *paste-back* fade (mask blur
   16 px, box), so the model blends the content and the composite hides the last pixels. Krita has *Feather* (a
   percentage of the selection, the denoise falloff) and *Blend* (absolute pixels, compositing only), and a
   red/orange/green/blue zone picture of the mask. Differential diffusion ([arXiv 2306.00950](https://arxiv.org/abs/2306.00950))
   makes the gray of a mask a per-pixel strength without training; ComfyUI ships it as a node.
5. **Always composite the original back outside the mask.** The VAE round trip changes every pixel it touches:
   FLUX.1 Fill's card lists "slight colour shifts" and "lines at the edges of the filled area"; SDXL inpaint's card
   notes the lossy autoencoder. Fugue already does this (Canvas Write and Stitch keep every pixel outside the blend).
6. **Fix a colour shift with a colour match** measured on the kept part (Acly's *Color Match (masked)*), or with a
   low-denoise seam pass over the boundary (InvokeAI's old Unified Canvas: seam size 96, blur 16, strength 0.7
   [S2, old docs]; Ultimate SD Upscale's seam fix at denoise 0.15 to 0.35, mask blur 8 to 16, padding 32
   ([FAQ](https://github.com/Coyote-A/ultimate-upscale-for-automatic1111/wiki/FAQ))).

### 1.3 Model kinds

- **Inpaint-trained models** read the mask and the masked picture as extra input channels: FLUX.1 Fill (16 noise +
  16 masked picture + 64 packed mask channels; card example 50 steps, guidance 30), SDXL Inpainting (9 channels;
  strength 0.99 on the card, "less sharp" at 1.0), Flex.2 (noise + masked picture + mask + control), BrushNet, the
  Fooocus inpaint patch (SDXL only). InvokeAI always denoises them fully.
- **Edit models** take a picture and an instruction, no mask: FLUX.1 Kontext, Qwen-Image-Edit, FLUX.2 Klein. Krita
  says to run them at 100% strength, because lower values often change nothing, and that a selection mostly serves
  to bring the work down to about a megapixel ([docs](https://docs.interstice.cloud/edit-models/)). In Fugue they
  are used in two ways at once: the masked latent (the helper puts the picture back outside the mask at every step)
  and the crop as the edit reference in Text Encode.
- **Removal models** (LaMa, MAT) fill a hole from its surroundings with no prompt; LaMa generalises from 256 px
  training to about 2k ([repo](https://github.com/advimman/lama)). The common two-stage recipe is LaMa first, then a
  low-denoise diffusion pass over it (Krita's *Inpaint* fill mode, Acly's node) [S2 as a recipe].

### 1.4 Outpainting

- **Context.** Old InvokeAI docs say to keep 50 to 60% of the box as existing picture [S2]; a newer support article
  reportedly says about 25% missing [S2, conflicting]. Fooocus users keep each new tile near a megapixel and expand
  one side at a time [S2, [discussion 414](https://github.com/lllyasviel/Fooocus/discussions/414)].
- **Pre-fill.** InvokeAI infills (LaMa) and then adds noise only in the masked area; Krita's *Expand* ignores the
  selection's content and matches the border colours ("more of the same"); A1111's poor man's outpainting runs at
  maximum denoise with an ancestral sampler.
- **Strength.** InvokeAI uses the same img2img strength as for inpainting and remaps it for FLUX/FLUX.2
  (`1 - strength^0.2`) because most visible change sits above 0.9; FLUX Fill always runs 0 to 1.
- **Photoshop Generative Expand**: the crop tool dragged past the canvas, then Generative Expand in the contextual
  task bar, the prompt may be empty; each pass adds more [S2: helpx.adobe.com returned 403].

### 1.5 Removal and Photoshop

Photoshop's guidance for removal is an empty Generative Fill prompt, the pixels around the selection guiding the
result, with "remove" typed if that fails; users report random objects from an empty prompt [S2]. Krita has a
dedicated *Remove Content* generation mode that "continues the surroundings" and *Replace Background*
(experimental) ([wiki](https://github.com/Acly/krita-ai-diffusion/wiki/Inpainting)).

### 1.6 Pages that failed

403: helpx.adobe.com (Generative Fill), stable-diffusion-art.com. 404: docs.interstice.cloud/inpaint/. DNS failure:
support.invoke.ai (its content is from snippets only). Not checked: BrushNet, Kontext colour-shift reports, Krita's
feather and grow defaults, Fooocus's erode/dilate and "respective field" defaults.

## 2. Known failure modes

| Failure | Why | Seen here | Fix |
| --- | --- | --- | --- |
| **Seams** (a line or halo at the selection's edge) | The model's redraw differs from the picture at the border; a hard composite shows it | Blend 0 gives a visible edge (seam 1.55 on the chair against 1.27 with Blend 16); a rectangle with padding 0 gives a hard box edge (seam 5.4), because Canvas Write's blend stops at the Region's box | Padding of at least Blend + Feather (the router's 64 px floor); Blend 16; enough context (4.2) |
| **Colour shift through the VAE** | The decoder does not give back the encoder's colours exactly | Small with FLUX.2's VAE: the mean luma shift in the blend ring is under 2 levels in every Fill and Remove case but the hard edge case | Composite the original back (Fugue does); Color Match for Remove and Expand only, never for a replacement (4.4) |
| **Content too small for the model** | A small crop is redrawn at its own size, far below the model's native megapixel | Canvas Region's `scale` is a fixed multiple and its cap only shrinks, so a 300 x 340 selection is redrawn at 432 x 464 | The router's scale (up to 1 MP); a Region "target megapixels" mode (gap G3) |
| **Hard masks** | A pixel-exact mask cuts through the object's shadow, hair or antialiasing, which stays | The rough cat mask leaves a rim of the old window around the cat in the background case | Grow the selection 8 to 16 px; for a subject, a segmentation mask (Remove Background, Segment), inverted for a background |
| **Edit models that take no mask** | Kontext, Qwen-Image-Edit and Klein edit the whole picture | Not a problem in Fugue's graph: the masked latent holds everything outside the mask at every step, and the crop is the edit reference; every result had all pixels outside the blend byte-for-byte equal (`outside_exact` 1.0) | Keep both: masked latent and reference (4.3) |
| **No reference: Klein loses the scene** | Without the crop as reference, Klein at 4 steps sees only the masked latent | Removal leaves a flat lamp-shaped patch; a replacement loses the shape and the light of the old chair | Always give Klein the crop as reference (4.3) |
| **The reference brings the object back** | For removal, the reference still shows the object | A faint lamp silhouette with a large padding or `scale` 2 | Remove: reference the blurred pre-fill, not the original (4.3) |
| **The prompt is drawn literally** | A distilled edit model follows the words | "Empty background ..." as the hidden Remove prompt paints a white blank over the plant at the window | A removal instruction, not a description (4.5) |
| **Outpaint repeats the subject** | The prompt names the subject and the region shows little of it, so the model draws it again in the new strip | The portrait extended upward gets a second, smaller woman above her head | Keep a large context band, prompt the whole scene as an instruction (4.6) |
| **Denoise below 1 does nothing** | With 4 distilled steps the strength falls into steps: 0.3 and 0.4 give the same picture, 0.5 and 0.6 another, 0.8 and 1 the same | A Refine at 0.5 changes only details; Remove at 0.6 leaves the lamp | Refine 0.5 (a light pass) or 0.75 (a restyle); Fill and Remove at 1 |
| **Edge artifacts** | A selection on the canvas's edge has no picture beyond it | Canvas Region moves the box inside (as Crop to Mask does); nothing beyond the edge was needed | The router keeps the padding on the inner sides (6) |

## 3. What Fugue can run on this machine

Checked with `fugue validate` and real runs, isolated `FUGUE_HOME`, `model_paths` = `/mnt/windows/models`, the CUDA
helper of `~/github/fugue`. Load Models takes one diffusion model, **one** text encoder (a language model, or UMT5
for Wan) and a VAE.

| Model (file) | Loads? | Notes |
| --- | --- | --- |
| FLUX.2 Klein 9B Q8 (`flux-2-klein-9b-Q8_0.gguf`, Qwen3-8B Q8, `flux2-vae`) | **Yes, run** | 4 steps, guidance 1. Peak 15 to 24 GB of the card's 24 GB, desktop included (4.7 GB) |
| FLUX.2 Klein 4B Q8 (`flux-2-klein-4b-Q8_0.gguf`, Qwen3-4B Q8) | **Yes, run** | 4 steps. Peak 10 to 21 GB, mostly 10 to 14 |
| FLUX.2 Klein base 4B Q8 | **Yes, run** | Not distilled: 24 steps, guidance 4 (4.8) |
| FLUX.2 Klein 9B KV Q8 (`flux-2-klein-9b-kv-Q8_0.gguf`) | **Yes, run** | 4 steps; no gain over 9B (4.8) |
| FLUX.2 Klein base 9B Q8 | Validates | Not run (the 4B base was) |
| `flux.2-klein-turbo-24steps-FP8.safetensors` | **Yes, run**, with Qwen3-4B (it reads 7680-wide features: a 4B) | Run at 8 steps; no gain over Klein 4B (4.8) |
| Qwen-Image-Edit 2511 Q3_K_L / Q4_K_M, with `qwen_2.5_vl_7b_fp8_scaled` and `qwen_image_vae` | **Yes, run** | 20 steps, guidance 2.5; one to two minutes a run, 17 to 20 GB peak (4.8) |
| Qwen-Image-Edit 2509 Q5_K_M, Qwen_Image_Edit Q6_K | Not tried | 14 and 16 GB files: with the 9 GB encoder they exceed the free memory |
| FLUX.1 Kontext dev Q8, FLUX.1 Fill dev FP8 (`flux1FillDevFp8_v10`) | **No** | FLUX.1 reads CLIP-L **and** T5; Load Models has one encoder input and refuses T5 for an image model (`model_mismatch`, with a self-contradicting message: "it is a T5 encoder, and Load Models takes a language model or a T5 encoder"). The helper itself can run FLUX.1 Fill (its README tests `fluxFillFP8_v10`); the node cannot feed it (gap G2) |
| `svdq-int4_r32-flux.1-fill-dev`, other `svdq-int4` files | **No** | SVDQuant (Nunchaku) is flagged unloadable by `model_shape` |
| SDXL inpaint (`unet/sdxl_inpaint_diffusion_pytorch_model.fp16`) | **No** | A bare diffusers UNet without its CLIP pair; same Load Models limit (gap G2) |
| `inpaint/`: Fooocus inpaint patch and head, BrushNet, MAT Places512 | **No** | No node runs them (ComfyUI-specific formats); LaMa/MAT support is gap G5 (MAT is non-commercial) |

Generate Image and Edit Image go through a provider (an Omarchy model server), not the helper, and Enhance Prompt
needs a text model from a provider. None was started here, so prompt enhancement was simulated by hand (4.7).

## 4. The benchmark

### 4.1 Set-up

- **Machine**: RTX 3090 (24 GB, 4.7 GB held by the desktop), the CUDA helper of `~/github/fugue`
  (`FUGUE_HELPER`), a Release `fugue` built from this branch, an isolated `FUGUE_HOME`
  (`~/github/fugue-inpaint-out/home`) whose only setting is `model_paths`. Offscreen; no window.
- **Path**: the real canvas path. Each base picture is imported (`fugue canvas import`); each variant is a canvas
  workflow, Canvas Region (the base revision pinned, a mask from a PNG or a rectangle) -> [Fill Masked] -> Text Encode
  (the crop as reference) -> VAE Encode (with the Region's mask) -> Sampler -> VAE Decode -> [Color Match] -> Canvas
  Write (Keep as candidate on, so every variant starts from the same head), run with `fugue run --wait`; the
  candidate is exported with `fugue canvas export`. One factor changes at a time against a base setting: Klein 9B Q8,
  padding 64, scale 1, cap 1 MP, feather 0, multiple 16, Blend 16, no pre-fill, reference on, 4 steps, guidance 1,
  denoise 1, seed 42 (a second seed, 7, for the model and final comparisons).
- **Harness**: `~/github/fugue-inpaint-out/bench/` (`make_cases.py` the masks and cut pictures, `bench.py` the
  variants and runs, `metrics.py` the numbers, `sheet.py` the contact sheets). Every run keeps its workflow, its PNG
  and a JSON record (parameters, per-node seconds, peak GPU memory, metrics) in `runs/<group>/`.
- **Pictures**: the bundled sample photos of fugue-workflows (`reading-nook.jpg` and `cat.jpg`, 1024 px, in the
  inpaint and edit templates), and a portrait made for this with Klein 9B (seed 7, `base/portrait.png`). For the two
  outpaint cases the picture is cut first (the cat to 640 px wide, the portrait by 304 rows at the top), so the
  original is the ground truth.

| Case | Picture | Selection (share of the picture) | Prompt | Action |
| --- | --- | --- | --- | --- |
| remove-lamp | reading nook | the floor lamp, a lasso (2%) | the hidden Remove prompt | Remove |
| replace-chair | reading nook | the armchair (17%) | "A mustard yellow leather armchair, in the same lamplight." | Fill |
| small-rect | reading nook | a rectangle over the lamp (10%) | "A tall brass floor lamp with a round white globe shade, glowing warmly." | Fill |
| background | cat | everything but the cat (65%) | "A cat sitting on a stone garden wall, a sunny cottage garden full of flowers behind it." | Fill |
| edge-plant | cat | the plant on the left and bottom edges (15%) | the hidden Remove prompt | Remove |
| refine-chair | reading nook | the armchair | "A green velvet armchair with deep folds in the fabric, in warm lamplight." | Refine |
| expand-lr | cat cut to 640 wide | 192 px added left and right, 32 px overlap | a description of the scene | Expand |
| expand-up | portrait cut by 304 rows | 304 rows added at the top | a description of the scene | Expand |

**Measures** (`metrics.py`; the person's eye decides, the numbers back it up):
- `outside_exact`: the share of pixels beyond the mask + Feather + Blend (+3 px) that are byte-equal to the base.
  **1.0 in 260 of the 261 runs**, and 0.99998 in the last (Blend 48 on the background case: the blend's tail
  reaches past the metric's ring): no action touched a pixel it should not have.
- `seam`: the gradient at the composite's outer edge and at the mask's edge, against rings 6 to 8 px either side,
  divided by the same ratio on the base picture. 1.0 adds no edge; above about 1.3 a line or a ghost shows.
- `ring_diff` and `ring_shift`: the mean absolute difference and the mean signed luma difference to the base in the
  Blend ring just outside the mask (0 to 255): how far the redraw strays at the border and whether it is lighter or
  darker.
- Time (`node_s`, per node, and the wall time of the run) and peak GPU memory (the whole card, sampled every 0.25 s,
  so it includes the desktop's 4.7 GB).

The numbers below are seed 42 on Klein 9B unless said otherwise. Contact sheets are in
`/home/bernard/github/fugue-inpaint-out/sheets/` (named `<group>__<case>.png`, `-crop` for a close-up) and every picture in `/home/bernard/github/fugue-inpaint-out/runs/`.

### 4.2 Region: padding, scale, cap, multiple

| Variant | remove-lamp | replace-chair | small-rect | What the eye sees |
| --- | --- | --- | --- | --- |
| padding 0 | 1.67 | 1.76 | 5.39 | a lamp-shaped ghost; the chair lit flat; the rectangle's box shows hard (Write's blend cannot reach past the Region's box) |
| padding 64 (base) | **1.27** | 1.27 | 2.31 | clean removal; the new lamp's glow ends at the rectangle |
| padding 192 | 1.51 | **1.20** | 2.45 | faint ghost; the chair sits best in the room's light |
| padding 384 | 1.44 | 1.34 | 2.54 | faint ghost; a second lamp pole appears in the rectangle |
| scale 2 (cap 1 MP) | 1.60 | 1.35 | **1.66** | ghost; the rectangle no longer shows and the lamp is sharper |
| multiple 64 | 1.34 | 1.42 | 2.85 | worse everywhere (the crop is resized to another shape) |
| cap 0.5 or 1.5 MP | = base | = base | = base | no effect: these regions are below 0.5 MP, which is the point of gap G3 |

Sheets: `region__remove-lamp-crop.png`, `region__replace-chair-crop.png`, `region__small-rect-crop.png`.

- **Fill wants context about half the selection's size.** The chair (476 px tall) is best at 192 to 384 px; at
  0 the light is wrong, at 64 it is good but the shadow under it is weaker.
- **Small regions want scale more than padding.** Canvas Region redraws at its own size: 432 x 464 for the 10%
  rectangle. Scale 2 (864 x 944) was the only change that hid the rectangle's box. The router therefore scales
  every region below 0.8 MP up towards 1 MP (6).
- **Removal ghosts come from the reference, not from the padding**: with the original crop as the reference, more
  room (192, 384, scale 2) lets Klein redraw the lamp's silhouette. The fix is in 4.3, and with it Remove takes the
  router's padding and scale like Fill (the final runs, 4.9).
- Never pad less than Feather + Blend: Canvas Write's layer is cut at the Region's box.

### 4.3 Reference and pre-fill

| Variant | remove-lamp | edge-plant | replace-chair | What the eye sees |
| --- | --- | --- | --- | --- |
| reference = the crop, no pre-fill (base) | 1.27 | 10.3 | 1.27 | removal clean; plant: white blank (the prompt, 4.5) |
| no reference | 2.17 | | 1.38 | **a flat lamp-shaped patch**; the chair loses its shape and the room's light |
| pre-fill gray | 1.29 | 10.8 | 1.26 | as the base |
| pre-fill blur | 1.29 | 10.2 | **1.23** | as the base |
| pre-fill blur, reference = the pre-filled crop | **1.18** | 5.0 | 1.58 | the cleanest removal; the chair forgets the old one's shape and light |
| pre-fill blur, no reference | 2.62 | | 1.74 | the patch again |
| pre-fill blur, denoise 0.85 | 1.29 | | 1.23 | identical to denoise 1 (4.7) |

Sheets: `prefill__remove-lamp-crop.png`, `prefill__edge-plant-crop.png`, `prefill__replace-chair-crop.png`.

- **Klein must have the crop as its reference.** Without it, Klein at 4 steps does not inpaint: it leaves a flat
  patch. (The outpaint template's note that the reference made Klein copy the gray border holds only for a gray
  border with 64 px of context: with an edge-extended pre-fill and 256 px of context the reference is right, 4.6.)
- **What the reference shows decides what comes back.** For a replacement, the original crop (the chair's shape and
  light); for a removal, the crop with the object blurred away (Fill Masked `blur`), so the object is not shown to
  the model again.

### 4.4 Blend, feather, soft mask and colour match

| Variant | remove-lamp | replace-chair | background | edge-plant |
| --- | --- | --- | --- | --- |
| Blend 0 | 1.34 | 1.55 | 4.32 | 26.6 |
| Blend 16 (base) | 1.27 | 1.27 | 2.64 | 10.3 |
| Blend 48 | 1.28 | 1.26 | 2.40 | 11.9 |
| Feather 16, Blend 16 | 1.27 | 1.27 | 2.64 | 10.3 |
| Feather 32, Blend 32 | 1.28 | 1.26 | 2.50 | 11.1 |
| Feather 32 + soft mask (differential diffusion) | 1.74 | 1.38 | 2.90 | 11.4 |
| Color Match (reference = the crop, mask = the selection) | 1.30 | **1.15** | 2.65 | 7.1 |

Sheets: `blend__replace-chair-crop.png`, `blend__background.png`.

- **Blend 16 is enough; 0 shows an edge.** Blend 48 and Feather add nothing visible on these cases.
- **Soft mask did not help** here: with a feathered mask, the half-redrawn ring of a 4-step distilled model shows as
  a soft ghost (lamp 1.74). It stays an Advanced option; it is not a default. [U] whether it helps a non-distilled
  model with more steps.
- **Color Match wins the numbers and loses the picture for a replacement**: it pulls the new mustard chair towards
  the old green one (olive), because it moves the whole crop's colours to the reference's. Use it only where the
  new content should match the old (Remove, Expand), never for Generative fill.
- The background case's high seam is the hand-drawn cat mask: a rim of the old window stays round the cat. A
  background change needs a real subject mask (Remove Background or Segment, inverted, grown a few pixels).

### 4.5 The Remove prompt

The base's hidden prompt, "Empty background, continued seamlessly from the surroundings, nothing in the way.", was
taken literally on the plant at the bright window: a white blank. Variants (Klein 9B unless noted):

| Prompt | remove-lamp | edge-plant | What the eye sees |
| --- | --- | --- | --- |
| "Empty background, ..." (old) | 1.27 | 10.3 | lamp removed; plant replaced by white |
| empty | 1.00 (unchanged: 3.0 inside) | 1.05 (unchanged) | **nothing removed**: Klein gives back its reference |
| "Continue the surrounding background into this area." | 1.00 (unchanged) | 1.05 (unchanged) | nothing removed |
| a description of the scene without the object | 1.01 (unchanged) | 2.50 | lamp kept; plant replaced by a garden through a new window |
| empty, blurred reference | 2.83 | 1.73 | a new lamp; **a second cat** |
| **"Remove the object and fill in what is behind it, matching the surroundings."** | 1.34 | 1.61 | lamp gone; plant gone, window and sill continued |
| the same, blurred pre-fill as reference | **1.21** | 1.66 | the same, cleanest on the lamp |
| the same, padding 256 | 1.41 | | lamp gone |
| the same on Klein 4B | 1.32 | **1.30** | lamp gone; plant gone |

Sheets: `removeprompt__remove-lamp-crop.png`, `removeprompt__edge-plant-crop.png`.

- **An edit model needs an instruction, not a description.** With an empty or a descriptive prompt Klein returns
  the reference, so the object stays; with a blurred reference and no instruction it invents something (a lamp, a
  cat). Photoshop's "empty prompt removes" does not carry over to FLUX.2 Klein. The Remove action's hidden prompt is
  the instruction above; Fill with an empty prompt must not be Remove with an empty prompt, which is why the router
  sends an empty prompt to Remove and the Remove workflow brings its own words.

### 4.6 Expand (outpaint)

The canvas resize revision does not exist yet (gap G1), so the benchmark makes the grown canvas itself
(`metrics.py expand`): the cut picture on a larger canvas whose new area holds one of the pre-fills, then a Fill of
the new strip plus the overlap.

| Variant | expand-lr | expand-up | What the eye sees |
| --- | --- | --- | --- |
| gray pre-fill, context 64, reference (base) | 1.19 | 1.61 | sides fine; **a second, smaller woman above her head** |
| no reference | 3.08 | 1.31 | a light band at each seam on the sides; still the duplicate |
| pre-fill: edge pixels stretched | 1.03 | 1.69 | sides fine |
| pre-fill: edge stretched and blurred (24 px) | **1.00** | 1.61 | sides seamless |
| pre-fill: border colour with noise | 1.06 | 1.38 | |
| pre-fill: transparent (what a resize would hold) | 1.51 | 1.58 | works, but the worst of the pre-fills |
| gray, then Fill Masked blur | 1.34 | 1.79 | worse: blur across a one-sided hole pulls the gray in |
| overlap 0 / 64 | 0.99 / 1.15 | 0.99 / 1.38 | 32 is the middle; 0 puts the composite's edge on the old picture's own edge |
| **context 256 px** | (whole canvas) | **1.02** | **no duplicate**: her hair continues upward into the park |
| context 512 px | (whole canvas) | 1.02 | as 256 |
| context 0 | (whole canvas) | 2.82 | a separate picture above a hard line |
| Feather 32 + soft mask | 1.19 | 1.71 | no gain |
| Color Match | **1.01** | 1.52 | helps a little; fine here because the new content should match |

The prompt, with context 256 or 512 on expand-up:

| Prompt | Seam | What the eye sees |
| --- | --- | --- |
| a description of the whole scene, subject included | 1.02 | good |
| only what the new area should hold ("trees and sky of a park") | 1.01 | **her head is cut flat**: hair stops where trees begin |
| **"Extend the picture upward: continue her hair and the park behind her."** | 1.02 | good |
| a generic instruction, "Extend the picture beyond its edges, continuing the scene." (edge-blur fill, final runs, 4.9) | 1.00 | good on Klein 4B; **Klein 9B gave back the blurred fill** |
| an enhanced, long prompt (4.7) | 1.02 | good, but it adds what was not asked for |

Sheets: `expand__expand-lr.png`, `expand__expand-up.png`, `expandprompt__expand-up.png`.

- **Context is the one factor that matters for outpainting.** Outpaint duplicated the subject at 64 px of context
  in every pre-fill and prompt variant, and never at 256 or 512. The router keeps 256 px of the old picture in the
  region (6).
- **Pre-fill the new area from its edge, blurred**, not gray and not transparent: the Expand action needs that fill
  when the canvas grows (gaps G1, G4).
- **Keep the reference.** Without it the sides get a light band at the seam.
- **A generic instruction works on Klein 4B**, so Expand needs no prompt from the person (as Photoshop allows); a
  prompt naming only the new content is the one that fails. On Klein 9B the generic instruction with the edge-blur
  fill gave back the fill unchanged on the portrait (4.9): another reason for 4B as the default, and a risk to
  track (G13).

### 4.7 Sampling, refine and prompt enhancement

| Variant (replace-chair / remove-lamp) | Seam | Note |
| --- | --- | --- |
| 4 steps (base) | 1.27 / 1.27 | |
| 2 steps | 1.35 / 1.51 | softer, a faint ghost |
| 8 steps | 1.32 / 1.28 | no better, twice the sampling time |
| guidance 2 | 1.25 / 1.65 | Klein is distilled for guidance 1; the removal gets a ghost |
| denoise 0.8 | identical to 1 | |
| denoise 0.6 | 1.27 (inside change 33 instead of 42) / 1.00 (lamp **not removed**) | |

At 4 steps the strength falls into steps: 0.3 and 0.4 give the same picture, 0.5 and 0.6 another, 0.75 a third,
0.8 to 1 the full redraw (measured: identical files within each group). Refine on the chair:

| Refine strength | Inside change | What the eye sees |
| --- | --- | --- |
| 0.3 or 0.4 | 3.0 | almost nothing |
| **0.5 or 0.6** | 8.5 | the same chair, its velvet re-rendered with deeper folds |
| 0.75 | 18.7 | a restyle: the fabric draped differently |
| 0.5, 8 steps | 2.7 | almost nothing: more steps do not make the scale finer |
| 0.5, no reference | 21.2 (seam 1.78) | a different, lighter chair: the reference keeps Refine a refinement |
| 0.5, Feather 32 + soft mask | 9.2 (seam 1.20) | no gain over the hard mask (1.09) |

Sheet: `refine__refine-chair-crop.png`.

**Prompt enhancement** could not be run: Enhance Prompt needs a text model from a provider, and none was started
(starting the owner's Omarchy model server would have touched the owner's setup). A hand-written "enhanced" prompt
in its style (60 words of materials, light and lens) made the chair smaller and tufted, not the shape of the
selection (seam 1.41 against 1.27), and added a light band on the sides of expand-lr (1.42 against 1.19); on
expand-up with context 512 it was as good as the plain prompt. **Enhance stays off by default** for canvas actions:
the prompt there describes a part, and enhancement writes a whole scene. [U] for the real node.

### 4.8 Models

| Case | Klein 9B Q8 | Klein 4B Q8 | Klein base 4B (24 steps, guidance 4) | Klein 9B KV | Klein turbo FP8 (8 steps) | Qwen-Image-Edit 2511 Q4_K_M (20 steps, guidance 2.5) | Qwen-Image-Edit 2511 Q3_K_L |
| --- | --- | --- | --- | --- | --- | --- | --- |
| remove-lamp (old prompt) | 1.31 | **1.18** | 1.15 | 1.30 | 1.22 | 1.03, lamp **not removed** | 1.04, not removed |
| replace-chair | 1.30 | 1.35 | 1.35 | 1.25 | 1.24 | **1.07** | 1.08 |
| small-rect | 2.22 | **1.56** | | | | | |
| background | **2.50** | 2.77 | 2.56 | 2.84 | 3.09 | 3.94 | 2.82 |
| edge-plant (old prompt) | 9.77 | **3.46** | | | | | |
| refine-chair (0.5) | 1.05 | 1.06 | | | | | |
| expand-lr (context 64) | 1.25 | 1.57 | 1.13 | 2.17 | 1.62 | **1.00** | 1.00 |
| expand-up (context 64) | 1.41 | 1.29 | | | | | |
| **sampling time**, median (range) | 8.4 s (4.3 to 38) | **3.8 s** (1.9 to 7.2) | 31 s | 9.0 s | 6.0 s | 84 s (40 to 122) | 91 s |
| **whole run**, median | 14.5 s | **7.4 s** | 36 s | 20 s | 12 s | 117 s | 103 s |
| **peak GPU memory**, median (max), desktop's 4.7 GB included | 15.9 GB (23.9) | 13.2 GB (21.0) | 10.5 GB | 16.1 GB | 13.3 GB | 19.3 GB (20.4) | 16.8 GB |

Seeds 42 and 7 for the two Klein models (seam, mean of the two). Sheets: `model__<case>.png` and `model-extra__<case>.png`.

- **Klein 4B is as good as 9B** on these cases, better on the hard ones (the edge removal, where 9B paints the
  window white and 4B continues it; the small rectangle, where 4B's lamp glow does not show the box), and about twice
  as fast with less memory. The chair and the background are a draw by eye.
- **Qwen-Image-Edit 2511** gives the cleanest seams and the most natural light (replace-chair, expand-lr) but takes
  10 to 20 times as long (one to two minutes a run) and nearly all of the card. Like Klein, it changes nothing when
  the prompt describes rather than instructs (the lamp stays). It is Apache-2.0: the quality option for a larger
  card or a patient person, not the default.
- **Klein base 4B** (24 steps) and **Klein turbo** gain nothing over the distilled 4B at 4 steps; **9B KV** is 9B
  with a worse expand-lr seam.
- Time is dominated by loading when the helper changes model (a first run with a model takes 20 to 50 s); the
  medians are of runs with the model already loaded. Expand-lr, a whole 1 MP canvas plus a 1 MP reference, is the
  slowest case for every model (Klein 9B 28 to 38 s of sampling).

### 4.9 Final runs: the recommended workflows

Each case run through its recommended workflow (5.1) with the router's padding and scale, on Klein 4B and 9B,
seeds 42 and 7. Seam (seed 42 / seed 7):

| Case | Klein 4B | Klein 9B | What the eye sees |
| --- | --- | --- | --- |
| replace-chair (padding 238) | 1.17 / 1.00 | 1.36 / 1.33 | a yellow leather chair in the room's light, both models |
| small-rect (padding 170, scale 1.52) | 1.67 / 1.82 | 2.30 / 1.92 | 4B: the new lamp's glow hardly shows the box; 9B: a lighter rectangle on the wall |
| background (padding 256) | 2.82 / 2.72 | 2.64 / 2.35 | the garden; the rim of the hand-drawn mask stays (G9) |
| remove-lamp (padding 158, scale 1.83, blurred reference, instruction) | 1.00 / 1.38 | 1.09 / 1.18 | lamp gone, wall and books continued; no ghost |
| edge-plant (padding 256, the same) | 1.91 / 1.73 | 1.54 / 1.45 | plant gone, window frame and sill continued (the old prompt gave a white blank) |
| refine-chair (0.5) | 1.01 / 1.00 | 1.06 / 1.09 | the chair kept, its velvet re-rendered |
| expand-lr (edge-blur fill, context 256, generic instruction) | 1.03 / 1.07 | 1.01 / 1.00 | a plant on each side, sill and window continued |
| expand-up (the same, scale 1.48) | 1.00 / 0.99 | 1.00 / 1.00 | **4B: hair and park continued, no duplicate. 9B: gave back the blurred fill unchanged**, both seeds |

Extra variants on 4B: Color Match on Remove and Expand changed nothing visible (lamp 1.00 both; expand-lr 1.05
against 1.03; expand-up 1.00 against 1.00), so it is left out; Blend 48 or Feather 32 with the soft mask on the
small rectangle did not hide its box better (1.66, 1.88 against 1.67); Remove at scale 1 instead of 1.83 is as
clean (1.00); expand-up at scale 1 is as good; the person's scene prompt instead of the generic instruction is
good on expand-up and leaves a slight band on expand-lr (1.65). A second generic wording ("Replace the blurred,
smeared border with a natural continuation of the scene.") did not stop 9B from handing back the fill (one seed of
two) and changed less on 4B: the first wording stays. Klein 4B sampled in 5 to 10 s per run on every action.

The same four files from [inpaint-workflows/](inpaint-workflows/README.md) were then run unchanged from the command
line (`--set` for the canvas, the rectangle, padding and scale): Fill, Remove and Refine landed as three layers on a
fresh canvas of the reading nook in 8 to 11 s each, and Expand on the grown portrait continued her hair and the
park. Pictures: `/home/bernard/github/fugue-inpaint-out/shipcheck/nook-after.png`, `/home/bernard/github/fugue-inpaint-out/shipcheck/portrait-after.png`.

Sheets: `final__<case>.png` (and `-crop`), `expandfix__expand-up.png`.


## 5. Recommendations

### 5.1 The four workflows

Saved as scratch files in [inpaint-workflows/](inpaint-workflows/README.md), each a canvas workflow with the
`action` block of the design. The router (6) supplies the selection, `padding` and `scale` per run; the numbers
below are the rest.

| | Generative fill / replace | Remove | Expand | Refine |
| --- | --- | --- | --- | --- |
| Model | Klein 4B Q8 | Klein 4B Q8 | Klein 4B Q8 | Klein 4B Q8 |
| Region padding | router: half the longer side, 64..256 | router: the same, at least 128 | 256 (context kept from the old picture) | router, as Fill |
| Region scale | router: up to 1 MP | router | router | router |
| Cap, feather, multiple | 1 MP, 0, 16 | 1 MP, 0, 16 | 1 MP, 0, 16 | 1 MP, 0, 16 |
| Pre-fill | none | Fill Masked `blur` | the resize fills the new area with stretched, blurred edge pixels (G1, G4) | none |
| Text Encode reference | the crop | the **pre-filled** crop | the crop | the crop |
| Prompt | the person's | fixed: "Remove the object and fill in what is behind it, matching the surroundings." | the person's (a whole-scene description) or the fallback "Extend the picture beyond its edges, continuing the scene." | the person's, fallback "Refine the details, keeping everything as it is." [U] |
| Steps, guidance, strength | 4, 1, 1 | 4, 1, 1 | 4, 1, 1 | 4, 1, **0.5** (0.75 to restyle) |
| Soft mask | off | off | off | off |
| Color Match | **off** (it pulls the new object to the old colours) | off (no visible change, 4.9) | off (no visible change, 4.9) | off |
| Canvas Write Blend | 16 | 16 | 16 | 16 |
| Prompt enhance | off | n/a | off | off |
| Selection | required | required (no prompt) | bounds (crop frame past the canvas, or a selection past the edge) | required, or none with a prompt |

### 5.2 Which model, and when the best one is missing

- **Default: FLUX.2 Klein 4B Q8, for every action.** Apache-2.0, so a film team can ship with it; 8.9 GB to
  download (`flux-2-klein-4b-Q8_0.gguf` 4.30 GB, `Qwen3-4B-Q8_0.gguf` 4.28 GB, `flux2-vae.safetensors` 0.34 GB, the
  same files as the `flux2-klein-4b` template); 10 to 14 GB at peak; about 4 s of sampling and 7 s a run on the
  3090. By eye it matched Klein 9B on every case, was better on the small rectangle and on Expand (where 9B handed
  back the pre-fill), and a little behind on the seam number for the edge removal and the background (4.8, 4.9).
- **Klein 9B Q8** (FLUX Non-Commercial; 19.3 GB with Qwen3-8B; up to 24 GB at peak on a 24 GB card): an Advanced
  choice with its licence line, for people who already have it. It is what the bundled templates use today.
- **Qwen-Image-Edit 2511** (Apache-2.0; Q4_K_M 12.6 GB + the Qwen2.5-VL 7B encoder 8.9 GB + its VAE 0.24 GB):
  the quality option for a large card. The cleanest seams and light in the benchmark, at one to two minutes a run.
  Q3_K_L (10.1 GB) looks the same and peaks lower (17 GB).
- **Not today**: FLUX.1 Fill and Kontext, SDXL inpaint (Load Models cannot feed them, G2); LaMa and MAT (no node,
  G5; MAT is research-only).
- **When the default is missing**, the bar shows the download line for the 4B files (8.9 GB, Apache-2.0); when 9B
  is installed and 4B is not, the action may run on 9B after the person accepts its licence once. A card with less
  than about 12 GB free needs a smaller quantization of 4B [U: not measured here].

### 5.3 When to use which: inpaint or outpaint

- **Inside the canvas → inpaint** (Fill, Remove, Refine), the selection's box plus context, redrawn near 1 MP,
  composited back through Blend 16. A prompt means Fill; no prompt means Remove; a strength below 1, or almost the
  whole picture selected, means Refine.
- **Past the canvas → outpaint** (Expand): grow the canvas first, pre-fill the new area from its edge, then the
  same Fill machinery on the new strip plus 32 px of overlap, with 256 px of the old picture as context.
- The router (6) makes this decision from the selection, the crop frame and the prompt, so the person only picks an
  action when they want something else than the default.



## 6. The router

`fugue::canvas::routeAction(input, tuning)` in [`src/engine/canvas_router.h`](../src/engine/canvas_router.h)
(pure C++, no Qt; tests in [`tests/engine/test_canvas_router.cpp`](../tests/engine/test_canvas_router.cpp)). The
task bar calls it with what the person did; it returns the action, the region and the numbers the action's
workflow needs, and its reasons in words (for a tooltip or the error line).

**Inputs**: the canvas size; the selection's bounds in canvas pixels (absent: no selection; it may reach past the
edge) and the share of the bounds that is selected (1 for a rectangle); the crop frame (the canvas bounds asked for,
absent when the size is kept); whether there is a prompt; a strength the person set below 1. Whether the selection
touches or passes the edge is worked out from the bounds, so it can never disagree with them.

**Rules, first match wins:**

| # | When | Action | Why |
| --- | --- | --- | --- |
| 1 | no canvas | none | |
| 2 | a crop frame reaching past the canvas on any side | **Expand** to the frame | Photoshop's Generative Expand gesture |
| 2b | a crop frame wholly inside the canvas | none ("that is a crop") | cropping needs no generation |
| 3 | no selection, a prompt | **Refine** the whole canvas, strength 0.5 | an instruction for the whole picture; a full redraw would replace it |
| 3b | no selection, no prompt | none ("Select an area first") | |
| 4 | a selection reaching past the edge | **Expand** to the union of canvas and selection; the whole new strip is filled | a strip left half empty would stay transparent |
| 5 | fewer than 64 selected pixels | none (`mask_empty`) | |
| 6 | a strength below 1 | **Refine** at that strength | the person asked to keep what is there |
| 7 | 90% of the canvas or more selected, a prompt | **Refine**, 0.5 | a full redraw of everything is text-to-image, not an edit |
| 7b | the same, no prompt | none ("type what should be there") | nothing is left to continue |
| 8 | no prompt | **Remove** | an empty prompt means "take it away" (4.5: the workflow brings its own instruction) |
| 9 | otherwise | **Generative fill** | |

**Numbers** (`RouteTuning`, each from the benchmark):

| Output | Rule | Evidence |
| --- | --- | --- |
| `padding` | half the selection's longer side, held to 64..256 px; Remove at least 128 | Fill best at 192 to 384 for a 476 px chair (4.2); the 64 px floor keeps padding above Blend + Feather; Remove needs surroundings (the plant at the edge) |
| `region` | the bounds grown by the padding, moved inside the canvas with its size kept (all of a side when longer) | Canvas Region and Crop to Mask move their box the same way; nothing beyond the edge was needed (edge-plant) |
| `scale` | up to 1 MP when the region is below 0.8 MP (InvokeAI's band), at most 4, never down | scale 2 hid the small rectangle's box and sharpened it (4.2); the Region's cap does the shrinking |
| `strength` | 1 for Fill, Remove, Expand; 0.5 for Refine unless set | 4.7: 0.5 refines, 0.75 restyles, below 0.5 does nothing at 4 steps |
| `edgeContact` | the bounds touch the canvas's edge | the region is moved inside; a reason says so |
| Expand `fill` | each grown strip plus 32 px of overlap into the old picture, their bounds | overlap 32 is between 0 and 64 on both cases (4.6) |
| Expand `region`, `padding` | the fill plus 256 px of the old picture beside it, cut at the canvas | 64 px of context duplicated the subject, 256 and 512 never did (4.6) |
| Expand `passes` | a side grown by more than half the old side is split, each pass continuing the last | [U]: Fooocus users' advice (one side, about a megapixel at a time); not measured here |

Two rules of the actions follow from the benchmark rather than from the router: the Remove workflow's hidden
prompt is an instruction (4.5), and the Expand workflow uses its own generic instruction when the person gives no
prompt (4.6). An empty prompt sent as is to Klein changes nothing.

## 7. Gaps in Fugue (proposed issues, not filed)

| # | Gap | Found by | Proposal |
| --- | --- | --- | --- |
| G1 | **Canvas resize revision** for Expand: the store cannot grow a canvas | the design (#231) and 4.6, which had to make the grown canvas by hand | A store operation and CLI (`fugue canvas resize <id> --to x,y,w,h --fill edge-blur\|transparent`), layers moved and re-tiled, with the new area of a new bottom layer pre-filled by stretching the edge pixels and blurring them (24 px), which beat gray and transparent |
| G2 | **Load Models cannot load FLUX.1 Kontext, FLUX.1 Fill or SD/SDXL inpaint models**: one text-encoder input, T5 refused for an image model, no CLIP-L input | section 3 (`fugue validate`) | A second encoder input (CLIP-L) and T5 for FLUX.1, so the inpaint-trained models the helper already runs become reachable; fix the message "it is a T5 encoder, and Load Models takes a language model or a T5 encoder"; until then masking.md's model list overstates (fixed in this change) |
| G3 | **Canvas Region does not bring a small region up to the model's size**: `scale` is a fixed multiple, the cap only shrinks | 4.2 (a 10% selection redrawn at 432 x 464) | A `target_megapixels` input (Crop to Mask's rule: up and down to about N MP); the router's `scale` stands in meanwhile |
| G4 | **No outpaint pre-fill but gray**: Pad fills gray, Fill Masked's blur across a one-sided hole pulls the gray in | 4.6 | Pad `fill` modes `gray`, `edge`, `edge-blur` (default), `transparent`; or an "Extend edges" node; the resize revision (G1) uses the same code |
| G5 | **No removal model** (LaMa) | 4.5: Remove relies on an instruction to an edit model; the large edge removal is soft | A LaMa pack (Apache-2.0, ONNX, like the BiRefNet pack) as Remove's pre-fill, then the same Klein pass; MAT is research-only |
| G6 | **Canvas Write's blend stops at the Region's box**: padding below Feather + Blend gives a hard edge | 4.2 (rectangle with padding 0: seam 5.4) | Canvas Region holds `padding >= feather + 16` with a warning, or Write grows its box; the router's 64 px floor avoids it for actions |
| G7 | **Strength moves in steps at 4 steps, and 8 steps make it coarser, not finer** | 4.7 (0.5 at 8 steps changes less than 0.5 at 4) | Look at how the start step is chosen for FLUX.2's schedule in img2img [U whether this is a bug]; meanwhile Refine offers two strengths (light 0.5, restyle 0.75) rather than a slider |
| G8 | **An action needs a fallback prompt**: the `action` block's `optional` prompt sends an empty prompt, which Klein reads as "change nothing" | 4.5, 4.6 | `"fallback": "<text>"` on the prompt role: used when the person leaves the prompt empty (Remove: always; Expand: when empty) |
| G9 | **Background replacement needs a subject mask** | 4.4 (a rim of the old window round the cat) | The planned Select subject (BiRefNet) as a selection, inverted and grown 8 px, for a Replace background action |
| G10 | **Templates disagree with the findings**: `generative-fill` and `inpaint-region` keep scale 1 and Klein 9B; `outpaint` uses gray, 64 px of feathering and no reference | 4.2, 4.6 | Move them to the recommended workflows of section 5 (fugue-workflows; a branch, not pushed from here) |
| G11 | **Enhance Prompt writes a whole scene**, wrong for a part | 4.7 (simulated) | Off for canvas actions; if offered, a prompt template for "the selected part" |
| G12 | **Klein 9B's peak is the whole card**: up to 23.9 GB used with the desktop's 4.7 GB on a 24 GB card | timings in 4.8 | Default to 4B (10 to 14 GB peak); keep 9B opt-in with its memory shown |
| G13 | **Klein 9B can hand back Expand's pre-fill unchanged** with a generic instruction (the portrait, both seeds) | 4.9 | Default to 4B; for 9B, try a reference without the pre-fill (the old picture alone) [U] |

## 8. Not verified

- Photoshop's behaviour (Adobe's help returned 403): every Photoshop claim is [S2] or [U].
- The real Enhance Prompt node (no text model was started); only a hand-written prompt in its style was tried.
- FLUX.1 Fill, FLUX.1 Kontext, SDXL inpaint, BrushNet, the Fooocus patch, MAT and LaMa: not runnable in Fugue's
  graph today, so not compared (G2, G5).
- Expand on a real grown canvas (G1): the benchmark builds the grown picture outside Fugue; a transparent new area
  (what a plain resize gives) was tried and works, worse than an edge-blur fill.
- Expand passes for large growth (the router's `passes`) and two-sided growth beyond 1 MP.
- One run per seed: two seeds (42, 7) for the model and final comparisons, one for the one-factor variants, so a
  small difference between two variants (under about 0.1 of seam) is noise.
- Soft mask on a non-distilled model with more steps.
- The seam number does not see everything: Color Match scored best on the chair and looked wrong (4.4). The pictures
  were looked at for every case; the sheets are the evidence.


