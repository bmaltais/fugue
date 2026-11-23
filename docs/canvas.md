# Canvases

A canvas is a layered picture that Fugue keeps as a chain of revisions: a change never edits a
canvas in place, it makes a new revision and the old ones stay. The canvas work
([epic #206](canvas-research.md)) so far is the store, the command line, the two nodes that read a
region from a canvas and write a result back, and the review of what they wrote. The design and its reasons
are in [spec 14.5](spec.md#145-the-canvas-revision-store-206-208).

```
fugue canvas new --name Sketch --size 1024x768     # prints the canvas id, canvas-0123456789ab
fugue canvas new --size 1920x1080 --background white   # filled: transparent (default), white, black or #rrggbb
fugue canvas import photo.png                      # a canvas from a PNG, JPEG or WebP (one layer), an .ora or a Photoshop .psd
fugue canvas log canvas-0123456789ab               # the revisions, newest first
fugue canvas show canvas-0123456789ab@sha256:ab12cd34   # one revision: tree hash, layers, tiles
fugue canvas export canvas-0123456789ab out.ora    # the head as an .ora (never replaces a file)
fugue canvas export canvas-0123456789ab@head out.png  # flattened: .png, .jpg (on white) or .webp
fugue canvas export canvas-0123456789ab out.psd    # layers kept, for Photoshop, Affinity, Photopea, Krita, GIMP (docs/psd-research.md)
fugue canvas export canvas-0123456789ab team.fuguepack            # a package to share: the layers as they are now (head only)
fugue canvas export canvas-0123456789ab team.fuguepack --history  # ... with every revision, so the other person can undo
fugue canvas import team.fuguepack                 # a package's canvas, as a new canvas (a new id when yours is taken)
fugue canvas export canvas-0123456789ab out.webp --quality 80 --scale 50%   # lossy WebP at half size
fugue canvas export canvas-0123456789ab --format jpg --quality 60 --estimate   # the size it would be, nothing written
fugue canvas export canvas-0123456789ab shots --layers   # one picture per layer into the folder shots
fugue canvas batch generative-fill photos results --prompt "a red hat"   # an action over every picture of a folder
fugue canvas list                                  # the canvases, newest first
fugue canvas candidates canvas-0123456789ab        # revisions nobody points at (scripts)
fugue canvas diff canvas-0123456789ab@sha256:ab12cd34   # what a revision changed, against its parent
fugue canvas accept canvas-0123456789ab            # the one candidate becomes the head (scripts)
fugue canvas drop canvas-0123456789ab@sha256:ab12cd34   # hide one (it is kept)
fugue canvas layer list canvas-0123456789ab        # the layers, top first, with their positions
fugue canvas layer set canvas-0123456789ab "Fill 1" --blend multiply   # one new revision
fugue canvas undo canvas-0123456789ab              # back to the revision before; redo goes forward
fugue canvas resize canvas-0123456789ab --to -256,0,1280,1024   # grow 256 px to the left: one revision, a pre-filled "Expanded area" layer at the bottom
```

`<id>@<revision>` names a revision by `head`, its full hash, or the first 8 hex digits or more.

A canvas lives in your Fugue folder (`~/.local/share/fugue/canvases`), or in the project's own
folder when you work in a project ([projects](spec.md#142-projects)). Pictures are kept as 256 px
tiles named by their pixels, so a revision that changes one corner of a large canvas stores only the
tiles it touched, and the same pixels are stored once however many revisions or canvases hold them.
Two canvases with the same pixels and layer settings have the same **tree hash**, shown by
`fugue canvas show`. Nothing is deleted yet.

## In a workflow

Two nodes in the `Image/Canvas` category use a canvas by its id (Canvas Region's Canvas is a drop-down in
the window, see below):

- **Canvas Region** takes a rectangle (X, Y, Width, Height; 0 runs to the edge), a mask you connect, or a selection
  painted in the Canvas view (Selection, see Tools below),
  adds Padding around it, and gives you that part of the canvas as a picture, the mask of the part to
  change, and a stitch. Scale and Cap set the size it is redrawn at (a small region is drawn larger,
  up to the megapixel cap); Target size (megapixels, 0 off) instead redraws it at about that size, up or down, as
  Crop to Mask does, so a small selection reaches the model's size (the Cap still brings it down). Padding is held at
  Feather + 16 or more (the node warns `padding_raised`) because Canvas Write's blend stops at the region's box; Feather softens the mask's edge outward. Its Revision is `head` unless you
  give a hash.
- **Canvas Write** puts a picture on the canvas as a new layer, blended in through a layer mask
  (Blend pixels soften the seam): the layer holds the whole redrawn picture and the mask decides how
  much of it shows, so you can turn the mask off later to see all of it. **Bake the blend into the
  layer** (an advanced input) puts the blend into the layer itself instead, with no mask. The layer is
  named after the prompt that made it (or Fill 1, Fill 2, ... when there is none), unless you give
  it a name. Wire Region's canvas, stitch and your generated picture into it.

A fill is Canvas Region, then a node that redraws the picture (Generate Image or Edit Image, fed the
region's picture and mask), then Canvas Write, which takes Region's canvas and stitch and the redrawn
picture.

`head` is looked up once, when the workflow is compiled, and the revision's hash is what the run
uses and what `compiled.json` records. So running the same compiled workflow again is a cache hit and
writes nothing a second time, and a run can be replayed exactly. Canvas Write never edits a canvas: it
makes a new revision from the one Region read and moves the head to it: the fill is a layer on the
canvas at once, to hide, edit or delete (Undo removes it too). If something else moved the head
meanwhile, the layer is added onto the head as it is now, as a new revision. Only if that is
impossible (the canvas changed size) the run says so in plain words (`canvas_head_moved`) and writes
nothing. There is no review step: a run is a layer, delete it or Undo. The next run reads the head as it has become. To keep reading one revision (to try several fills of the same picture), give
Region that revision's hash, or freeze it.

Region's picture keeps the canvas's transparency. A stitch from Region also goes to Stitch. Region
reads the canvas as it looks (the visible layers, with their opacity, blend modes and masks); its
**Layer** input (advanced) reads one layer by name instead, its own pixels.

In the window, Canvas Region's **Canvas** is a drop-down of the canvases of the folder you are working in
(the Default's, or the project's own: a canvas belongs to the one it was made in). Each row has the name,
the size, the time of the last change and the id. **New canvas…** opens the New canvas dialog (below); **Import picture…** makes a canvas straight from a PNG, JPEG, WebP, `.ora` or `.psd`. The
canvas is put in the input. The input still holds the id (a workflow file is the same as before). An id this
folder has no canvas by, such as one copied from the Default into a project, shows as `canvas-… (missing)`
and the list is there to pick the right one. The list follows `fugue canvas new` and runs as they happen.

### New canvas

**File > New canvas…** (Ctrl+Alt+N) opens Photoshop's New Document dialog, from anywhere in the window; Canvas Region's **New canvas…** opens the same one. Give a name (Untitled 1, Untitled 2, … never one this folder already has) and say where the canvas starts:

- **Blank**: pick a preset (Square 1024 × 1024, Landscape 4:3, Wide 16:9, Portrait 3:4, HD, 4K) or type a width and a height, then a background: Transparent, White, Black or a Color. A canvas is at most 16384 px a side and 67 megapixels; the line under the fields shows the size in megapixels and says when it is too much. A transparent canvas is one empty layer, any other background one filled layer called Background.
- **Clipboard**: when you have copied a picture, the dialog opens on it with its size filled in, and Create makes the canvas from it (a screenshot, a picture copied from a browser). **File > Paste as new canvas** skips the dialog: it makes the canvas at once, named Pasted 1, Pasted 2, ….
- **A file…**: a PNG, JPEG, WebP, `.ora` or `.psd` file, as Import picture… does, named after the file.

The size, background and color of the last canvas you made are there the next time. A canvas made from the File menu opens in the Canvas view (in the active Canvas area when there is one).

**Pick region…**, the last entry of that list, opens the canvas at the node's revision (`head` is read as it is now) in a large view with a rectangle on it, so the region is dragged out instead of typed. Drag inside the rectangle to move it, drag a handle to resize it (Shift keeps its shape, Alt resizes from the center), or type X, Y, Width and Height under the picture; Free, Original, 1:1, 4:3, 3:2, 16:9 and Turn lock its shape, and **Snap to 16** (the node's Multiple of) puts its edges on multiples of 16. The dashed outer box is what the node will cut (the rectangle and its Padding), and the line under the picture says the size the model will get (Scale, Cap and Multiple of applied, as the node does). Zoom and pan work as in every large view; a large canvas is drawn from a smaller copy, the numbers stay in the canvas's pixels. **Apply** writes X, Y, Width and Height in one step (Undo takes all four back); Cancel or Escape change nothing. With a mask connected the node uses the mask's bounds and the window says so; with X, Y, Width or Height linked to another node there is nothing to apply.

## Fill a region

A fill lands on the canvas at once. The `generative-fill` template (File > Browse templates, or
`fugue workflow new fill.fugue --template generative-fill`) is Canvas Region, a FLUX.2 Klein redraw
and Canvas Write. Put your canvas's id and the rectangle in Canvas Region and run it.

## Harmonize a collage

Harmonize (the `harmonize` template, category Canvas actions) makes a picture assembled from pasted parts look like one
photograph: one light direction, colour grade, shadows, sharpness and grain, with every object and face kept. Select the
pasted parts and press Generate (or select nothing to treat the whole picture as pasted); the result is a layer called
**Harmonized**. Its prompt only adds to a built-in instruction ("make this one photograph: blend any pasted part into the
scene and remove hard edges and halos ..."). For a flat picture made elsewhere (a Photoshop collage as a jpg or png), the
`harmonize-image` template (File > Browse templates, Image editing) does the same from a Load Image with a painted mask of
the pasted parts and saves the result at the original size; paint nothing for a mild whole-picture pass.
In `harmonize-image` the painted mask is resized (Resize, node `mask_work`) to the size of the working picture (Resize to
Megapixels, node `work`) before the seam map is built, so the VAE Encode gets a picture and a map of one size for any
picture; the canvas action cuts both from the same region.

How it works (nothing new in the node set): the mask's edge becomes a seam band (Grow Mask outward minus inward, 48 px
of the picture each way, Advanced nodes `outer` and `inner`) that the model redraws at the full Strength; the pasted
parts inside it are redrawn at about 0.75 of it, and the rest of the picture at about 0.5, through the Sampler's soft mask
(differential diffusion: gray in the mask is held until later in the run). Before the model sees the picture, the seam band
is blurred (Blur 30 px blended in through the band), because FLUX.2 Klein copies a hard edge that its reference shows,
even at strength 1: softening what it is shown is what lets it blend. Colour Match then moves the result back to the
picture's own colour statistics so the grade does not drift. Strength is the Sampler's noise level, default 0.9 (the
Refine "restyle" preset of 4.7: at 0.7 a blurred ring of the seam band stays visible). Measured only on one made-up
collage (a cat photo pasted on the reading nook as a disc, 1 MP, 3090, about 12 s).
Limits: the whole-picture pass without a mask cannot find seams, so a hard-edged paste stays; a very bright or
differently lit part (a blown window) keeps some of its own light; band widths are in the picture's pixels (use about
four times as much on a 4K picture) and the model works at 1 MP, so a large picture comes back resampled (upscale it
afterwards, or work on a crop).

When a run's layer lands on the head, a Canvas view that is looking at the head (File > Canvas…, or a Canvas area)
goes along to it and says so for a moment, "Layer <name> added", with **Undo** (the head back one step; Ctrl+Z over the
Layers panel does the same). Delete a layer you do not want in the Layers panel. An older workflow that has Canvas
Write's former **Keep as candidate** saved lands on the head all the same: the value is ignored, with no error and no notice.
While an earlier step is shown, the Layers panel says so and offers **Back to latest**: layers are changed on the latest
version.

For scripts (film teams), the store still has candidates, revisions nobody points at, which the command line makes and
reads; the window does not review them:

- `fugue canvas candidates <id>` lists them, newest first.
- `fugue canvas diff <id>@<candidate>` says what a candidate adds: the layer, how many tiles, and
  the rectangle they cover.
- `fugue canvas accept <id>@<candidate>` makes it the head. If something else became the head
  meanwhile and the candidate only added a layer, the layer is placed on the new head as a new
  revision; if the candidate changed more than that, `canvas_conflict` says so.
  `fugue canvas drop <id>@<candidate>` hides it. Neither deletes anything: a dropped one can
  still be accepted, and every revision stays in `fugue canvas log` or `candidates --all`.

In the window, File > Canvas… is the picture of one canvas (the newest at first), its Layers panel with Undo and
Redo, and a **History** panel you can open from the bar: each change in words, newest first ("Layer added: A wooden
table", "Opacity 50%: Fill", "Layer deleted: Sketch"), whoever made it. Click a step to look at the canvas as it was
then (read only) and **Back to latest** to return; **All revisions** lists every revision, the ones kept apart by the
command line too. The picture
zooms with the wheel and pans with the middle button or Space held (Home or 0 fits, 1 is 100%, + and - zoom); a left
drag draws a region. The canvas button opens the
list of your canvases, with **New canvas…** and **Import picture…** under it; with no canvas yet the picture's place
offers the same two. Export… is in File > Export, and **Compare** opens the views of Compare Images (slider, horizontal slider, side
by side, flip and difference) against the step before; the view you pick last comes back next time. The view follows the Canvas
Region node you select in the graph, or by itself the only one that reads this canvas (a node with no canvas chosen reads the **Active canvas**, the one in the Canvas view you used last, so a drag
on one does not change its canvas): its rectangle (and the padded box the node cuts) is drawn on the picture;
A node reads the latest version unless you pinned it to a step; only then a line under the picture says which step and **Use for region** points it at the step you are looking at (the latest version, if you are on it), as one
undo step. With no node followed a drag still draws a rectangle: with several Region nodes the view asks which one it
is for, with none it offers **Add a Canvas Region node** (one undo step); until then the rectangle is not kept. A node
that reads another canvas says so, with **Show** its canvas and **Use** this canvas **for this node**. That rectangle is
the next work area, and you pick it on the picture: drag in empty space to draw a new one, drag inside it to move it
or a handle to resize it (Shift keeps its shape, or makes a new one square; Alt works from the center). The size
and position in the status line, the padded box and its small label (the model's size is in the tooltip of the frame) follow the pointer, and **Snap to 16** (the node's
Multiple of) puts the edges on multiples of 16. Letting go writes X, Y, Width and Height in one undo step, with no
Apply; Esc during a drag leaves the node as it was. The wheel zooms and the middle button (or Space held) pans, and
neither moves the rectangle. It works on whichever revision you are looking at. With X, Y,
Width or Height linked to another node, or while Compare is on, it is only drawn; with no Canvas Region selected the
view says so. The view refreshes by itself when a run moves the head.

Under the picture there is one thin **status line**, as Photoshop's: the zoom and the canvas size ("32% · 2000 × 2000") and, while you drag or when a rectangle is set, its size and place ("456 × 777 at 0, 253"). **Advanced details** at its end opens, for those who want the numbers, the context and the size the model gets, what the node reads, the revision shown and what changed in it. Nothing else of that kind is written under the picture; only what you must act on is (a linked input, Compare on, an error).

The Canvas view's **History** panel (View > History) lists the revisions of the canvas, newest first, and **All revisions** adds the ones kept apart (a candidate the command line made, open or dropped); click one to see it at full size, read only, with **Back to latest** to leave it. **File > Open canvas…** in the view's own menu lists your canvases, and **New canvas…** and **Import picture…** make one. There is no separate history window any more (#228, D1); **Compare** is the view's. **File > Export > Export as…** saves the revision you are looking at: a flattened PNG, JPEG or WebP, or an .ora or .psd with its layers, by the file's extension.

`fugue canvas export <id>[@<revision>] <file>` writes the same flattened picture from the command line by
the file extension: `.png`, `.jpg` or `.webp` (a JPEG has no transparency, so it is flattened onto white
and the command says so; WebP is lossless); `.ora` and `.psd` keep the layers. It never replaces a file.

**File > Export > Export with quality and scale…** (Save for Web) is the same command with a window: PNG, JPEG, WebP or
the layers kept (.ora, or Photoshop .psd); a quality (JPEG, and WebP unless Lossless); a size as 25, 50, 100 or 200 percent or in pixels
(width and height keep the canvas's proportions); a background under the transparency (a JPEG has none, so it
is white unless you choose another color); and a preview of the picture as it will be written, with its size in bytes,
made again as you change a choice. **Export…** asks where and never replaces a file. On the command line the same
choices are `--format`, `--quality 1-100` (`--lossless` for WebP), `--scale 50%`, `--size 800x600`, `--width`,
`--height` and `--background transparent|white|black|#rrggbb`; `--estimate` prints the size and writes nothing.

**File > Export > Export layers as files…** (Layers to Files) writes one picture per layer into a folder you choose, PNG or WebP,
named `<prefix>-<place>-<layer name>`, for example `Harbour-01-Sky.png` (the bottom layer is 01; characters a file name cannot hold become `_`).
You choose whether hidden layers are written, whether each layer's layer mask is applied, and the prefix (the canvas's name at first). A
layer is written as its own pixels: its opacity and blend mode are not applied. A mask layer (a selection) is not a picture and is left out, and the
dialog says so. If any of the names is taken the whole export is refused before the first file is written. On the command line:
`fugue canvas export <id> <folder> --layers [--format png|webp] [--only-visible] [--no-masks] [--prefix <text>]`.

**Canvas > Automate > Batch…** (Photoshop's Automate > Batch) runs a canvas action over every picture of a folder. Choose
the action (one whose models are not on this machine is greyed), type the prompt if it has one, choose the folder of pictures and the
folder for the results, and a format (PNG, JPEG, WebP or the layers as .ora). **Run** makes each picture a canvas, runs the
action on all of it (the whole picture is the selection), writes the result to the results folder under the picture's name
(`harbour.png` becomes `harbour.png` there, or `harbour-2.png` when that name is taken: nothing is replaced) and removes
the temporary canvas. It runs in the background, one picture at a time, through the same queue as every run, so it waits
for the GPU and shows in the queue panel. The dialog lists each picture as Waiting, Running, Done, Failed (with the reason)
or Cancelled, and a progress bar; a picture that fails never stops the others. **Cancel batch** ends the run under way and marks
the pictures not started as cancelled; **Hide** closes the dialog and leaves the batch running (reopen it to see where it is).
An action that works on a crop frame beyond the canvas (Expand) cannot be batched.

`fugue canvas batch <action> <in-folder> <out-folder>` is the same without a window: `<action>` is an action file or the id of a
bundled one, `--prompt <text>`, `--seed <n>` and the picture options of `canvas export` (`--format`, `--quality`, `--scale`,
`--size`, ...) apply, `--keep` leaves each temporary canvas in the store with the result as its head, and the command prints one line
per picture (one JSON event per line with `--json`). Its exit code is 1 when any picture failed or was cancelled.

## Layers

Every run that writes to a canvas lands as a layer, like a layer in Photoshop or Krita, and you can
hide it, change its opacity or blend mode, rename, delete, duplicate or reorder it. None of that edits
the canvas: each change is a new revision of the head (the pixels are not copied; the new revision
shares them), so Undo goes back one change and the history keeps every step.

From the command line, a layer is its position (0 is the bottom, `fugue canvas layer list` shows them)
or its name:

```
fugue canvas layer list canvas-0123456789ab
fugue canvas layer set canvas-0123456789ab 2 --visible false
fugue canvas layer set canvas-0123456789ab "Fill 1" --opacity 0.6 --blend screen   # two revisions
fugue canvas layer rename canvas-0123456789ab 2 Sky
fugue canvas layer dup canvas-0123456789ab Sky          # "Sky copy", right above it
fugue canvas layer move canvas-0123456789ab "Sky copy" --to 0   # to the bottom
fugue canvas layer rm canvas-0123456789ab 3
fugue canvas layer set canvas-0123456789ab Sky --lock true      # protect it (--lock false lets go)
fugue canvas layer merge canvas-0123456789ab "Fill 2"           # into the layer under it
fugue canvas undo canvas-0123456789ab
fugue canvas redo canvas-0123456789ab
```

The blend modes are Normal, Multiply, Screen, Overlay, Soft light, Darken, Lighten and Add, the same
as Krita's and GIMP's: an `.ora` you export keeps them, and one you import from Krita or GIMP keeps
its own (a mode Fugue does not draw yet, such as Color Dodge, is drawn as Normal and kept as it was).

A layer can have a **layer mask**: white shows the layer, black hides it, grey in between. It is part
of the layer (it moves and is deleted with it) and can be turned off to see the layer whole
(`--mask off`, `--mask on`) or deleted (`--mask delete`). Other programs have no layer mask in an
`.ora`, so an export gives them the layer as it looks with its mask applied, and keeps the mask for
Fugue to read back. A `.psd` does have layer masks, so a Photoshop file keeps the layer's own pixels and its mask apart.

**Photoshop files (.psd, #227).** `fugue canvas export <id> out.psd` (or Photoshop (.psd) in Export with quality and
scale…) writes every paint and picture layer and every group (as a layer group) with its name, visibility, opacity, blend mode and layer mask, its own
pixels unblended, and the merged picture; 8-bit RGB, deterministic, at most 30000 px a side (more is refused in plain
words). A mask layer is a selection, not a picture, so it is left out and the answer names it. `fugue canvas import
x.psd` (and Import picture…, New canvas… from a file) reads such a file back and lists, one warning each and never
silently, what it could not keep: groups that are not plain (a group that holds only layers is a real group again; any other has
its layers kept in order with its visibility and opacity folded in), adjustment layers (dropped), text and smart objects (kept as pixels), layer effects, vector masks and clipping
(dropped), blend modes Fugue lacks (shown as Normal; the ones with an OpenRaster name are kept for exporting). It reads
8-bit RGB only and refuses 16-bit, CMYK, grayscale and `.psb` with the way to convert them. Details, limits and the
checklist for trying it in real programs: [psd-research.md](psd-research.md).

In the window, the Canvas view (File > Canvas…, or a Canvas area) has a **Layers** panel at the right
of the picture, top layer first, with a small picture of each layer (and of its mask), its name and
an eye:

- Click the eye to hide or show a layer; **Alt+click** it to look at that layer alone (only a view:
  nothing changes, and the strip "Showing … alone" ends it).
- Click a layer to select it, Ctrl+click to add one, Shift+click for a range. Under the list,
  **Opacity** is a field you drag left or right (Shift for fine steps, Ctrl for steps of 10%), click
  to type a value, or double-click for 100%; **Blend** picks the blend mode; a layer with a mask has
  **Mask on/off** and **Delete mask**, and Shift+click on its mask picture does the same.
- Double-click a name (or F2) to rename it; drag a layer up or down to reorder; **Duplicate**, the
  arrows and **Delete** are under the list, and a right click on a layer opens the same with their
  keys.
- While the pointer is over the panel: H hides or shows, X or Delete deletes, Ctrl+D duplicates,
  `]` and `[` move up and down, F2 renames, Ctrl+Z and Ctrl+Shift+Z undo and redo. Elsewhere those
  keys keep their meaning in the graph.
- Each change is a new revision of the head and shows at once; **Undo** and **Redo** at the top of
  the panel go back and forward. Layers are changed on the head only: on an older revision the list is there to look at.
- The lock beside the eye (Ctrl+/) protects the layer: a **locked layer** cannot be deleted, merged (as the upper or the lower
  one), flattened, moved or transformed, or have its layer mask painted, replaced or deleted. Showing, opacity, blend, a name,
  order and Duplicate stay open. Locking is a revision like the rest, so Undo takes it back, and the lock is kept in an `.ora` (and a
  `.fuguepack`). The layer list says "locked" in `fugue canvas layer list`.
- **Generate reads this layer only** (the row's menu; again on the same layer, or the x in the notice, goes back) makes the
  task bar's Generate read that layer's own pixels, as Canvas Region's **Layer** input does, instead of the whole picture. It is a
  view state, like Show alone, not a revision; it ends with the canvas, and when the layer is renamed or deleted. Only a paint layer
  whose name no layer above shares can be read this way.
- Hover a layer to see where it came from (the Canvas Write that made it, and when).

## Tools, masks and Quick Mask

The Canvas view has a narrow column of tools at the left of the picture, with Photoshop's keys (they work while the
pointer is over the view; over the Layers panel the panel's own keys win). The tooltip of each says its key:

- **Hand** (H, or hold Space with any tool): drag the picture to pan. **Zoom** (Z): click to zoom in, Alt+click to
  zoom out. The wheel zooms and the middle button pans with every tool.
- **Rectangle region** (M or R), the tool you start with: the region rectangle described above.
- **Mask brush** (B) and **eraser** (E; X swaps them): paint a layer mask, or the Quick Mask selection. `[` and `]`
  make the brush smaller and larger; Alt+drag on the picture sizes it (left and right) and sets its hardness (up and
  down). Size, Hardness and Opacity are in the bar over the picture.
- **Quick Mask** (Q), at the foot of the column.

The bar over the picture shows the current tool's settings and what the brush paints.

**Paint a layer mask.** Select a layer and press **Add mask** (under the Layers list, in the layer's right-click menu,
or Ctrl+Shift+M): the layer gets a mask that shows it all, and the brush takes it ("Mask of <layer>" in the bar; the
mask's thumbnail gets a frame). A click on the mask thumbnail of a layer that has one does the same. The brush hides
the layer where you paint and the eraser shows it again; the hidden part is red while **Overlay** is on (\ turns it
off to see the result). The strokes you make in a row are one step: when you stop for a moment they become a new
revision of the canvas (History names it "Painted: <layer>"), and Ctrl+Z takes it back. The `⋯` menu has Invert
(Ctrl+I), Reveal all and Hide all, one step each.

**Paint the area to change (Quick Mask).** Press Q, or the Quick Mask button: the same brush now paints a selection, in
red, which no revision keeps (Ctrl+Z undoes a stroke). With a Canvas Region node followed, **Use selection** gives it the
selection as the area to change, as one undo step of the workflow: the next run redraws the painted part, and its
result lands as a layer. Drawing a rectangle afterwards replaces it. Press Q again to leave Quick Mask; the selection
stays until you clear it (**Select > Deselect**, Ctrl+D, or Escape) or change canvas.

Canvas Region's **Selection** input (advanced) is what Use selection writes: the hash of the stored selection. With
it, the Mask input and the rectangle are not used; the selection's bounds are in X, Y, Width and Height so the frame
and the size the model gets read right. A replay of the run reads the same selection.

## Modify and transform a selection

**Select > Modify** and **Select > Transform selection…** (#251) change the selection by a number you type, with Photoshop's
meanings. Each asks in a small dialog (Enter applies, the last amount is offered again) and is one step of the selection's own undo
(Ctrl+Z in the view). They need a selection: the painted one, or a rectangle (the free one, or the Region node's), which is
painted into the selection first (the free rectangle goes; the Region node's area stays until Deselect). The result shows as the
Quick Mask, the one view a painted selection has until the marching ants (#232) come.

- **Expand** and **Contract**: every edge moves out or in by N pixels (round corners). A selection against the picture's edge is not eaten from that side.
- **Feather**: the edge fades over about N pixels each side of where it was (a Gaussian). A selection reaching the picture's edge stays solid there.
- **Border**: a band N pixels wide centered on the edge (an odd width puts the extra pixel outside); the inside is let go.
- **Smooth**: blurs by N and keeps what is more than half selected: rounds corners and drops specks.
- **Transform selection**: Scale in percent and Rotate in degrees about the selection's center, then Move right and down in pixels. There are no handles to drag yet, and no live preview: Undo takes it back. What leaves the picture is lost.

An amount is 1 to 250 pixels. The work is `engine/mask_ops.h`: Expand and Contract run the Grow Mask node's own 3x3 pass (`morphPass`, shared), alternating the tapered and the full footprint so the growth looks round (the node alone makes a diamond).

## The menu bar

The Canvas view has its own menu bar at the top, in the File > Canvas… dialog and in an area alike (#232). Every option of
the view is a row of a menu, with its key beside it; what does not apply to the canvas now is greyed.

- **Canvas** (it was File; the window's File is the workflows', #262): New canvas…, Open canvas… (the picker), Import picture… (also a `.psd` or a `.fuguepack`, below), **Export** (Export as…: PNG, JPEG, WebP flattened, .ora or .psd
  with layers, by the extension; Export with quality and scale… and Export layers as files…, #247 and #248, above; never replaces a file), **Share…** (asks, then where: a `.fuguepack` of the canvas as it is now, or with its history; see Sharing a canvas), Rename canvas…, Duplicate canvas (the whole history, under a new
  id; the copy is shown), **Clear canvas…** (asks; one undoable step that leaves one empty layer of the same size),
  **Delete canvas…** (asks; permanent; the next canvas is shown, or the empty state) and Close canvas (the dialog closes; an
  area shows no canvas).
- **Edit**: Paste as new layer, Paste as new canvas and Free transform (below), and Undo and Redo of the canvas's own history, as the keys do. It stays here, and not only in the window's Edit, because the canvas has its own history and the File > Canvas… dialog covers the window's bar.
- **Select**: Select all (Ctrl+A: the whole canvas as the rectangle, or the whole Quick Mask), **Deselect** (Ctrl+D, and
  Escape when nothing is being dragged or run: it clears the free rectangle, sets the Canvas Region node's area back to 0, 0, 0, 0 = the
  whole canvas as one undo step, and clears a painted selection), Inverse (Ctrl+Shift+I, the painted selection), Quick Mask
  and Use selection, **Modify** and **Transform selection** (below).
- **Layer**: Add layer mask, Invert mask, Mask overlay, Reveal all, Hide all. The layers panel keeps its own rows.
- **View**: Fit, 100%, Zoom in, Zoom out, History, Layers, Compare, Advanced details, Snap region, and
  **Tools** (Hand, Zoom, Rectangle region, Mask brush, Mask eraser, swap, brush size); then the rulers, grid and guides below.

**Rulers, grid and guides (#253).** View > Rulers (Ctrl+R) puts a ruler along the top and the left of the picture, in the
canvas's own pixels; it follows zoom and pan. Drag from a ruler onto the picture to pull out a guide (from the top ruler a
horizontal one, from the left a vertical one; its position shows in a chip while you drag). Drag a guide to move it; drag it
back onto a ruler, or off the picture, and let go to remove it (View > Clear guides removes all). View > Guides (Ctrl+;)
shows or hides them, View > Grid (Ctrl+') shows a grid, and while it is on the options bar has a **Grid** spacing field (the
default is 64 px; the grid is not drawn, and does not snap, while its lines would be closer than 6 screen pixels).
View > **Snap to guides and grid** (Shift+Ctrl+;) pulls the Rectangle region's edges (the selection) to a guide or grid
line that is shown and within 8 screen pixels; a move keeps the rectangle's size and follows the edge nearer a line. It works
with, and wins over, **Snap to 16** (the node's Multiple of) when a line is close. Guides and the grid's spacing belong to the
canvas: they are kept in `guides.json` in the canvas's folder, not in a revision (no history step, Undo does not take them
back; Duplicate canvas copies them), and a damaged file reads as no guides. Guides, grid and rulers are shown only for one
picture, not while comparing. A press within 6 pixels of a guide with the Rectangle region or Hand tool takes the guide, not
the frame.

**Paste and place a picture (#236).** Edit > **Paste as new layer** (Ctrl+V while the Canvas view is the one in use: the pointer is
over it or it has the focus, as for every key of the view; the Graph's Ctrl+V still pastes nodes) puts the clipboard's picture on the
canvas as a new layer above the current one (on top when none is picked), centered in what the view shows of the canvas, named
"Pasted 1", "Pasted 2"... A picture file dropped on the picture, or Canvas > **Place picture…**, does the same and names the
layer after the file. Each is ONE revision (producer `canvas-view:paste`, history "Layer pasted: Pasted 1"); Undo takes it back.
Edit > **Paste as new canvas** makes a canvas of the clipboard's picture (the same as File > Paste as new canvas). The picture is
**kept whole**: one bigger than the canvas is shown cut at the canvas's edge, and its off-canvas part stays in the layer's original
(below), so it can be scaled down later. A picture comes from outside and is checked first: at most 16384 px a side and 64
megapixels (the canvas's own limit), a file at most 256 MB and only a PNG, JPEG, WebP, GIF, BMP or TIFF by its name, its size read
from the header before anything is decoded; a refusal says why under the picture and changes nothing.

**A layer's original (#236).** A pasted picture, and a layer after a transform (below), keeps the pixels it started from: the layer
record has an optional `"source"` ({width, height, tiles, an optional mask's tiles, the placement (cx, cy, width, height, angle,
flipX, flipY, filter) and `rendered`}), written only for a layer that has one, so every tree from before keeps its hash. The
source's tiles are blobs like any other (shared by content, carried by a package and a duplicate). The layer's own tiles stay the
picture a revision SHOWS (the original placed, cut at the canvas), so the composite, nodes, a flattened export, **.ora and .psd read the
shown layer as they always did and never see the original**: a file exported and opened elsewhere has the transformed pixels, canvas
sized, with the layer's mask; the original stays in Fugue (and in a `.fuguepack`). `rendered` is a hash of the shown tiles and mask
the placement produced: a layer painted on, or given another mask, since then no longer matches it, and its next transform starts
from the layer as it is then (Photoshop's rasterise-on-paint).

**Transforming a layer (#236).** Layer > **Transform** has Flip horizontal, Flip vertical, Rotate 90° clockwise and Rotate 90° counterclockwise for the
current paint layer (a group, a mask layer and a picture layer say in words that they cannot be moved or transformed: transform the group's layers, or
merge it into one layer first). A layer is drawn from its **original**: a pasted picture's own pixels; for a plain layer, the first transform takes
the smallest rectangle that holds every pixel that is not transparent (Photoshop's Free Transform box) as the original and draws it again, its
layer mask cut to the same rectangle and **transformed with it** (where the layer has nothing the mask hides nothing). Every later transform
starts again from the original, so scaling a layer down to a tenth and back to its size gives the original pixels exactly, and a picture
bigger than the canvas that was scaled down shows all of it. The picture is resampled on premultiplied alpha (a transparent edge leaks no color) with
**Lanczos** (the default) or **bicubic**, their support widened when shrinking, or **nearest** (for pixel art; a turn then has no soft edge);
a quarter turn and a flip lose nothing, any other angle is resampled a second time at the turned positions with a one pixel soft edge. Only the part the
canvas shows is computed. A layer may reach past the canvas edge: the layer's tiles are cut there (the canvas is the picture), the original keeps the
rest; a layer is at most 32768 px a side and 16384 x 16384 px. Each transform is ONE revision (producer `canvas-view:transform`; History says
"Layer moved: Fill", "Layer scaled: …", "Layer rotated: …", "Layer flipped: …", or "Layer transformed: …" for several at once). A layer painted on, or
given another mask, since its last transform has no current original: its next transform starts from the layer as it is. Growing the canvas
(Canvas size) moves the original with the layer. The sizes and the placement are kept to a thousandth. Not yet: transforming a group, a mask layer
or a selection together with a layer, and a command line verb (`fugue canvas layer transform`).

**Move and Free Transform (#236).** The **Move** tool (V, in the tool strip under Zoom) moves the current layer: a drag anywhere on the picture, and the
layer is drawn at its new place while the canvas under it has the layer left out; when the button is let go it is ONE revision ("Layer moved: Card").
Shift holds one axis. The arrow keys nudge it one pixel, with Shift ten, each a revision, while the picture has the focus. **Edit > Free transform**
(Ctrl+T, any tool) opens the layer's box: eight handles (a corner or an edge scales from the opposite one, the box's own edges when it is turned), a drag
inside moves it, and a drag just outside a corner turns it about its center. **Shift** keeps the proportions (and turns by 15° steps, and moves along one
axis), **Alt** works from the center, and the size never goes under 1 px or flips (Layer > Transform flips). The options bar has X and Y (the center),
W and H (with a **Link** toggle that scales the other side along when one is typed), Angle, the resampling (Lanczos, Bicubic, Nearest) and Apply and
Cancel; the numbers and the drag are the same box. **Enter** or Apply keeps it as ONE revision, **Escape** or Cancel leaves the layer as it was; another
tool picked, or Apply with nothing changed, writes nothing extra (a box nobody changed makes no revision), and another layer or canvas ends the box.
**Snapping** uses the view's guides and grid (View > Snap to guides and grid, #253) through the same targets as the Region frame: a move is pulled by the
nearest of its bounds' edges and middle, a handle drag of an unturned box by the edge it moves; nothing snaps with Shift, Alt or a turned box. The pull
is not clamped to the canvas (a layer may reach past it; `snap::snapFloating`). Only a paint layer can be moved or transformed: a group, a mask layer
and a picture layer say so in words and nothing opens. The preview shows the layer without its layer mask and blend; the revision has both.

A greyed row with the tooltip "Not built yet (#N)" is a feature that is planned and tracked in issue N; it is not a command and has
no key ([the research](canvas-menus-research.md) says what the other editors have and what was decided). The top bar keeps the
canvas picker, Fit and 100%; each tool's own settings stay in the options bar.

## The task bar: Generate on the canvas

You can change the canvas without opening a workflow (#231, [design](canvas-actions-design.md)). Draw a rectangle on
the picture, or paint a selection with Quick Mask, and a bar appears under it (above it when there is no room; across
the foot of the picture in a small area):

- the **action** menu: **Auto** (picks the action from the selection and the prompt) and the actions: every template
  whose workflow carries an `action` block (the bundled Generative fill, and any of your own or your project's), listed
  again each time the menu opens;
- the **prompt**: type what should be in the selection and press Enter (Ctrl+Enter is still Run of the Graph). Up and
  Down in an empty field bring back earlier prompts; the clock button lists the last 20;
- **Generate**, which becomes **Cancel** while it runs (Escape too); a cancelled run adds nothing;
- `⋯`: **Edit workflow** (opens the chosen action's graph as a new tab of the Graph and shows the Graph; Save asks where,
  and a bundled action is never written; from the File > Canvas… dialog, which hides the Graph, the dialog closes) and
  **Clear prompt history…** (it asks first).

While it runs, the selection's outline moves and the bar says "Generating… 45%". The result is a layer on the latest
version, named from the prompt, with the selection's soft edge as its layer mask; Undo takes it back. Generate works on
the latest version only: on an older step the bar says so and offers **Back to latest**. When something goes wrong the
bar says it in one line ("Not enough GPU memory. Try a smaller selection."). On first use it says which models are
missing and how large they are ("Generative fill needs 3 models, 8.9 GB.") with **Download**, which goes through the
window's download line; Generate stays off until they are here.

An action is an ordinary workflow with an `action` block (see the design): the bar copies it, puts in the canvas, the
latest version it showed (by full hash), the selection (a rectangle, or the painted selection stored by content), the
prompt and a random seed, takes the Canvas Region's padding and scale from the router
([research 6](inpaint-outpaint-research.md#6-the-router)), and queues it as an ordinary run: it shows in the queue
panel under the action's name and waits for the GPU like any run. The bundled Generative fill
(the `generative-fill` template of fugue-workflows, category Canvas actions; `share/actions/generative-fill.fugue` is the same
workflow compiled into Fugue, used only when the library lacks it) is the recommended workflow of the research: FLUX.2 Klein
4B Q8, 4 steps, the selection redrawn near 1 MP with the crop as its reference, a Blend of 16. If you generate again
with exactly the same prompt, seed and selection on the same version after an Undo, the cached result is placed again.
The prompts you type are kept per project, 200 per action, and never shared with another project.

### Refine, Remove and Expand

Three more actions are in the action menu (templates `refine`, `remove` and `expand`, category Canvas actions; same
model, FLUX.2 Klein 4B Q8, 4 steps, as Generative fill):

- **Refine** redraws the selection lightly, keeping its shapes and colours (strength 0.7: a noise level, 0.55 polishes,
  0.9 restyles). The prompt is optional: left empty it asks to refine the details. The strength is the action's Advanced
  input (the Advanced popover comes later; until then it is the workflow's value, which Edit workflow changes).
- **Remove** takes out what is selected and fills it from its surroundings: no prompt, a fixed instruction, the selection
  blurred into the surroundings first and at least 128 px of them in view. For hard removals a LaMa pack
  ([Remove Object](../examples/packs/io.github.bmaltais.fugue-remove-object)) can do that first fill.
- **Expand** grows the canvas, then continues the picture into the new border. It needs no selection: the bar shows a
  **Grow the canvas** row with the side (Left, Right, Top or Bottom) and how much (25% or 50% of the picture's size on that
  side). Generate first grows the canvas (one step in History, Undo takes it back; the new area is pre-filled from the edge
  pixels as a layer called "Expanded area") and then fills the new strip plus 32 px of the old picture, with 256 px of the
  picture beside it in view. One side per run; the prompt is optional (a description of the whole scene helps). If the run
  fails or is cancelled the canvas stays grown: Undo.

## Sharing a canvas

A canvas travels in a `.fuguepack`, the same kind of file as a shared workflow (spec 11.5), so a teammate opens it with Fugue.
**Canvas > Share…** (the Canvas view) asks what goes in: **As it is now…** (the layers, their masks and properties, with no
history: one revision) or **With history…** (the head and every revision it came from, so the other person can undo your
steps). Candidates and what an Undo left for Redo stay out. `fugue canvas export <id> <file>.fuguepack [--history]` does the
same; the file is never written over another.

Opening a package that carries a canvas (File > Open, a double click, **Import picture…** in the Canvas view or
`fugue canvas import <file>.fuguepack`) imports it as a canvas of your own, under its name or the `--name` you give. It never
replaces a canvas of yours: when the package's id is one you already have, the canvas gets a new id and the notice says so.
A package is untrusted input: its files are read only by the hashes its manifest lists, never by their own names or paths;
every revision, tree and tile must be what its hash says and every tile the size its place gives, or nothing is imported
(`canvas_corrupt`); names outside the package's folder, links, and files or packages past the size limits are refused
(`invalid_document`), and all of it is checked before a canvas appears in your store. Layout and checks: `src/engine/canvas_package.h`.
