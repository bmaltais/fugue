# Canvas actions: GPU verification (2026-11-23)

Every canvas action was run for real through `fugue canvas act ... --wait --json` on the local RTX 3090 (CUDA helper, FLUX.2 Klein 4B Q8 for the
generative ones, RealESRGAN_x4plus for Upscale), in a scratch `FUGUE_HOME` (`/tmp/claude-1000/scratch-verify`, models via `model_paths` =
`/mnt/windows/models`). The owner's own app was using the same GPU at times (14 GB held, bursts to 20 GB), so a first try sometimes failed with
`node_failed: generation failed` on `encode`/`vae_encode`; a plain retry passed. That is contention, not an action bug (the scratch root has its own GPU
lock, so it could not queue behind the app's).

Pictures: `templates/*/inputs/*` (reading-nook 1024, collage 1024 + collage-mask, cat-768, ramen 192). Outputs and exports are in
`/tmp/claude-1000/scratch-verify/out/` (`before-*.png`, `after-*.png`, `cmp-*.png` side by side); five are kept in `/tmp/claude-1000/keep/verify-*.png` (fill with a selection, expand, harmonize before and after the fix, upscale source / no model / model).

| Action | Command (after `canvas act <id>`) | Exit, time | What landed | Judgement |
|---|---|---|---|---|
| generative-fill, rect | `generative-fill --prompt "a tall potted green plant" --rect 520,240,200,330 --seed 7` | 0, 27 s (cold helper) | 1 layer `a tall potted green plant` (visible, layer mask), canvas 1024x1024, same canvas | Very good: the lamp is replaced by a plant with a matching shadow on the wall, no visible seam. 4 preview frames on stderr. |
| generative-fill `--count 2` | `... --prompt "a red cushion" --rect 600,730,200,110 --seed 3 --count 2` | 0, 19 s for both | layers `a red cushion (1 of 2)` visible and `(2 of 2)` hidden; seeds 3 and 4 | Correct: the hidden layer is not in the export; the two variants differ. The rect's own left edge shows as a straight edge in the cushion (my rectangle, no feather). |
| generative-fill `--selection mask.png` | `... --prompt "an oval framed painting of mountains" --selection mask-ellipse.png --seed 5` | 0, 21 s | 1 layer, named after the prompt | Very good: the painting follows the ellipse exactly, soft shadow, outside untouched. |
| remove | `remove --rect 530,240,170,340 --seed 2` | 0, 15 s | 1 layer `Remove` | Good: lamp gone, shelf and wall filled. A faint rectangular edge shows where the lamp's light pool is cut by the (tight) rectangle. |
| refine | `refine --rect 410,550,430,460 --prompt "worn brown leather armchair" --set strength=Restyle --seed 4` | 0, 9 s | 1 layer `Refine` | Works and keeps the shapes, but even at Restyle (denoise 0.9, verified in the run's workflow) the chair stays green velvet: Refine does not restyle with a 4-step Klein. Doubtful value of the Restyle word, not a defect of the plumbing. |
| expand | `expand --expand right --percent 25 --seed 1` | 0, 9 s | canvas 768x768 -> 960x768; layers `Expanded area` (bottom, from canvas-resize), `cat-768`, `Expand` (mask); `expanded: {width 960, height 768}` | Very good: pots and the shelf continue, no visible seam at the overlap (checked at 2x). |
| expand, cancelled | `expand --expand left --pixels 128` + SIGINT after 4 s | 1 (`cancelled`), 0.4 s after the signal | canvas 896x768 (grown, `Expanded area` layer, nothing written), head is one revision `action:expand` | Consistent, but the grown empty strip stays (see notes). |
| harmonize, selection | `harmonize --selection collage-mask.png --seed 1` | 0, 19 s | 1 layer `Harmonized` | **Bad at the old default (strength 0.9):** a frosted, blurred ring of 48 px around the selection edge, and the plant at the edge smeared. Fixed (below): at strength 1 the seam is redrawn and only a faint edge remains. |
| harmonize, no selection | `harmonize --seed 1` | 0, 19 s | 1 layer `Harmonized`, whole canvas | Subtle global grade; the circle's edge stays (nothing to seam). Fine. |
| upscale-no-model | `upscale-no-model` | 0, <1 s | new canvas `Upscaled` 384x384 (one layer `Layer 1`); source head unchanged; `canvases: [id]` | Correct 2x, soft (bicubic-like). |
| upscale (RealESRGAN_x4plus) | `upscale` | 0, ~1 s warm | new canvas `Upscaled 2` 384x384; source unchanged | Correct 2x, visibly sharper than no-model (noodles, wood grain). `--set scale=4x` gives 768x768 (checked on its size). |
| generative-fill count 4 + cancel | `--count 4` + SIGINT after 10 s | 1 (`cancelled`), "(1 of 4 run(s) finished.)" | layer `a clock (1 of 4)` kept; the other runs `cancelled` in `runs list`; the 4-count run before it gave `(1..4 of 4)` with 2..4 hidden | Consistent; no helper left from my root after the cancel. |
| remove-background-action | not run | | | needs the Remove Background pack (an install needs a person). `--layer` therefore untested. |

## Live preview frames

With the default `step_preview = fast`, stderr shows `sample preview frame` lines (4 for a 4-step fill, 8 at `steps=8`; Refine at 4 steps showed
1, Remove 4). Frame files: `<run folder>/nodes/sample/preview.png` (e.g.
`/tmp/claude-1000/scratch-verify/fugue/cache/fugue/runs/20261123-192402-137-tdi2/nodes/sample/preview.png`): a small crop (about 112x140) of the region, a
noisy early-step picture of the plant. Works.

## Bugs found

1. **Harmonize with a selection left a frosted ring** (template, fixed): the seam band is blurred in the reference and the Sampler ran at denoise 0.9
   (also 0.95), which does not redraw it. Strength 1 (4 steps) does, on two seeds; 8 steps at 1 is the same. Whole-picture Harmonize is unaffected.
   Fix: `templates/harmonize` and `templates/harmonize-image` default 1 (branch `harmonize-seam-strength` of the templates submodule), the two tests that
   pinned 0.9 now pin 1.0.

## Doubtful, not changed

- `--set model=<file name>` fails with `type_mismatch` ("takes a link or a fugue/upscale-model resource handle"). Only a full handle with `hash` works, and
  a file served by a provider gives `model_missing`. So `canvas act` cannot pick another upscale model by name (the bar can). A name lookup (hashing the
  file) would be a feature; the default model of the action ran.
- A cancelled Expand keeps the grown canvas (a separate revision, undoable like any), with a transparent strip. The design does not say what a cancel
  should do for Expand.
- `Upscale` names its first layer `Layer 1` in the new canvas; the source's layer name would read better.
- Refine "Restyle" barely changes anything at 4 steps.
- Harmonize's default strength 1 was judged on one picture and 3 runs; the whole-picture path was run once at 1 and once at the old default.

## Resolved afterwards (2026-11-23)

- `--set model=<file name>` now resolves a model by its file name (#309).
- A cancelled or failed Expand takes the growth back when the head is unchanged (#310, canvas-actions-design.md, Expand row).
- Upscale's new canvas names its layer after the canvas, `Upscaled` (#311).

## Not run

Remove Background (`--layer`), the pack is not installed. No window or task-bar run; this was only the CLI. A cancel was not tried while only waiting
in the queue.
