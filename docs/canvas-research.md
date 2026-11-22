# Canvas research (epic #206)

Research notes for the canvas document: prior art for the select-run-place loop, storage of immutable revisions, and how it fits the engine. Written 2026-11-22 by research agents from primary sources, measurements on this machine and a read of this repo; claims marked **[U]** or "unverified" could not be checked against a source and must be re-checked before they drive a decision. The decisions themselves are in issue #206.

1. [Prior art](#prior-art-canvas-with-a-generative-select-run-place-loop-fugue-epic-206)
2. [Revision storage](#canvas-revision-storage-fugue-epic-206)
3. [Engine integration](#fugue-epic-206-canvas-document-code-research)

# Prior art: canvas with a generative select-run-place loop (Fugue epic #206)

Date: 2026-11-22. Method: public docs, release notes, issues and licence files only; no GPL source read. `[U]` = not verified from a source I could open. Web fetches of Adobe help pages, Draw Things docs and some Krita doc pages failed (403/404), so those rows lean on search snippets and forum posts and are marked.

## Summary and recommendation

Every product runs the same loop: a region (rectangle or mask) is cut from a layered document, padded with context, resized to the model's native range, generated, and the result is composited back, usually as a new layer or a held "staged" result the user accepts. The differences that matter are (a) where the result waits before it becomes part of the document, (b) how the seam is blended, (c) how resolution is handled, and (d) how history works.

Fugue's immutable-revision design (#206) is a good fit. Two details worth taking from prior art:
- A staged result is a candidate that is not yet in the document. InvokeAI's staging area and Krita's preview/apply both do this, and both lock or defer document edits while it exists. Fugue gets this for free: a Canvas Write run produces a *candidate revision* off the parent; "accept" moves head to it, "reject" drops the ref. No lock is needed because revisions are immutable. This directly beats InvokeAI (canvas is locked while staged images exist) and removes the "lost on reset" class of bug.
- Photoshop's model (result = new non-destructive layer, three variations, regenerate) is what users expect. Fugue should make each result a layer, not a flattened paste.

Minimum Fugue v1 loop (see section below): rectangle selection plus mask layer; Canvas Region with padding, context rect and target-size scaling; Canvas Write that adds the result as a layer with a feathered blend mask; N candidates; accept/reject/compare; revision list with revert. Skip live mode, infinite canvas panning tools, tile storage and layer transforms for v1.

## Comparison table

| Feature | InvokeAI canvas (v5+ layers; Apache-2.0) | Krita AI Diffusion (GPL-3.0) | Photoshop Generative Fill | A1111 / Forge inpaint | ComfyUI mask editor / nodes | Draw Things |
|---|---|---|---|---|---|---|
| Document and layers | Raster, control, inpaint mask, regional guidance layers; raster adjustments (v6.7) | The Krita document itself (any layers); selection or transparency mask as the region | PSD layers; result is a Generative Layer | None (single img2img image) | Mask editor has Base Image / Paint / Mask layers per image; no document | Infinite canvas [U, secondary sources only] |
| Region selection | Generation bounding box (only visible content inside is sent) plus inpaint mask layers | Krita selection; auto context area; optional Selection Bounds / Entire Image / Custom Layer context | Marquee / lasso / object selection | Brush mask drawn on the one image | Mask pen, bucket, colour select | In-painting mask [U] |
| Context / padding | Bbox size is the context; `Shift+B` fits bbox to masks with padding for blur (v6.1.0) | Settings > Diffusion > Selection Padding; larger = more coherence, more cost | Not documented [U] | "Only masked padding" option [U: name known, text not fetched] | Manual crop nodes | [U] |
| Resolution scaling | "Scale Before Processing" (default on in old canvas) | Native-range aware; resolution multiplier, max pixel count; avoids resizing when possible | ~1024x1024 per generation, larger selections upscaled and soft (Adobe community, July 2025) | "Whole picture" resizes everything; "Only masked" resizes just the region and pastes back | Manual | [U] |
| Mask blur / blend | Seam Size/Blur/Strength/Steps (old canvas); compositing settings (v4.0.4) | Feather (% of selection, sets denoise transition zone) and Blend (px, composite mask only) | Automatic | Mask blur; Soft Inpainting (soft-edged masks in the denoiser) | Per-workflow (e.g. inpaint nodes: Color Match, Denoise to Compositing Mask) | [U] |
| Staging / accept | Staging area: cycle, commit or discard; canvas locked meanwhile; bypass via "Save New Generations to Gallery" (v6.1.0) | Preview on canvas, history of generations, then Apply as layer | Variations (3) in Properties panel, already on a layer | Output gallery; nothing to accept | Save in editor, then run | [U] |
| Result placement | Accepted staged image becomes a raster layer | New layer with blend mask | New Generative Layer | Pasted back into the image (Only masked) | Workflow decides | Into canvas [U] |
| History / undo | Undo a discard within a session; unsaved generations lost on canvas reset (old docs) | Browse previous generations and prompts (README) | Photoshop history plus layer stack | None beyond output folder | Undo inside editor only | "Version history" [U] |
| Live mode | None found | Live painting, selection-limited since v1.9.0; heavier pipeline skipped for live | None (cloud) | None | Not in core [U] | [U] |
| Outpaint | Bbox covering transparent area; infill Patchmatch/Tile (old docs) | "Expand" mode extends canvas, ignores blank area | Generative Expand | Poor man's outpainting script; openOutpaint extension (MIT claimed in a comment [U]) | Template workflows | [U] |
| Large canvas | Bbox-limited work area, canvas is unbounded | Works on the document; context area keeps cost bounded; upscale uses tiling | 1024 cap per generation | Only masked | Tile nodes (comfyui-tooling-nodes, GPL) | [U] |
| Code reusable by Fugue | Apache-2.0: yes (with notices) | No (GPL-3.0) | No | A1111 AGPL-3.0 [U, not fetched] | GPL nodes: no | Closed [U] |

## Details with source URLs

### InvokeAI (Apache-2.0: https://raw.githubusercontent.com/invoke-ai/InvokeAI/main/LICENSE)
- Bounding box: "Only the visible Canvas content inside the box is sent to the model"; its size is the output size, constrained to model-supported values; it "is not an inpaint mask". https://invoke.ai/features/canvas/bounding-box/
- Scale Before Processing: may process at a different resolution, then resize back to the bbox footprint. Same page.
- Staging area: generations for the bbox go to a staging area to cycle and commit or discard (search summary of release notes, https://invoke.ai/releases/version/v5-0-0/). v6.1.0 added a setting to send generations straight to the gallery, bypassing staging "so the Canvas stays editable"; v6.3.0 blocked creating a new canvas while images are staged. https://invoke.ai/releases/version/v6-0-2...latest/
- v6.x layer and tooling facts (raster adjustments, `Shift+B` fit bbox to masks with blur padding, masked-area extraction in v6.10.0, SAM2 select object): same release page.
- Old Unified Canvas (dated 2022-12-20): base layer, mask layer, staging area; patchmatch vs tile infill; seam size/blur/strength/steps with suggested 96 / 16 / 0.7; "any generations that are not saved will be lost when the Canvas resets". https://mauwii.github.io/InvokeAI/features/UNIFIED_CANVAS (mirror of the old docs).
- InvokeAI 5.0 merged generation and canvas UIs ("Control Canvas"). https://invoke.ai/releases/version/v5-0-0/ (via search; page body not read).

### Krita AI Diffusion (GPL-3.0: https://github.com/Acly/krita-ai-diffusion/blob/main/LICENSE; README states no licence)
- Selection guide: automatic context, adjustable with Selection Padding; modes Fill, Expand, Add Content, Remove Content, Replace Background; Seamless, Focus options; mask pipeline with core, transition zone, blend mask; Selection Feather (% of selection) and Selection Blend (px). https://interstice-docs.pages.dev/selections
- Resolution guide: tries to place the context area so no resize is needed; resolution multiplier (0.5 on 2000x2000 gives 1000x1000), maximum pixel count (1 MP: 2000x1000 becomes 1416x712), dimensions multiple of 8 (some models 16/64). https://interstice-docs.pages.dev/resolutions (the docs.interstice.cloud/resolutions page returned the same content)
- Features: "Preview results and browse previous generations and prompts at any time"; "Let AI interpret your canvas in real time"; uses ComfyUI as backend. https://raw.githubusercontent.com/Acly/krita-ai-diffusion/main/README.md
- Live mode on selections since v1.9.0 for performance on large canvases; the full inpaint pipeline is too heavy for live. (search summary of release notes: https://github.com/Acly/krita-ai-diffusion/releases)
- Custom workflows (v1.26.0): a Graph workspace imports ComfyUI API JSON, can exchange canvas, image/mask layers, selections and style settings, expose parameters, queue or run live. https://newreleases.io/project/github/Acly/krita-ai-diffusion/release/v1.26.0
- Architecture lesson from the maintainer: built-in workflows are "more like templates", with most nodes generated dynamically from checkpoint, LoRAs and control layers; replacing them with a user graph is hard because generate/inpaint/refine differ; extension points are "a lot of work to implement and maintain". https://github.com/Acly/krita-ai-diffusion/discussions/385
- Backend nodes: `comfyui-tooling-nodes` (GPL-3.0): base64 image/mask load, websocket image send, image cache, region attention masks, tile layout/extract/merge nodes. https://github.com/Acly/comfyui-tooling-nodes. `comfyui-inpaint-nodes` (GPL-3.0): mask expand/shrink/stabilize, fill modes, Color Match (Masked), Denoise to Compositing Mask. https://github.com/Acly/comfyui-inpaint-nodes
- A node-graph host's lesson: the canvas tool owns geometry (context rectangle, scaling, compositing) and the graph owns the model call. Fugue can invert this nicely: put Region (crop, pad, scale) and Write (unscale, feather, composite) as nodes, and let user graphs sit between them.

### Photoshop Generative Fill (docs not fetchable; 403)
- After Generate, "choose from the Variations that appear in the Properties panel. The generated content will appear in a new layer"; edits are non-destructive on a Generative Layer; variations only show when a Generative Layer is selected. Via search summary of https://helpx.adobe.com/ph_en/photoshop/desktop/create-open-import-images/create-images/edit-images-with-generative-fill.html and https://community.adobe.com/t5/photoshop-ecosystem-discussions/photoshop-no-longer-creating-three-options-when-using-generative-fill-or-generative-expand/m-p/14367053
- Generative restyle: a slider to refine an existing result rather than regenerate. https://community.adobe.com/announcements-710/fine-tune-your-ai-edits-with-generative-restyle-1653465
- Size cap: an Adobe community representative (July 2025) said desktop max is still 1024x1024 per generation, citing a community expert; a 2000 px claim exists and conflicts. https://community.adobe.com/questions-712/what-is-the-max-resolution-for-generative-fill-in-photoshop-in-2025-1181021 (user/rep forum posts, not official docs).

### A1111 / Forge
- Inpaint area "Whole picture" vs "Only masked" (resizes only the masked region, inpaints, pastes back, helps large images); masked content modes; soft inpainting; outpainting through the "Poor man's outpainting" script. https://github.com/AUTOMATIC1111/stable-diffusion-webui/wiki/Features
- Forge inpaint area docs and openOutpaint licence not verified. [U] openOutpaint is "MIT licensed" only per a user comment in https://github.com/Mikubill/sd-webui-controlnet/discussions/1464; read https://github.com/zero01101/openOutpaint-webUI-extension/blob/main/LICENSE before relying on it.

### ComfyUI
- Mask editor tools: Mask Pen, Paint Pen, Eraser, Paint Bucket, Color Select; layers Mask / Paint / Base Image; mask opacity and blend modes. Docs do not state file format or undo depth. https://docs.comfy.org/interface/maskeditor
- No document concept; it is image-in, image-out. That gap is what #206 fills.

### Photopea, Affinity, Draw Things
- Photopea and Affinity: no official docs found for generative fill. A 2025 Reddit thread says Affinity has no generative fill equivalent and suggests running Stable Diffusion locally (low trust). https://axebps-redlib.hf.space/r/Affinity/comments/1l6v4jk/alternative_to_generative_fill [U] for anything further.
- Draw Things: official docs could not be found; infinite canvas, in-painting mask and version history appear only in third-party and App Store summaries. https://apps.apple.com/us/app/draw-things-offline-ai-art/id6444050820 [U]. Closed source [U].

### OpenRaster (for revision metadata)
- `stack.xml` is the layer stack; `mergedimage.png` is mandatory; `composite-op` has 19 `svg:` values, default `svg:src-over`; the spec says extensions are permitted but must be specified and "work safely with the baseline profile"; unknown-element behaviour is not defined on the pages fetched. https://www.openraster.org/baseline/file-layout-spec.html and https://www.openraster.org/baseline/layer-stack-spec.html
- Consequence: keep revision metadata out of `stack.xml` where possible (a sidecar manifest or a namespaced file inside the zip) and test that Krita, GIMP and MyPaint still open the file. [U] whether they preserve or drop unknown zip entries; must be tested, not assumed.

### Revision storage (not researched from primary sources)
- Content-addressed layer blobs with a manifest is the design #206 already chooses; I did not fetch git/restic/ostree docs. [U] on tile size and chunking costs. Nothing in competitor products does this: they keep only a session history or none, so this is Fugue's own design.

## What to match
1. Result lands as its own layer (Photoshop, InvokeAI, Krita).
2. Candidates are held for review before they count (InvokeAI staging, Krita preview): N variations, cycle, accept or discard.
3. Context padding and a resolution-aware target size with a cap and multiplier (Krita's multiplier and max-pixel cap are the clearest model).
4. Feather for the denoise region and a separate blend mask for compositing (Krita's split is the best-documented one).
5. Fit-region-to-mask shortcut with blur padding (InvokeAI `Shift+B`).
6. Keep previous generations browsable with their prompt (Krita history).

## What to beat
- Staging area that locks editing (InvokeAI): revisions make candidates independent; the canvas stays editable and a Write on an old parent offers rebase or keep both.
- Results that vanish on reset or crash (InvokeAI old docs): a candidate is a stored revision.
- Fixed per-generation caps with silent soft upscaling (Photoshop 1024): show the actual scale factor and let the user cap or pick the target size.
- No reproducibility anywhere: record revision hash, region, seed and graph hash per layer, so a re-run is a cache hit and "regenerate this layer" is exact.
- Real compare: A/B or swipe between any two revisions (competitors only cycle candidates).
- Geometry hidden in a plugin (Krita): Region and Write are visible nodes users can swap.

## What to avoid
- Rebuilding Krita's fixed workflow templates with dynamically generated graphs, which the Krita maintainer calls costly to maintain (discussion #385). Ship a small set of template graphs instead.
- Live mode in v1: Krita had to downgrade the pipeline for it and its live path differs (no grow factor), causing small-selection quirks (#662).
- Shipping a mutable canvas that nodes edit in place (breaks cache).
- A separate pixel-editing toolset (scope guard in the epic).
- Copying GPL code or node structure from Krita's plugin or its ComfyUI nodes.

## Minimum loop for Fugue v1
1. Canvas as a project document: `.ora` per revision (or manifest + blobs), `fugue/canvas` handle, revision list panel with revert.
2. Selection: rectangle and a `role: mask` layer (the same type #178/Remove Background emit). No lasso in v1.
3. Canvas Region: input = canvas handle + revision (head resolved once per run) + region + padding + target size (multiplier and max pixel cap as in Krita). Output = image, mask, and a transform record needed to place it back.
4. Canvas Write: input = image, mask, transform; applies feather and blend mask; writes a new layer and a candidate revision. Not head until accepted.
5. Generative fill template: Region, existing inpaint graph (#167), Write; N candidates via seeds.
6. UI: candidates strip, Accept / Reject, compare with parent, undo as "revert to previous revision". Re-running the same revision and graph must be a cache hit.
Defer: live link, lasso, transforms, tile storage, infinite pan, outpaint-specific UI (outpaint is just a region that extends beyond the current bounds, so allow Region to extend over transparent pixels and Write to grow the canvas).

## Licence of anything reusable
| Item | Licence | Reuse |
|---|---|---|
| InvokeAI | Apache-2.0 (verified) | Code and ideas reusable with notices; its canvas is a TypeScript/React frontend, so only designs and algorithms (e.g. seam blending) port to C++ |
| Krita AI Diffusion | GPL-3.0 (LICENSE file verified) | Do not copy; behaviours from docs only |
| comfyui-tooling-nodes, comfyui-inpaint-nodes | GPL-3.0 (verified) | Do not copy |
| A1111 | [U] (AGPL-3.0 from memory) | Do not copy |
| openOutpaint extension | [U] MIT per a user comment | Verify before use |
| OpenRaster spec | Open format; spec licence not checked [U] | Fine to implement |
| Photoshop, Draw Things, Photopea | Proprietary [U] | UX reference only |

## Complaints (with sources)
- Krita Live mode: sharper seams and colour shifts versus Refine; maintainer says the grow factor is not applied in live mode so small selections get enlarged context (select at least 512 px SD1.5, 1024 px SDXL); colour and brightness shifts are a model limitation; halos from Differential Diffusion blending; closed as completed 2024-06-10. https://github.com/Acly/krita-ai-diffusion/issues/662
- Krita: live-mode selection bug fixed in a release (#539, via release-note search summary). https://github.com/Acly/krita-ai-diffusion/releases
- InvokeAI: canvas locked while staged images exist (fixed by the bypass setting in v6.1.0); staging area slowdowns with many images (virtualised in v6.0.1); auto-switch failures with many staged images; stale bbox after a cancelled run placed images in the wrong spot (PR #6184); canvas images in wrong places listed as known issue in v4.0.4. https://invoke.ai/releases/version/v6-0-2...latest/ , https://github.com/invoke-ai/InvokeAI/pull/6184 (via search summaries).
- Photoshop: soft results because 1024 px is stretched over large selections; variations panel disappearing when no Generative Layer is selected; "quality unusable since the beta". https://community.adobe.com/questions-712/what-is-the-max-resolution-for-generative-fill-in-photoshop-in-2025-1181021 , https://community.adobe.com/questions-712/generative-fill-quality-has-become-unusable-since-the-beta-1161701
- A1111: "Only masked" may fail to remove an object that "Whole image" removes (a user comment, low trust). https://github.com/Mikubill/sd-webui-controlnet/discussions/1464

## Open questions
1. Region padding: percentage of the region or pixels? Krita exposes a padding setting, InvokeAI uses the bbox itself. Recommend: context rect as an explicit Region parameter, default auto.
2. How are variations modelled: N candidate revisions off one parent, or one revision with N layers? Recommend N candidates, one accepted.
3. `.ora` metadata: sidecar manifest in the zip vs separate store; test with Krita/GIMP/MyPaint. [U] how each preserves unknown entries.
4. Tile storage and large canvases: not researched from primary sources; do a spike with a 16k x 16k canvas before committing.
5. Draw Things and Photopea behaviour needs hands-on testing; no primary docs were fetchable.
6. Photoshop's current official resolution limit: only forum evidence found; check Adobe's Generative AI FAQ.
7. What a Write on a stale parent does by default (rebase vs keep both).

---

# Canvas revision storage (Fugue epic #206)

Researched 2026-11-22. Read-only on ~/github/fugue (spec.md 10, 14.4, Appendix A rows, `src/engine/paint_file.*`, `paint_document.h`, issues #206, #204). Measurements were run locally with Python 3 + Pillow 13.0.0 + numpy in the scratchpad (`m/bench.py`, `m/gitbench.py`), git installed; restic, borg, casync and ostree are NOT installed here. Claims I could not verify are marked [U].

## 1. Summary and recommendation

**Use a content-addressed store with 256 px tile blobs and a JSON manifest per revision (a Merkle tree), not one `.ora` per revision.** The `.ora` stays what it is today: the interchange/export/share format, produced from a revision on demand.

- Layer **tiles** (256x256, the same size as `canvas/cell_grid`'s texture cells) are PNG blobs addressed by SHA-256 of their raw pixels; all-zero tiles are not stored. A **revision manifest** lists layers (id, name, kind, opacity, visibility, size) and, per layer, a sparse map `tile -> blob hash`.
- Two hashes, git-style: a **tree hash** = SHA-256 of the canonical manifest (pixels and layer properties only, no parent, no time, no producer) and a **commit record** (parent tree hash(es), producer, time) stored beside it. **Only the tree hash goes into cache keys.** Two revisions with the same pixels then share one identity, and downstream nodes still hit.
- Cost, measured (4096x4096, 12 layers, a photo-like opaque base of 39 MB PNG, 9 patch layers, 2 masks): first revision about 62 MB; a 512 px edit adds **1.06 MB** (9 tile blobs) and **0.2 s**, versus 3.3 MB (a patch layer re-stored whole) or ~39 MB (the base layer re-stored whole) or 65 MB (a full `.ora`). 10 revisions: ~72 MB tiles vs ~95 MB layer blobs vs 693 MB for 10 `.ora` files.
- Head = a tiny ref file per canvas, replaced by atomic rename; writers append revisions without a lock, and moving head is a compare-and-swap on the parent.
- Where: **inside the project root** (#204), `canvases/<canvas id>/{objects,revisions,refs,runs}`. An `.ora` export goes to the inputs folder only when a workflow node needs a file or a share package is built.

## 2. Measurements

Machine: 12 cores, Pillow 13 (libpng, `compress_level` 1 and 6), SHA-256 in Python. Content is synthetic: summed upsampled noise octaves plus grain, which gave a PNG of 58% of the raw RGBA size at level 1 (real photos are typically 50 to 65% [U, from experience, not measured]); a flat or AI-generated picture compresses better. Patch layers are a 1024x1024 photo-like square on a transparent full-size layer (Fugue layers are always full canvas size: `paint_file.cpp` refuses an offset layer). Masks are one gray disc.

### 2.1 PNG layer cost

| Canvas | Layers | Raw RGBA per layer | Base PNG (lvl 1) | Patch layer PNG | Mask PNG | Sum, level 1 (encode) | Sum, level 6 (encode) |
|---|---|---|---|---|---|---|---|
| 4096x4096 | 12 | 67 MB | 39.0 MB, 1.47 s | 2.87 MB, 0.26 s | 0.08 MB, 0.04 s | 65.0 MB (3.9 s) | 54.8 MB (10.8 s) |
| 8192x8192 | 5 | 268 MB | 155.7 MB, 5.75 s | 3.77 MB, 0.71 s | 0.30 MB, 0.15 s | 163.9 MB (7.5 s) | 142.2 MB (29.0 s) |

Level 6 saves ~16% size for ~2.8x to 3.9x time: use level 1 (or 2) for revisions, level 6 only for an export. Decode of the 4096 base PNG: 0.55 s; the 8192 base: 2.1 s. SHA-256 over the raw pixels of all layers: 0.51 s (4096 x 12) and 0.63 s (8192 x 5), so hashing is free next to encoding. Encoding is the cost to avoid, and per-tile dedup avoids it for unchanged tiles.

Fugue's own limits matter here: `paintFileMaxLayerBytes` is 1 GiB of decoded pixels, so an 8192x8192 canvas (268 MB per RGBA layer) holds **at most 3 RGBA layers** in an `.ora` Fugue will read (mask layers are 1 channel, 67 MB), and 4096x4096 holds 16. "10 to 20 layers at 8192" is not reachable today; the revision store is the place to lift that (tiles never need the whole layer in RAM), but the `.ora` export would still hit the cap.

### 2.2 Dedup after one edit (a 512x512 region, unaligned, filled with random color = incompressible worst case)

| Scheme | Rev 1 | Added by rev 2 (4096 case) | Added by rev 2 (8192 case) |
|---|---|---|---|
| One full `.ora` per revision | 65 MB | 65.4 MB | 164 MB |
| Per-layer PNG blobs + manifest | 65 MB | 3.28 MB (edit in a patch layer; **~39 MB if the edit were in the base layer**, inferred) | 4.18 MB (patch; ~156 MB if base) |
| 256 px tile blobs + manifest | 61.7 MB in 491 blobs (2578 of 3072 tile slots all-zero, skipped) | **1.06 MB in 9 blobs**, 0.2 s to hash and encode | **1.06 MB in 9 blobs**, 0.4 s (rev 1: 161 MB, 1084 blobs, mean 149 KB per blob) |

Tile cost is independent of layer size and layer count: it is about the touched area. An unaligned 512 px edit touches 3x3 = 9 tiles; an aligned one 4. Tile blobs average 126 to 149 KB, so file-count overhead is small (a 4096 base is 256 tiles, an 8192 base 1024).

### 2.3 git as the object store (10 revisions, 4096x4096, 12 layers, `.ora` as one stored zip, each revision edits 512 px in one layer)

| | Size |
|---|---|
| Sum of the 10 `.ora` files | 692.6 MB |
| git loose objects (zlib does nothing to PNG) | 702.0 MB |
| after `git gc --aggressive` (delta against the previous revision) | **85.5 MB** |

So git's delta compression works on a **stored** zip of PNGs (identical PNG byte runs match), landing near the tile result (~72 MB) but only after a gc, the packing is slow [U: I did not time it; the gc of 700 MB took on the order of a minute], and every read of one layer means reading the whole `.ora` blob. Without a gc the cost is the full 70 MB per revision. Not recommended as the primary store, but it shows a plain "keep `.ora` files and let a packer dedup" fallback would be acceptable at this size.

### 2.4 Not measured [U]

restic/borg/casync (content-defined chunking): not installed; no network install attempted. Expected from their design: CDC re-aligns after an insertion, which stored-PNG zips need, but a PNG's deflate stream after the first changed row differs for the rest of the layer, so CDC gains little over fixed tiles for a *whole-layer* PNG and nothing over tiles for raw pixel data. ostree (file-level hashing of a checkout, hardlinks, static deltas): its unit is the file, so it equals "per-layer blobs". Both are heavy dependencies and neither adds a thing a tile map lacks.

## 3. Options compared

| Option | Edit cost (4096, 512 px) | Transfer to a peer | Read one layer/region | Complexity | Verdict |
|---|---|---|---|---|---|
| A. One `.ora` per revision | 65 MB | whole file (~65 MB) | read zip, decode layer | lowest; what exists | Reject as the store: 10 revs = 0.7 GB; keep as export |
| B. git repo of `.ora` (+gc) | ~2 MB after gc, 65 MB before | git protocol, deltas | read whole blob | needs gc scheduling, libgit2 or the git binary | Reject: slow pack, whole-blob reads |
| C. Per-layer PNG blobs by SHA-256 + manifest | 3 MB patch / 39 MB base | only missing layers | one PNG decode | low | Good v0 if tiles are cut; base-layer edits are costly |
| D. **256 px tile PNG blobs + manifest** | ~1 MB, any layer | only missing tiles (by hash) | decode only tiles in the region | medium (tile grid, sparse map) | **Recommended** |
| E. CDC chunking (restic/borg/casync) | similar to D on raw pixels, worse on PNG | chunk-level | reassemble | high, dependency | Reject |
| F. ostree | = C | = C | = C | high | Reject |
| G. Raw (uncompressed) tiles | same; 4x disk | 4x network | fastest | low | Reject on disk; fine as an in-RAM form |

Why tiles also help the rest of the epic:
- **Canvas Region** reads a padded rectangle: with tiles it decodes only the intersecting tiles (a 1024 px region at 256 px tiles = 16 to 25 tiles, ~2 MB of PNG) instead of the 39 MB base PNG (decode 0.55 s at 4096, 2.1 s at 8192).
- **The paint window** already works in 64 px undo tiles (`MaskPaint::tileSize`) and 256 px texture cells (`canvas/cell_grid`); saving a revision on stroke end is "hash the dirty 256 px cells", which the window already tracks [U: dirty-cell reuse not checked in code].
- **Mesh (#203)**: a revision is sent as its manifest (a few KB: 12 layers x up to 1024 tile entries of 64 hex chars is ~0.2 to 0.8 MB worst case for a full 8192 canvas; typical tens of KB) plus the tile blobs the receiver lacks, requested by hash. Every blob is self-verifying (hash = address), so peers need no trust in the transport, and an interrupted transfer resumes by simply asking again for the missing hashes. A 512 px edit is ~1 MB on the wire instead of 65 MB.
- **Share package**: unchanged (the `.ora` is built from a revision on export).

Cost of tiles: more code (tile grid, sparse map, region read/compose); a manifest format to version; many small files (use a pack file per N blobs later if file counts bite: 1100 blobs per revision at 8192 is not a problem on ext4/btrfs).

## 4. Details with sources

### 4.1 What exists in the code (read)

- `paint_file.cpp`: writes `mimetype` (first, stored), `stack.xml`, `data/layer-N.png`; fixed 1980-01-01 timestamps, uid/gid 0, stored entries, no thumbnail, no merged image: same layers give same bytes (so the `.ora` is a stable function of a revision). Root `image` has `version="0.0.3"`, `w`, `h`; layers carry `fugue:kind` in namespace `urn:fugue:ora` (paint, mask, picture). A picture layer is a 1-pixel transparent placeholder (the picture is the node's input).
- Reading is strict, for hostile files: it **refuses** any zip entry not `mimetype`, `stack.xml`, `mergedimage.png`, `Thumbnails/thumbnail.png` or under `data/`; refuses any element inside `<stack>` other than `<layer>`; refuses a nested `<stack>` (groups), offsets, unknown `fugue:kind`; at most 200 entries, 64 layers, 8192x8192 pixels, 16384 per side, 1 GiB per file. **Consequence:** unknown root attributes are tolerated (only `w`, `h` are read) but a new zip entry or element is a `media_unreadable` today, so any metadata entry in the `.ora` needs a reader change plus a Fugue version that can read it; older Fugue builds would refuse such a file.
- `paint_document.h`: layers have an id (never reused), name, kind, visibility, opacity; one undo history; a mask layer is 1 channel, others RGBA.
- spec 10.1: a node key is SHA-256 of canonical JSON of type/version, inputs (literal or upstream output content hash), resource identities, etc.; "a file a node reads by path is keyed by its content hash, not its path". 14.4/line 867: a `paint` input holds the path of an `.ora` in the inputs folder, **its bytes key the cache**, the package carries it (`inputs/`, at most 256 MB per file). 10.2: the result cache already stores blobs once by content hash. All hashes are `sha256:<hex>` (spec line 155).

### 4.2 `.ora` and revision metadata (question 2)

Sources: OpenRaster file layout spec (https://www.openraster.org/baseline/file-layout-spec.html) and layer stack spec (https://www.openraster.org/baseline/layer-stack-spec.html), fetched 2026-11-22 (through a summarizing fetch, so not word for word).

- Layout rules found: `mimetype` first, stored, exactly `image/openraster`; `stack.xml` is UTF-8 XML; `mergedimage.png` is mandatory since 0.0.2 and `Thumbnails/thumbnail.png` (at most 256x256, not referenced from XML) is required "to allow file browser software to render the thumbnail"; "all files inside `data/` should be referenced from somewhere". Nothing was said about files outside `data/`.
- Stack rules found: the root `image` carries a mandatory SemVer `version`; layer attributes are `src`, `name`, `x`, `y`, `opacity`, `visibility`, `composite-op`. The layer-stack page says the format "permits extensions, which should be individually specified in detail" and links to an extensions section; the spec text I could read **does not define how a reader treats unknown elements, attributes or namespaces**, and the extensions page URL I tried returned 404. So unknown-content handling is unspecified by the spec: it is up to each reader.
- Our file already breaks no rule in practice: `fugue:kind` is a namespaced attribute. Note the spec asks for `mergedimage.png` and a thumbnail, which Fugue's writer omits deliberately (determinism, no picture); other editors may therefore show an empty preview and, for strict readers, may reject the file [U].
- What Krita, GIMP, MyPaint do with unknown attributes, elements and zip entries: **[U]**, not tested (none installed or run here). From memory: Krita's `.ora` writer adds its own non-spec attributes on layers and ignores what it does not know; GIMP's `file-openraster` plugin reads the known layer attributes and skips the rest; MyPaint is the reference implementation and uses extra attributes of its own [all U]. A test with each editor, on a copy, is in the test plan.
- **Recommendation:** make the revision store the source of truth and the `.ora` a derived view. Put at most namespaced attributes on the root `image`: `fugue:canvas="<id>"`, `fugue:revision="sha256:<tree hash>"` (the tree hash does not depend on the `.ora` bytes, so there is no circularity). Do **not** add new zip entries or elements (it would break `knownName`/`parseStack` in current Fugue builds and risks other readers). On import of an `.ora` without a canvas, the importer creates revision 1 from its layers. A manifest, if wanted inside the file, could ride in one extra entry only after the reader allows it [U: needs a decision].
- If a person edits the exported `.ora` in Krita and re-imports it, the result is a **new revision with parent = the exported one**: the `fugue:revision` attribute (if Krita preserves it, [U]) gives the parent; otherwise the importer asks which canvas.

### 4.3 Manifest format sketch

`canvases/<canvas id>/revisions/<tree hash>.json` (canonical JSON: sorted keys, no whitespace, integers only, as for cache keys):

```json
{
  "format": "fugue-canvas-revision/1",
  "width": 4096, "height": 4096, "tile": 256,
  "layers": [
    {"id": 1, "name": "Background", "kind": "picture", "opacity": 1, "visible": true,
     "source": "sha256:<hash of the input picture's file>"},
    {"id": 2, "name": "Fill 1", "kind": "paint", "opacity": 1, "visible": true,
     "channels": 4,
     "tiles": {"4,2": "sha256:ab12...", "4,3": "sha256:cd34..."}},
    {"id": 3, "name": "Mask", "kind": "mask", "opacity": 1, "visible": true,
     "channels": 1, "tiles": {"4,4": "sha256:ef56..."}}
  ]
}
```

- `tree hash = sha256(canonical manifest)`. A layer with no `tiles` entry for a position is all zero. Layer order is the array order (bottom first, as in `paintToOra`). Layer ids stay stable across revisions (as `PaintDocument` never reuses them), so a diff between two revisions is a per-layer, per-tile comparison.
- A tile blob is `objects/ab/cdef...png` holding the tile PNG (RGBA or gray); its address is the SHA-256 of the **raw pixels** prefixed by `"fugue-tile\0" + channels + w + h` (so it is stable across libpng/zlib versions and pixel-identical edits dedupe), and the integrity check on read decodes the tile and re-hashes it (cheap at 256 px; ~0.5 ms to 2 ms per tile [U: not timed per tile; whole-layer decode was 0.55 s for 256 tiles, ~2 ms each]).
- A picture layer is recorded by the hash of the picture file (it lives in the inputs folder or the result cache, as today), not by tiles, matching the placeholder design.
- Commit record `revisions/<tree hash>.meta.json`, mutable-free, not part of the hash:

```json
{ "tree": "sha256:<tree>", "parents": ["sha256:<tree of parent>"],
  "created": "2026-11-22T10:15:00Z", "producer": {"kind": "window"|"workflow"|"import",
  "workflow": "<path>", "run": "<run id>", "node": "<path id>", "key": "sha256:<cache key>"},
  "label": "Fill sky", "tags": ["v1"] }
```

Because two commits can point at one tree, meta files are keyed by a commit id (`sha256` of the meta JSON), and `refs/` and tags point at commit ids; the cache and Canvas Region use the tree. That is exactly git's tree vs commit split. Simplification if the owner prefers one id: hash the manifest including `parents` and accept that an identical picture with another history misses the cache [decision for the owner; I recommend the split].

### 4.4 Cache keys, head resolution, staged re-runs (question 4)

- A `fugue/canvas` handle value is a descriptor `{canvas: <id>, revision: "sha256:<tree>"}`, never pixels (as the epic says). **Canvas Region's key** includes node type/version, the tree hash, and the region parameters (rect or mask hash, layer or composite, padding, target size); its output is an image keyed by its own content hash, so a *new* revision that leaves the region's pixels unchanged yields the **same output hash and all downstream nodes still hit** (spec 10.1, last rule). Using the tree rather than a commit id is what makes that work after a no-op revision or a revert.
- It does not require reading the whole canvas to key: the key needs only the tree hash, known from the ref without decoding anything. But precision matters: with a per-tree key, *any* edit anywhere makes Canvas Region run again (cheap: it only reads the tiles), and the output hash decides whether downstream re-runs. **Option:** key by the hash of just the tiles the region intersects (manifest-derived, no pixel decode) to keep even Region itself a hit; I recommend not doing this at first (Region is cheap; one more key mode to test) [decision].
- **"Head resolved once at run start and recorded":** the run record stores `canvases: {<id>: "sha256:<tree>"}` before any node executes, and Region nodes that name `head` read that map, never the live ref. Implications:
  1. A re-run of the same run record (or the same workflow with the same revision) is a hit: reproducible, as required by the epic.
  2. **Staged / partial re-runs** (run from node X, frozen nodes, 10.4 Freeze): a re-run of a *later stage* must reuse the recorded revision of the original run, not the new head, or the unchanged upstream results would be stale against it. The UI choice is explicit: "re-run on the recorded revision" (default for a partial re-run, cache hits) vs "re-run on the current head" (a new run record, new keys, upstream re-reads).
  3. A frozen Canvas Region keeps its result and its tree hash (the frozen marker stores the key); a head that moved is shown as "stale vs head" in the node state, not silently refreshed (10.3 states).
  4. The `head` literal in the saved workflow is a *name*, so it must not enter the key; the resolved tree hash does. The validator should show "head = rev N" so a user sees what a run will read.
  5. Tag/branch names resolve the same way (resolved once).
- **Canvas Write** (sink): its natural id is deterministic: new tree = parent tree + new layer whose id, name and tiles derive only from the image hash and parameters (no timestamp, no counter from the clock), so the *same* inputs produce the *same* new tree hash. Re-running with unchanged inputs makes no duplicate (mirroring the spec's rule for sinks: "with unchanged inputs and the file still present, it does not write a duplicate"). The commit meta (time, run id) is separate. Whether to move head is a separate step (the window's accept, or the node's `advance head` setting), so a generated layer can wait in a staging area (InvokeAI's staging model, from the epic) without becoming head.
- Result-cache storage: the existing `blobs/<hash>.<ext>` store stays for node outputs; canvas tiles do **not** live in the evictable cache (10.2 says the cache evicts). Canvas objects are the user's document: separate, never evicted by cache size limits.
- `deterministic: false` nodes put the run id in their key; a canvas revision produced by one is still a normal immutable object.

### 4.5 Garbage collection, retention, integrity, crash safety, concurrency (question 3)

- **Objects are immutable; GC is mark and sweep from roots.** Roots: every ref (head, branches/tags), every revision named by a retained run record, frozen markers, and the last N revisions of each canvas. Mark: walk manifests to tile hashes; sweep: delete unreferenced blobs older than a grace period (e.g. 24 h, so a writer's in-flight tiles are never swept). Cheap: a manifest is a few KB, so marking 1000 revisions is reading 1000 small files.
- **Retention policy** (all per canvas, defaults a proposal): keep the last 50 revisions or 30 days, whichever is more, plus everything tagged/pinned plus anything a retained run record or `used_by` points at; an optional size cap (per canvas, default e.g. 5 GB) drops the oldest untagged revisions first and says so. A revision removed from the middle of a history leaves its children valid (the tree is self-contained; only `parents` pointers dangle and are treated as "pruned"). Estimated rate with painting at 1 MB per edit and 4 MB per generated layer: 50 revisions is ~50 to 200 MB at 4096 [estimate from the measurements, not measured as a series].
- **Integrity:** hash on write (address = content hash); on read, hash the decoded tile and compare to its name; mismatch means quarantine (rename to `.bad`), error `canvas_blob_corrupt` naming the revision, and for a peer-supplied blob, refetch. A revision manifest is verified by re-hashing it against its file name. Also a `fugue canvas verify` command (walks everything). Cost is low: hashing all layers of a 12-layer 4096 canvas took 0.5 s raw.
- **Atomic writes and crash safety:** the code already has `engine/atomic_file.h` (used by `writePaintFile`: temp file and rename, [U: implementation read only by name and use]). Order: (1) write all tile blobs (temp file, fsync, rename into `objects/`); (2) write the manifest the same way; (3) write the commit meta; (4) only then update the ref (rename over `refs/head`, fsync the directory). A crash before step 4 leaves unreferenced objects that GC removes; a ref never points to something missing. Blobs are idempotent: a second writer writing the same hash is a no-op, and `rename` over an equal file is harmless. Startup check: ignore and remove `*.tmp`.
- **Concurrent writers** (the window and a running workflow, or two workflows): each writer creates revisions **without locking** because objects and commits are append-only and content-addressed; the only shared mutable thing is the ref. Moving head is a compare-and-swap: `refs/head` stores `{tree..., commit...}`; a writer advances it only if it still equals the commit that writer based its revision on (a `flock` on `refs/head.lock` around read, compare, rename). If head moved (parent not equal head), the writer does one of:
  1. **Rebase** (automatic when safe): the new layer is independent of what changed (Canvas Write adds a *new layer*; the edit in head touched other layers or other tiles), so re-apply "add layer L" on top of the new head. For tile-level independence the check is "none of L's tiles overlap tiles the intervening commits changed on the same layer"; adding a whole new layer always rebases cleanly. This is the common case and costs one manifest write.
  2. **Keep both**: otherwise (same layer, same tiles changed, or the user chose it) the writer leaves its revision on a side ref `refs/branches/<run id>` and the UI offers "make this head", "compare" or "discard". Nothing is lost and nothing blocks.
  The window follows the same rule: each stroke-end or accept makes a revision on top of the head it started from. The window is never blocked by a running workflow, and a workflow's Write never silently overwrites the person's later edits.
- **Head pointer storage:** one small JSON file per ref (`refs/head`, `refs/tags/<name>`), replaced by atomic rename; the reflog (`refs/head.log`, append-only, one line per move: old, new, who) gives "undo the head move" and audit. Not in the `.ora`, not in the workflow file (the workflow stores only the canvas id and a name like `head`).
- Undo in the window: `PaintDocument` keeps its own in-memory history inside a session. Whether each undo step becomes a revision (cheap, ~1 MB) or only saves/accepts do is a product decision; the storage supports either [decision].

### 4.6 Where it lives, sharing (question 5)

- **Inside the project (#204).** #204 makes one root (`FUGUE_HOME`) hold cache, run folders, inputs, results, settings; private projects are encrypted/ephemeral vaults, "a cache hit would reveal that a prompt was run before". A canvas is user content with a privacy profile like inputs, so it belongs in `<project root>/canvases/<id>/` and thereby follows the vault, the backup and the "nothing outside the vault" test; the shared-between-projects option is a library that a private project must not write to, with an explicit import (copy by hash) instead. The mesh (#203) exchange by hash fits: a peer in a private project only receives what that project allows.
- **Inputs folder next to the workflow:** that is how `paint` and `mask` inputs work today (`~/Pictures/Fugue/inputs/<stem>-paint-<10 hex>.ora`, one file per save, never deleted). It would make each revision a new input file, pollute the shared inputs folder with the 65 MB blobs measured above, and has no manifest or refs. Rejected for the store; **accepted for the export** (a node that needs a file path, e.g. an existing `paint` widget, gets an exported `.ora` keyed by bytes as today).
- **A canvas library** (cross-project, `~/Pictures/Fugue/canvases`): conflicts with project isolation; defer. If wanted, it is just another store root with the same format, plus `fugue canvas copy` between roots (copy missing blobs by hash).
- **Sharing today:** `.fuguepack` is a zip; `inputs/` carries each `paint` input's `.ora` ("by its bytes, at most 256 MB"), renamed to a safe name, workflow rewritten to name `inputs/<name>`. Canvas sharing in a pack = the head revision exported as an `.ora` into `inputs/` (history not included by default; an option "with history" can add the manifests and tiles as `canvas/objects/...`, with the 256 MB cap in mind: the measured 10-revision store is ~72 MB). A canvas that exceeds the 8192x8192 / 1 GiB / 256 MB `.ora` caps (see 2.1) cannot be shared as an `.ora` and needs the "with tiles" form [decision for the owner].
- Workflow reference: a Canvas node holds a **canvas id + optional named revision** (a name or a tree hash), not a path; the project resolves the id. The share package then needs a canvas export for each id the workflow names (pack-level import creates a fresh canvas in the recipient's project, with a new id and the imported head as revision 1).

## 5. Test plan

Determinism and identity
1. Same layers, same properties give the same tree hash and byte-identical manifest on two runs and two machines (also with libpng at different compress levels: tile address is of raw pixels, so equal).
2. Tile address ignores PNG encoder settings; changing one pixel changes exactly one tile hash and the tree hash; reverting restores the old tree hash.
3. The tree hash ignores parents, time, producer, label (two commits, one tree).
4. `.ora` export of a revision has the same bytes twice (existing `paintToOra` property) and round-trips: import export gives a tree equal to the original (layer ids renumbered: define the rule).
5. Canvas Write with identical inputs produces the same new tree hash (idempotent); no second revision on re-run.

Storage and cost
6. Edit one 512 px region: only the touched tiles are new blobs (assert count <= 9, bytes < 2 MB on a seeded random image); all-zero tiles are not stored.
7. 4096x4096 x 12 layers: time budget for first revision < 5 s and for a 512 px edit < 1 s (the measurements above are 2.6 s and 0.2 s); 8192 with 3 RGBA layers.
8. A region read decodes only intersecting tiles (instrument a decode counter).
9. Canvas larger than the `.ora` caps stores and reads fine as tiles; its export fails with a clear error naming the cap.

Integrity, crash, GC
10. Flip a byte in a tile blob: read gives `canvas_blob_corrupt`, blob quarantined, other tiles unaffected; a peer-fetched bad blob is refused.
11. Kill the writer (fault injection) after each of the four write steps: no ref points to a missing object; the next start cleans temp files; GC removes the orphans only after the grace period.
12. GC: keeps tagged, last N, run-record and frozen references; removes the rest; a retained child of a pruned parent still opens; size cap drops oldest untagged first.
13. Disk full during a write leaves head unchanged and reports an error.

Concurrency
14. Two writers on the same parent: both revisions exist; head moves once; the loser rebases when it only adds a layer, and keeps a side branch when tiles conflict. Run under a stress loop with many threads (and two processes with `flock`).
15. The window edits during a run: the run still reads the recorded revision; its Write rebases onto the window's newer head.

Cache and runs
16. Region node: same revision gives a cache hit; a new revision that does not change the region gives a Region re-run but its output hash is equal and downstream nodes hit.
17. `head` is resolved once: move head mid-run, and a later Region in the same run still reads the recorded tree; the run record lists it.
18. Partial re-run from a middle node uses the recorded revision by default and the new head when asked; a frozen Region keeps its result and shows "stale vs head".

Interop and sharing
19. Open an exported `.ora` in Krita, GIMP and MyPaint (manual, headless where possible): layers and names intact, root `fugue:*` attributes do not stop the load; record each editor's treatment of unknown attributes, and of a missing `mergedimage.png`. [U until run]
20. Re-import an `.ora` saved by Krita: parsed or refused with `media_unreadable` and a clear message (today's reader refuses groups, offsets, extra elements and entries).
21. A `.fuguepack` with a canvas: import makes a new canvas in the recipient's project; the 256 MB cap message for larger ones.
22. Mesh transfer: send a manifest, the receiver lists missing hashes, fetches, verifies, ends equal; interrupted transfer resumes; a malicious blob (wrong hash) is rejected.
23. Private project: the canvas store lives under the vault; the empty-HOME guard test from #204 still passes.

## 6. Open questions (for the owner)

1. Tree/commit split (recommended) or one hash that includes the parent?
2. Tile size 256 (matches `cell_grid`) vs 512 (fewer files, 4x the edit cost)? My numbers say 256.
3. Does every undo step or stroke-end make a revision, or only save/accept/Write? Affects retention numbers (about 1 MB per revision at 4096).
4. Retention defaults (last 50, 30 days, 5 GB cap) are my proposal.
5. Do we want to lift the 1 GiB / 3-layers-at-8192 `.ora` cap with the store, and is a pack "with tiles" format wanted for canvases beyond the cap?
6. Add `mergedimage.png` and a thumbnail to the `.ora` export for better interop (not needed for hash identity, breaks nothing, costs a flatten at export)? Needs a test in the real editors.
7. How other editors treat unknown XML attributes and entries [U]: needs the interop run in Test 19 before the `fugue:revision` root attribute is committed to.
8. Per-key tile-set hashing for Canvas Region (keeps Region itself a hit when an unrelated area changes): wanted, or run it and rely on the output hash?
9. restic/borg/casync/ostree numbers were not measured; the conclusion rests on design reasoning for those.

---

# Fugue epic #206 (canvas document): code research

Repo `~/github/fugue`, read-only, no build. Paths are relative to it. "Canvas" in `src/canvas/*` means the node-graph canvas, not a paint canvas: the paint canvas is `PaintLayer`/`PaintDocument` (a name clash to keep in the epic's vocabulary: call the new thing a "canvas document" or "picture canvas" in code, e.g. `src/engine/canvas_store.*`, `fugue/canvas`).

## 1. Fit with the node model

**Sinks.** `"sink": true` in the manifest (`src/engine/node_type.cpp:419`, `node_type.h:82`) means "an end point that keeps the node in the compiled graph": the compiler drops every node no sink depends on (`compiler.cpp:1075-1106`; `CompiledGraph::sinks()` 1188). Save Image (`src/nodes/manifests/save-image/node.json`: sink, `needs.files`, an `image` output "file") and Save Video are sinks; Preview is a sink with no outputs, whose inputs are reported as `shown` (`scheduler.cpp:74,1217-1220`, spec 9.4). Sinks inside groups are removed unless they are the target (`compiler.cpp:1041`). A sink is still a normal cached node: its key is looked up and a hit skips the kernel (`scheduler.cpp:590-600`; spec 10.2 "A sink records what it wrote; with unchanged inputs and the file still present, it does not write a duplicate"). So Canvas Write as a sink gets "same inputs = no second revision" for free, and the head move must be treated as a side effect like Save Image's file (outside the key).

**Inputs.** Load Image (`manifests/load-image/node.json`) has a `file` text input with `"widget": "file"`; Paint Mask a text input with `"widget": "mask"` and `"over": "image"`; Paint a `"widget": "paint"` (`.ora`). These three widgets are what `InputSpec::holdsInputFile()` returns true for (`node_type.cpp:361-364`). Consequences for such an input (`scheduler.cpp:779-815`): the text literal is trimmed, `~` expanded, resolved against the workflow folder (`inputFilePath`), checked against the read roots (`FileRoots::checkRead`), and the key gets `{"file": <content hash>}` instead of the path. `forEachFileLiteral` (`file_inputs.cpp`) walks these for share copies/packages/templates.

**`deterministic`.** Default true (`node_type.h:83`); false adds the run id to the key (`scheduler.cpp:588`), and the node is always stale under validate. No built-in manifest sets it to false (grep). Neither Region nor Write should be non-deterministic: Region is keyed by the revision hash, Write by its inputs including the parent revision.

**`frozen`.** A node field (`document.cpp:203,601`, `compiler.cpp:870,1108`, `CompiledNode::frozen`): a result-cache key. A frozen node serves that key's result whatever its inputs are or what failed upstream (`scheduler.cpp:494-505`), else warns `frozen_result_missing` and runs. So a frozen Canvas Region keeps its crop + descriptor of the old revision even after the head moves, which is the natural "pin" and costs nothing. A frozen Canvas Write replays its recorded descriptor and does not write.

**Cache key (`result_cache.h:39-56`, `scheduler.cpp:560-596`).** SHA-256 of canonical JSON of: node type id and version, pack, pack version, `inputs` (name -> `Value::identity()`; file-widget text inputs as `{"file": hash}`), provider/model/extra (provider nodes only; `keyExtra` exists only on `ProviderKernel`, `node_kernel.h:150`, not on a plain `NodeKernel`), run id when non-deterministic. `Value::identity()` (`value.cpp:38-100`): a media handle without `path`; a data handle (`"data"` key) with only the top-level `path` removed (`jsonIdentity`) so **nested paths stay in the identity**; a resource handle by `resourceIdentity`. Upstream links contribute the upstream output's identity, i.e. its content hash.

**`fugue/stitch`.** Declared in `src/nodes/manifests/pack.json:182` as `{"id": "fugue/stitch", "version": 1, "kind": "data"}`. Value = `{"data": "fugue/stitch", "path": <workDir>/stitch.json, "meta": {image: <MediaHandle json>, mask: <MediaHandle json>, box:{x,y,width,height}, size:{width,height}}}` (`crop_stitch.cpp:100-112`); the file holds canonical `meta` only, so its hash is the meta's hash (file header comment lines 7-10). The scheduler adds `hash` by hashing the file (`withFiles`, `scheduler.cpp:141-153`); the cache moves it into `blobs/` and revives it (`result_cache.cpp:235-247,307-313`). Stitch re-hashes the two referenced media files and fails `file_not_found` if changed (`crop_stitch.cpp:150-157`). Wrinkle (existing, unverified in effect): `meta.image` is a full `MediaHandle::toJson()`, which I believe carries `path`, so the key of a node fed a stitch contains a nested absolute path (cache-store path, stable per content). A canvas descriptor must not do that.

**A `fugue/canvas` data type needs:**
- Registration: one more entry in `types` of `src/nodes/manifests/pack.json` (`"kind": "data"`) (loader: `pack_loader.cpp:171`, `Registry::add(DeclaredPortType)` `registry.h:52`). Nothing else for compatibility: the type system is nominal and `compatibility()` in the compiler (`compiler.cpp:742-753`) already requires the same id and version (spec 2.3). A Region output and a Write input of `fugue/canvas` connect; nothing else does.
- Value: a data handle like stitch: `{"data":"fugue/canvas","path":<workDir>/canvas.json,"meta":{"canvas":"<canvas id>","revision":"sha256:...","size":{w,h},"origin":{x,y}?}}`. No pixels, no absolute path inside `meta` (use a canvas id resolved by the store under the project root) so the identity is portable and the key changes exactly with the revision. A data handle must name a readable file (`withFiles` fails `node_bad_output` otherwise, `scheduler.cpp:146-149`), so write a tiny `canvas.json` in `workDir` like Crop to Mask does.
- UI colour/shape: automatic. `CanvasModel::colorKey` (`canvas_model.cpp:183-189`) gives every declared type the key `"declared"` (the theme's accent, `look.cpp:67-105`, `portColors` 286-330) and `shape()` a diamond for non-builtin kinds. So `fugue/canvas` looks like Stitch/Latent. A distinct colour would need a new key in `colorKey`, `portRoles`/`keepOrder` (look.cpp) and the `Theme::portColors` map. Not recommended for v1.
- Validation: a data type "takes a link only, not a value" (`node_type.cpp:580`), so a literal cannot hold a canvas handle. That shapes the node design (below): Region names its canvas by a text input, and outputs the handle.
- .fugue serialization: nothing new for the handle (it never lives in a document; only literals and links do). What is stored is Region/Write's literal inputs (canvas name/id, revision text, geometry). Document format changes are not needed.

**Region and Write as nodes (proposal):**
- `fugue/canvas-region` (input): inputs `canvas` (text; new widget `canvas`, **not** `holdsInputFile`, see below), `revision` (text, default `head`), `source` (choice: composite | layer), `layer` (text, optional), `x,y,width,height` (integers) or a `mask` input of role mask, `padding`, `megapixels`, `multiple` (reuse Crop to Mask's). Outputs: `image`, `mask` (role mask), `stitch` (`fugue/stitch`), `canvas` (`fugue/canvas`, the read revision), width/height.
- `fugue/canvas-write` (sink): inputs `canvas` (`fugue/canvas`, the base revision; makes the parent explicit and part of the key), `image`, optional `mask`, `stitch` (optional, for placement), `x,y`, `layer name`, `blend`. Output: `canvas` (`fugue/canvas`, the new revision, so a chain of writes and a following Region can read it, also the sink "result"). The `fugue/canvas` input being required (not a path) keeps the dependence in the graph: a Region -> inpaint -> Write chain has the right edges, so the Write key contains the revision Region read.
- Why not widget `file` for the canvas path: `holdsInputFile` would hash the whole `.ora` file as `{"file": hash}` and run read-root checks, i.e. the key would follow the mutable head file's bytes and ignore the revision input. Use a new widget (e.g. `canvas`) and add it deliberately to the places that enumerate file inputs if it must travel (see section 5).
- Registration pattern: manifest under `src/nodes/manifests/<name>/node.json`, kernel in a new `src/nodes/canvas_nodes.cpp` + `addCanvasKernels` called from `src/nodes/builtin.cpp:30` (`addBuiltinKernels`), like `addCropStitchKernels`.

## 2. "Head resolved once at run start and recorded"

There is no existing run-start resolution hook for inputs, only: (a) `auto` values (random seeds) resolved inside the scheduler per node at `resolveValue` (`scheduler.cpp:660-669`) and recorded in `run.json` `auto` (`scheduler.cpp:375,395`); (b) a provider's selected instance/model recorded in `run.json` `providers`. Neither is replayed on a re-run; a re-run resolves again (a new seed, a new provider choice). Spec 10.1 says "an `auto` value is resolved before keying ... Every result is therefore reproducible" in the sense of recorded, not replayed.

Where a run starts and what it records:
- `compileForRun` (`src/engine/compile_for_run.cpp:17-37`) is "the one place that knows the recipe" and is called by the window's `RunService` (`run/run_service.cpp:377`), `fugue run`/`validate`/`compile` (`cli/compile_step.cpp:12`). It takes a `RunCompileSetup` of callbacks (`registerProviders`, `trusted`; `compile_for_run.h`), which is the existing pattern for injecting machine knowledge.
- The compiled graph (`CompiledGraph`, `compiler.h:64-90`) has `source` = `Document::sourceHash()` plus every node's resolved `inputs`; it is written to `compiled.json` by `RunFolder::writeCompiled` (`run_service.cpp:220`, `cli/runs.cpp:165`), and `writeWorkflow` keeps the document (`run_service.cpp:222`). `fugue run compiled.json` replays a compiled graph (`compiler.h:111-113`), so anything resolved before this point is replayed exactly.
- `run.json` (`scheduler.cpp:360-396`, spec 9.7) records status, `workflow` {path, hash = `graph.source`}, `auto`, `providers`, per-node key/outputs.

**Recommended placement:** a pass in `compileForRun` after `compile()`: for every compiled node of type `fugue/canvas-region` whose `revision` literal is `head`, ask a new `RunCompileSetup::resolveCanvasHead(canvasId) -> revision hash` and substitute the literal in `CompiledNode::inputs` (and keep a `resolved` note, e.g. a `canvas_heads` map in compiled.json / run.json). Because the substitution happens before the scheduler, the cache key automatically contains the concrete revision hash, `compiled.json` is a faithful record, and the window's 300 ms `checkGraph` (`run/run_service.cpp` check path, canvas_runs.h header) re-resolves each time, so a Region goes stale in the node-state view when the head moves. Alternative (scheduler-side resolution beside `auto`) works for recording but cannot be replayed from `compiled.json` and complicates `m_dry`; not recommended.

Consequences:
- **Re-run:** the document still says `head`, so a re-run resolves again; reproducing a past run = replay its `compiled.json` (revision pinned) or pin the node (a "Pin to revision" command that writes the hash into the literal). Needs a decision from the owner: whether re-run in the window means "same head" or "new head". The epic text says "re-running the same revision is a cache hit", which holds as soon as the revision text is concrete or the head has not moved.
- **Partial / staged re-run ("Run to here", Shift+Enter; staged = the Sampler/VAE stage nodes):** each run is a fresh compile (`canvas_runs.cpp:132 startRun` takes the document as it is), so a run-to-here after a Write moved the head re-reads the new head for Region and everything downstream re-keys; upstream of Region is untouched. Two partial runs of one workflow can therefore disagree about "head". Mitigation: freeze or pin Region (a frozen Region serves its frozen key regardless, `scheduler.cpp:494`), or record the heads of the last full run per tab and offer them.
- **Write moving the head inside the run:** a Write after a Region of the same canvas makes the head differ from what the same run's Region read. That is the reason Write takes the parent from its `fugue/canvas` input (the revision Region read), never from "head now". If head != parent, create the new revision as a child of parent (a branch), and let the window offer "rebase / keep both" per the epic. Writing must be serialized per canvas (file lock in the canvas folder, or only the engine-lock holder writes: runs already serialize by `engine.lock`, spec 9.8, but the window's own edits also write, so use a small per-canvas lock file).
- **Frozen Write:** serves the recorded descriptor; does not touch the store; the recorded revision's blobs may be gone (see risks).

## 3. Selection: mask, crop, stitch

How masks flow today: a mask is an `image` port with `"role": "mask"` (one channel, 1 = change; spec 2.1; hollow square). Producers: Paint Mask (`manifests/paint-mask`, widget `mask` over an input `image`, kernel `nodes/mask_input.cpp`), Load Image's `mask`, Paint (`nodes/paint_input.cpp`, flattens a `.ora` via `PaintDocument::flatten`/`flattenMask`), pack nodes Remove Background (`examples/packs/io.github.bmaltais.fugue-remove-background/remove-background/node.json:21`, output `mask` role mask) and Segment (`examples/packs/io.github.bmaltais.fugue-segment/segment`), and the mask node set (`nodes/mask*.cpp`: grow, feather, threshold, combine, invert). Consumers: Crop to Mask, Stitch (optional `mask` override), Fill Masked, Composite Masked, VAE Encode.

Crop to Mask (`crop_stitch.cpp:49-116`, ops in `crop_stitch_ops.*`): inputs image, mask, padding, megapixels, multiple; cuts the mask's bounding box plus padding (`cropToMask`), resizes to about N megapixels in multiples of 16, outputs the crop image, the cropped mask, the `fugue/stitch` descriptor (original image handle + mask handle + box + size) and width/height. Stitch (`crop_stitch.cpp:159-193`, `stitch()` in `crop_stitch_ops.cpp:86-94`): fits the redrawn crop to the box (Lanczos), `featherMask(..., Outward)` over the whole picture, `cutBox`, `compositeMasked(original, fitted, &inBox, box.x, box.y)`. The inpaint group `src/nodes/manifests/inpaint-crop-stitch/group.fugue` is Crop to Mask -> Text Encode -> VAE Encode -> Sampler -> VAE Decode -> Stitch, with exposed inputs including `padding`, `megapixels`, `blend`; templates `inpaint-region` and `outpaint` (`templates/`) use it.

**Canvas Region producing the same outputs:** the kernel reads the revision (composite or one layer) into a `Pixels`, takes the selection as either a rectangle (literal x,y,w,h, or a `mask` input) or the canvas's mask layer, then runs the same `cropToMask` (or a rectangle crop with the same padding/megapixels/multiple rules) and outputs `image`, `mask`, `stitch`. To make the unchanged inpaint group usable unchanged, the group's `crop` node is replaced by Region's outputs: group inputs are `image`/`mask`/`stitch`, so a variant group (or template) takes them from Region. Cheapest honest implementation: Region = Crop to Mask with the picture and mask read from the canvas; the `stitch` descriptor's `meta.image` is a media handle of the flattened region source saved in the node's workDir (`MediaHandle::saved`, as `named()` does, `crop_stitch.cpp:41-48`), so the existing Stitch kernel works unchanged and yields a **full-picture** composite. A Canvas Write fed that full picture would replace the whole canvas layer; better is a Write that accepts the `stitch` + redrawn crop and writes a **layer** = redrawn crop fitted to the box with alpha = the feathered mask cut at the box (the `inBox` mask in `stitch()`), positioned at `box.x, box.y`. That needs a small refactor in `crop_stitch_ops` to expose "fitted crop + inBox alpha" as an RGBA patch (the placement math is already there: `resample`, `featherMask`, `cutBox`), and the box must be translated by the Region's own offset into canvas coordinates (store `origin` in the descriptor; Stitch itself works in source-image coordinates, which equal canvas coordinates only for a full-canvas source). Keep Stitch's `shapeFits` 2 % check (`stitch_mismatch`).

Selection UI model: the epic's "rectangle, lasso, mask layer, a node's mask output" all reduce to the existing `role: mask` image; the window can paint into the canvas's mask layer (PaintDocument already has `LayerKind::Mask`), and a wired `mask` input on Region overrides it (as Stitch's `mask` input does).

## 4. Window side

- Paint window: `MaskEditor.qml` with `layers: true` (`FugueCanvas.qml:640-668`, `openPaint`), driven by `PaintLayer` (`src/canvas/paint_layer.h`: wraps a `PaintDocument`, draws via `CellGrid`, `layers` property, `setPicture`, `store` into `folder`/inputs dir, "knows nothing of nodes or of the window"). It edits a **node's `paint` input**: `CanvasModel::paintState(node)` / `setPaintPath(node, path)` (`src/canvas/mask_edit.cpp:54-78`) read and write the node's text literal through `widgets()->setText` (an undoable edit), and `App.runs.inputFile` / `inputPicture` (`run/canvas_runs.h:123-126`) resolve file and linked picture. Data: `.ora` via `engine/paint_file.h` (`paintToOra`, `paintFromOra`, deterministic bytes, hostile-file limits, picture layer = placeholder, one transparent pixel; limits 16384 px side, 8192x8192 pixels, 64 layers, 1 GB).
- A canvas viewer/editor fits as a second host for `PaintLayer` that is **not bound to a node input**: it opens `canvas id` -> head revision -> `PaintDocument` (via a loader beside `readPaintFile`), and "save" makes a new revision instead of `setPaintPath`. The existing `paintDialog` has the shape (a modal `MaskEditor`); a persistent side panel needs a new component built on `PaintLayer` plus a revision list (a `ListView` of `{hash, parent, time, producer}`) in the style of `MediaBrowser`/`SidePanel`.
- Panels: `src/panels/side_panels.*`, `SidePanel.qml`, `SideTabStrip.qml`, `WorkflowsPanel.qml` (files/results switch, results in `MediaBrowser`, `src/media/*`: `MediaLibrary::resultsOf(workflow)`, `resultsOfRun`). A "Canvases" view (third state of the Workflows panel's switch, or a new side tab) listing canvases of the project and their revisions fits the pattern; thumbnails reuse `thumbnail_cache`.
- Notification of a running workflow: `RunService` emits `event(run, QJsonObject)` (`run/run_service.h:129`), `CanvasRuns::onEvent(run, event)` (`canvas_runs.h:189`) already consumes `node.done`/`node.cached` events with `outputs` and a sink's `shown` (spec 9.4). A Canvas Write's output (the `fugue/canvas` handle with the new revision hash) arrives in `node.done.outputs`, so the viewer can react to it with no new event type: CanvasRuns (or a new `CanvasDocuments` QObject) filters events whose node type is `fugue/canvas-write` (or whose output is a `fugue/canvas` handle), then reloads the head and appends a row. A new event kind (`canvas.revision`) is only needed for edits made outside runs (CLI, mesh peers); `fugue runs events` clients ignore unknown events (spec 9.4), so adding one is safe. Edits made in the window while a run is going: a window edit also appends a revision with a parent; the lock above serializes.
- Accept/reject of a generated layer (epic step 4): a Write should add its layer **hidden or marked "proposed"** in the manifest, accept = a new revision that flips it, reject = the head returns to the parent (nothing deleted). Not read in code; design only.

## 5. Projects, locks, mesh, share packages

- **Projects (`src/engine/project.h`, spec 1.4.2):** a project is a `FUGUE_HOME` root with `project.json` (`{"format":1,"name","inherits"}`, keys it does not know are kept). A canvas store belongs under the project's home: add a path function in `src/engine/paths.h` (e.g. `canvasesDir()` = `<documents or data>/canvases/<id>/`), and add it to the home-guard test (`tests/cli/test_cli_home_guard.cpp`, spec 1.4.1 requires every new write path to be in `paths.h`). Canvases must never be inherited (list them with the "never inherited" items, or leave them off `inheritableItems()`, `project.h` top). Read/write roots (`FileRoots`, `file_roots.h`, spec 13.6): the kernel reads and writes the store through its own code, not through a `file` input, so it must itself check the canvas folder is under the allowed roots or the project's own folder (add it to `FileRoots::defaults`/`forInputs`, like the inputs folder).
- **Cross-root lock:** the GPU lock (`fugue-gpu-<uid>.lock` in the base runtime folder, spec 1.4.2 "One GPU, many roots", and per-root `engine.lock`, spec 9.8) serializes **runs**, not store writes. Canvas writes need their own: a lock file inside the canvas folder (`flock`), since the window's edits and a run's Write can overlap, and two roots can share one canvas folder only if the user pointed both at it (unsupported; say so).
- **Mesh (docs/mesh-research.md, issue #203):** revisions content-addressed by hash fit the mesh's blob-by-hash direction: the doc plans `ResultCache::hasBlob`/`importBlob` and notes handles never contain paths. A revision = manifest + layer blobs, each a SHA-256 object, so a peer fetches the manifest by hash then missing layer blobs. Rules it implies for #206: no absolute paths in the descriptor or manifest (above), layer blob files hashed by content, manifest canonical JSON (`canonical.h`, RFC 8785). The doc also marks `fugue/stitch` as a non-cuttable edge because of its nested paths; `fugue/canvas` is cuttable if the peer has the revision (descriptor only). Region is an input (like Load*), listed as a node the planned `cutSubgraph` rejects, so a remote run of a Region requires shipping the revision by hash.
- **Share packages (`src/engine/share_package.cpp`, `share_copy.cpp`, spec 11.5/11.10):** a package carries only images (`image_format.h`) and `.ora` paint files named by `file`-type inputs (`share_package.cpp:124`: "is not an image or a paint file"), and rewrites the input to `inputs/<name>`. A canvas is a directory of blobs, not a file, so it is **not** shared by default: a Region with `canvas` = an id would break in the package. Options: (1) nothing travels, the share copy blanks `canvas`/`revision` and warns (extend `shareCopy` to treat the new widget like `file`: replace by bare name and warn); (2) the package carries the pinned revision flattened as an `.ora` into `inputs/` and the Region becomes a `paint` input (cheap, uses existing code, loses history). Recommend (1) for steps 1-3 and (2) as a later "export revision" command. `forEachFileLiteral` and `InputSpec::holdsInputFile` are the single predicate for file-like widgets; if the new widget is not added there, package/template/share copy ignore it (silently leaving a local id in a shared workflow: needs a test).

## 6. Smallest ordered changes for epic steps 1-3

Step 1: canvas document, revision store, `fugue/canvas` type, viewer.
1. `src/engine/canvas_store.{h,cpp}` (pure, Qt Core only): revision manifest `{parent, layers[{name,kind,visible,opacity,blob}], size, producer}` as canonical JSON, hash = manifest hash; layer blobs = PNG bytes by SHA-256 (reuse `MaskPaint::toPng` and the layer encoding in `paint_file.cpp`); `head` pointer file written atomically (`atomic_file.h`); `create`, `commit(parent, PaintDocument|layer patch) -> hash`, `read(hash) -> PaintDocument`, `list()`, per-canvas lock; export/import `.ora` (`paintToOra/paintFromOra`) so Krita/GIMP can open a revision and the existing Paint node can read one. Note a picture layer is a placeholder in `.ora` today (`paint_file.h`): a canvas holds real pixels, so it needs a real "raster" layer kind or the picture layer's bytes in a blob; `PaintDocument::loadLayer` takes pixels already. Tests: `tests/engine/test_canvas_store.cpp` (same content gives same hash and bytes, dedup of identical layer blobs, parent chain, `.ora` round trip via `test_paint_file.cpp` patterns, hostile inputs, concurrent commit under lock). Paths: `engine/paths.h` + home-guard test.
2. `pack.json` `types` entry `fugue/canvas` (data); a helper `canvasHandle(canvasId, revision, workDir)` writing `canvas.json` (as `crop_stitch.cpp:100-112`). Test: registry accepts; a link `canvas -> canvas` compiles, `canvas -> image` is `type_mismatch`.
3. Viewer (smallest): `CanvasDocuments` QObject (list canvases, revisions, thumbnail) + a "Canvases" view in the Workflows panel or a side panel; open head in a `PaintLayer`; save makes a revision. Tests in `tests/canvas`/`tests/panels` following `test_paint_layer.cpp`, `test_canvas_paint_node.cpp`.

Step 2: Canvas Region.
4. `manifests/canvas-region/node.json` + `nodes/canvas_nodes.cpp` kernel (+ `addCanvasKernels` in `builtin.cpp`); reuse `cropToMask`/rectangle crop, `named()`-style saving of the region source for the `stitch` handle; new widget `canvas` in `node_type.cpp`/canvas widgets (`src/canvas/widgets/`, `widget_edit.cpp`) for the picker (check how `paint` is wired there; I did not read the widget factory).
5. Head resolution: `RunCompileSetup::resolveCanvasHead` + pass in `compile_for_run.cpp`; wire the setup in `cli/compile_step.cpp` and `run/run_service.cpp`; record the resolved heads in `compiled.json`/`run.json`. Tests: `tests/engine` compile test (head -> hash substituted), scheduler test with `test_scheduler.cpp` helpers: same revision twice = cache hit, moved head = miss; frozen Region ignores a moved head; `fugue run compiled.json` replays.

Step 3: Canvas Write.
6. `manifests/canvas-write/node.json` (`"sink": true`), kernel: base = input handle's revision; build a layer from `image`(+`mask`/`stitch`); `commit` -> new `fugue/canvas` handle output; head move after commit (side effect) guarded by lock and "head == parent else branch". Refactor `crop_stitch_ops` to expose the fitted patch with alpha (`stitch()` pieces). Tests: Region -> Write -> Region chain; byte-identical revision for identical inputs; second run is a cache hit and does not add a revision; outside-pixels unchanged byte for byte (the 14.4 promise); stitch_mismatch.
7. `CanvasRuns::onEvent` hook so the viewer follows `node.done` of a Write; `templates/` generative-fill template is step 4, not here.
8. Docs: spec 2.6 (add `fugue/canvas`), 14.x (new nodes), 9.7/10.1 (head resolution), Appendix A (research entries; the epic asks for it) and the home-guard/paths docs.

**What would be hard:**
- Revision storage format and cost: whole-canvas PNG layers per layer per revision explode for big canvases (a 8192x8192 RGBA PNG is tens of MB); tiling (the paint code already tiles at 256 px in `CellGrid`/`MaskPaint`, per `paint_document.h` "undo is bounded by the tiles a stroke touched") suggests tile blobs, but then the manifest is a tile map and `.ora` export must reassemble. Decide before step 1.
- Cache hit vs missing revision: a Write hit returns a recorded descriptor whose revision objects may have been deleted or be on another machine. Add a check on revive (the cache validates blob files by hash only, `result_cache.cpp:285-300`), e.g. a `check()` in the kernel for validate, and make cache GC aware (`cache_manager.h` never touches the output folder; the canvas store must be outside the results cache and never garbage-collected silently).
- Head semantics across window edits, runs and partial runs (section 2): needs an owner decision on re-run behavior and a "pin" command.
- Sink side effect on a cache hit (no write, head unchanged) is correct but surprising to someone who wants "write again"; document it as Save Image does.
- Existing `.ora` rules ("a file that is not exactly what Fugue writes is refused", picture layer placeholder, no groups/blend modes) conflict with using `.ora` as the interchange of a canvas with raster layers and blend modes (epic step 5).
- `fugue/stitch` nested paths in the key: reuse works only because the region source is saved in the node folder and moved into the cache; it does not cross machines (mesh).
- The name clash "canvas" (graph editor vs picture canvas) across code, QML (`FugueCanvas`, `CanvasModel`) and the new type.

## Unverified

I did not read: the widget factory for new widget kinds (`src/canvas/widgets/`, `widget_edit.cpp`); `MaskEditor.qml` internals; `CellGrid`/`MaskPaint` tiling beyond the headers; `paint_file.cpp` layer encoding; `MediaHandle::toJson` (whether it includes `path`, which decides the stitch nested-path wrinkle); `run_service.cpp` internals beyond the lines cited (that `checkGraph` also goes through `compileForRun` I inferred from the header comment of `canvas_runs.h` and `compile_for_run.h`, which says all callers do); the scheduler's sink-cache-hit path beyond `scheduler.cpp:590-600`; the `.fugue` document reader/writer for unknown node widgets; `tests/` helpers' exact names. Nothing was built or run. Test files named as models exist: `tests/engine/test_scheduler.cpp`, `test_result_cache.cpp`, `test_paint_file.cpp`, `test_paint_document.cpp`, `tests/nodes/test_crop_stitch.cpp`, `test_paint_node.cpp`, `tests/canvas/test_paint_layer.cpp`, `test_canvas_paint_node.cpp`.
