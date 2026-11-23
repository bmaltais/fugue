# Photoshop (.psd) files: research, decisions, checklist (#227, epic #206)

Owner request 2026-11-22: a canvas must open in other tools. Photoshop does not open OpenRaster; film teams live on
PSD and EXR. The code is [src/engine/psd_file.h](../src/engine/psd_file.h) (no Qt GUI, no store); the exporter
([canvas_export.h](../src/engine/canvas_export.h)) and `fugue canvas import` are its callers.

## Sources, and how they were used

| Source | Licence | Use |
|---|---|---|
| Adobe, "Adobe Photoshop File Formats Specification" (adobe.com/devnet-apps/photoshop/fileformatashtml) | published spec | The only source the writer is written from. |
| psd-tools (MIT) | MIT | Ideas for the reader (what a real Photoshop file holds), and the independent reader for the round trip test. Its source also showed how a reader treats the merged image (below). No code copied. |
| psd_sdk (BSD-2) | BSD-2 | Read-only reference for details the spec leaves out (layer mask block sizes). No code copied. |
| GPL readers (Krita, GIMP, ImageMagick) | GPL | Not read for code. Their behaviour is what the checklist asks the owner to look at. |

## The format, as far as Fugue uses it

All numbers are big-endian. A file is: header (26 bytes), color mode data, image resources, layer and mask information,
merged image data.

- **Header**: `8BPS`, version 1 (2 is PSB, the large format), 6 zero bytes, channels (1 to 56), height, width, depth
  (1, 8, 16 or 32), color mode (3 is RGB). **PSD limit: 30000 px a side** (PSB 300000). Fugue's canvas cap
  (16384 px a side, 67 MP, `new_canvas.h`) is lower, so the limit never bites today; the writer still refuses past
  it, in plain words, and says to export .ora or .png.
- **Color mode data, image resources**: empty (length 0) when written; skipped when read.
- **Layer and mask information**: layer info (a count, negative when the merged image's first alpha channel is its
  transparency; the records; the channel image data), then the global layer mask info (written as length 0), then
  tagged blocks.
- **Layer record**: rectangle (top, left, bottom, right), channels (id and byte length: 0, 1, 2 are R, G, B; -1 is
  the layer's transparency; -2 is the user layer mask), `8BIM` and the 4-character blend key, opacity (0 to 255),
  clipping, flags (bit 1 set: the layer is **hidden**; the spec's wording calls it "visible", psd-tools and the real
  files agree it is "hidden" when set, and the round trip test confirms it), then the extra data: the layer mask
  block (20 bytes in the form Fugue writes: rectangle, default color, flags where bit 1 is "disabled", 2 padding), the
  blending ranges, the Pascal name padded to 4, and tagged blocks (`luni` is the Unicode name; `lsct` marks groups).
- **Channel data**: a 2-byte compression code, then the data: 0 raw, 1 RLE (PackBits, a 2-byte length per row first),
  2 zlib, 3 zlib with a per-row difference predictor. Fugue writes RLE; it reads all four.
- **Merged image**: the flattened picture, compression code then planes (all R, then all G, ...).
- **Blend keys** Fugue maps (to its OpenRaster composite-op, which it stores): `norm`, `mul `, `scrn`, `over`, `sLit`,
  `dark`, `lite`, `lddg` (Linear Dodge, which is Add). More are carried by name so a file survives a round trip:
  `idiv` Color Burn, `div ` Color Dodge, `hLit`, `diff`, `smud` Exclusion, `hue `, `sat `, `colr`, `lum `. Fugue does
  not draw those yet (the layer shows as Normal and the import says so); the key is written back as it came. The rest
  (`diss`, `dkCl`, `lgCl`, `lbrn`, `vLit`, `lLit`, `pLit`, `hMix`, `fsub`, `fdiv`) import as Normal with a warning.
  Soft Light differs a little between Photoshop's formula and the W3C one Fugue uses; flat colors are the same, gradients
  can differ by a few levels.

## Decisions (made without asking; each can be overturned)

1. **8-bit RGB only**, both ways. 16-bit and EXR are later (the issue says so). A 16-bit, CMYK, grayscale, indexed, Lab or
   PSB file is refused with the Photoshop menu path that converts it (Image > Mode ...).
2. **Every layer is the canvas size** on export (no trimming): simple, deterministic, and each layer's pixels are
   exactly what Fugue holds. A viewer that trims loses nothing.
3. **Layers are written unblended**: opacity, blend mode and layer mask are layer settings Photoshop applies, not baked
   in. (The .ora export bakes the mask into the layer for programs without masks; PSD has real masks.)
4. **Mask layers** (Fugue's selection-like layers) are **not written**; the export names them (CLI `data.notes`, a line in
   the Export window). Picture layers are written as the picture they show.
5. **Groups** (#252, merged in #273): a canvas group is written as a PSD layer group (a divider record `</Layer group>`, its
   layers, then the folder record with `lsct` type 1 or 2; the group's own name, visibility, opacity, blend key and layer
   mask). On import a group is kept as a real group, in ONE revision (`CanvasStore::createWithEdits` applies the Group,
   Visible, Opacity and Blend edits to the new tree), nested up to 8 deep, when it holds only layers Fugue keeps. A group
   that lost something (an adjustment layer or a clipped layer inside, its own mask or vector mask, a blend mode Fugue lacks,
   no layers at all, more than 8 deep) is **flattened** into the level below, with its visibility and opacity folded into
   each layer and one warning. Pass Through (`pass`) is read as Normal: a Fugue group composites as one, so a pass-through group
   can look a little different. A resized export (and the document path) flattens groups to one layer each and says so.
6. **The merged image is matted on white**, as Photoshop writes it: where there is transparency the stored color is
   `c * a + 255 * (1 - a)` and the alpha plane is stored too. Found in psd-tools' source (`_remove_white_background`):
   a reader takes the white out again with the alpha. Written plain, a reader would show dark fringes. The import of a
   file with no layers undoes the matting, and reads the 4th merged channel as transparency only when the layer count was
   negative (a flat file's 4th channel may be a saved selection). The test allows 2 levels of rounding for pixels at least half opaque.
7. **The layer count is stored negative**, as Photoshop does for a file whose merged image has transparency.
8. **Unicode names** go in both the Pascal name (ASCII, `?` for the rest, 255 bytes) and `luni`.
9. **Deterministic**: the same document gives the same bytes (no date, no program name, no thumbnail, no unique ids).
10. **Import is bounded** (see below); more than 64 layers is refused (a Fugue canvas holds 64) rather than dropping
    some, because which to drop is the user's call.

## What import keeps and what it reports

Kept: layers (pixels placed on a canvas-size layer by their rectangle, cut at the canvas edge with a warning), names
(`luni` first), visibility, opacity, blend mode, layer mask (position, default color, disabled flag), and for a file with
no layers its merged image as one layer.

Reported, one warning each (`psd_dropped`; the CLI prints them and puts them in the JSON `warnings`; the Canvas view
shows "Imported, with changes: ..." as its notice): groups flattened; adjustment layers dropped (brit, levl, curv, hue2,
blnc, blwh, mixr, clrL, phfl, vibA, nvrt, post, thrs, grdm, selc, expA, SoCo, GdFl, PtFl); text and smart objects kept as
the pixels Photoshop rendered; layer effects (`lfx2`, `lrFX`) dropped; vector masks dropped; clipping dropped; a layer past
the canvas cut; blend modes not drawn. Not read at all, and not reported because they hold no picture: image resources
(guides, resolution, ICC profile), the global mask, extra alpha and spot channels. A layer with a vector mask too has its pixel mask as
channel -3: that one is used. A mask whose rectangle is relative to the layer is placed from the layer's corner.

### Untrusted input

A `.psd` is hostile until read. The reader never reads past the bytes it has (every read is bounds checked and a failed
one stops the layer parse); no offset in the file is followed without being inside its parent; counts are capped (56
channels, 64 layers plus group markers); sizes are capped (canvas: 16384 px a side and 67 MP; a layer's decoded size: 134
MP); an RLE row is decoded only to the width the header promises and a run that would pass it is an error (so the
expansion is bounded by the declared size, not by the data); zlib channels are inflated with zlib directly into a buffer of exactly the promised size (a stream with more or less is refused, so a zlib bomb costs nothing); a rectangle is checked against the 30000 px limit before anything is allocated and only its part on the canvas is visited; every channel is read once (a duplicate id is refused), only for layers that are kept, and the bytes decoded in all are capped.
`TestPsdFile` cuts a good file at many points and corrupts 600 copies of it at random (a deterministic fuzz).

## Tests

- `tests/engine/test_psd_file.cpp`: header and sections, determinism, round trip through our own reader (pixels, names,
  visibility, opacity, blend, mask, disabled mask, odd sizes), the merged-only fallback, group and adjustment layers built
  by hand, refusals, fuzz, the exporter's `psd` format, `readCanvasFile`.
- **Round trip with an independent reader** (psd-tools 1.27, in a scratch venv outside the repo; nothing installed in the
  repo). `FUGUE_PSD_SCRATCH=<folder> build/test_psd_file` writes `sample.psd` and each layer's raw pixels, mask and
  settings; a script then opens `sample.psd` with `python -I`, and compares size, depth, color mode, every layer's name,
  visible flag, opacity, blend mode, mask (value for value, its disabled flag), bbox, pixels (alpha everywhere; color
  where alpha is above 0) and the merged picture. All equal on 2026-11-23. The other direction: a file written by
  psd-tools (a group, a hidden group, a layer reaching past the canvas, zlib compression, Color Dodge, opacity) was
  imported with `fugue canvas import` and every difference was reported.
  The script is short and lives with the owner's checklist below only as a description; it is not committed (a fixture
  would add nothing the test does not generate).

## Which programs accept it

Not tested here: Krita, GIMP, Photopea, Affinity and Photoshop were not installed and are not to be installed by the
agent. From their documentation, all five read layered 8-bit RGB PSD with layer names, opacity, visibility, the blend
modes above and layer masks; Krita and GIMP drop Photoshop-only features (not used here). That is expectation, not a
result: the checklist is what turns it into one.

## Checklist for the owner (real programs)

Make the sample first: in Fugue make a canvas with 3 layers (a normal one at 50 % opacity named with an accent, for
example "Ciel été"; one set to Multiply and hidden, with a layer mask; one set to Add), then File > Export > Export as...
and choose Photoshop (.psd), or `fugue canvas export <id> test.psd`. For each program, tick:

- [ ] Opens with no warning or repair dialog.
- [ ] Canvas size is right; mode is RGB, 8 bits.
- [ ] Three layers, bottom to top in the same order, names right (the accent too).
- [ ] The 50 % layer shows at 50 %; the hidden layer is hidden (and shows when turned on).
- [ ] Multiply and Add (Linear Dodge in Photoshop) are named so in the layer panel.
- [ ] The layer mask is a real mask (Photoshop: a mask thumbnail beside the layer; Krita: a transparency mask child; GIMP:
      a layer mask) and looks the same as in Fugue; with the disabled mask, the layer shows whole.
- [ ] The picture looks like Fugue's own preview (a few levels of difference in Soft Light are expected).
- [ ] No fringe around soft edges (the merged image, which a thumbnail shows, is matted on white by design).
- [ ] Save it again from that program as .psd, then Import picture... in Fugue: layers, names, opacity, blend, mask come
      back; the warnings list is empty or names what that program added (groups, effects).
- [ ] A file made in Photoshop with a group, a text layer, an adjustment layer and a smart object imports with one
      warning each and the pixels you see.
- [ ] Photoshop only: a 16-bit file is refused with the Image > Mode advice; a layered file over 30000 px cannot be made.
