# Canvases

A canvas is a layered picture that Fugue keeps as a chain of revisions: a change never edits a
canvas in place, it makes a new revision and the old ones stay. The canvas work
([epic #206](canvas-research.md)) so far is the store, the command line, the two nodes that read a
region from a canvas and write a result back, and the review of what they wrote. The design and its reasons
are in [spec 14.5](spec.md#145-the-canvas-revision-store-206-208).

```
fugue canvas new --name Sketch --size 1024x768     # prints the canvas id, canvas-0123456789ab
fugue canvas import photo.png                      # a canvas from a PNG, JPEG or WebP (one layer) or an .ora
fugue canvas log canvas-0123456789ab               # the revisions, newest first
fugue canvas show canvas-0123456789ab@sha256:ab12cd34   # one revision: tree hash, layers, tiles
fugue canvas export canvas-0123456789ab out.ora    # the head as an .ora (never replaces a file)
fugue canvas export canvas-0123456789ab@head out.png  # flattened: .png, .jpg (on white) or .webp
fugue canvas list                                  # the canvases, newest first
fugue canvas candidates canvas-0123456789ab        # results waiting for your review
fugue canvas diff canvas-0123456789ab@sha256:ab12cd34   # what a revision changed, against its parent
fugue canvas accept canvas-0123456789ab            # the one candidate becomes the head
fugue canvas drop canvas-0123456789ab@sha256:ab12cd34   # hide one (it is kept)
fugue canvas layer list canvas-0123456789ab        # the layers, top first, with their positions
fugue canvas layer set canvas-0123456789ab "Fill 1" --blend multiply   # one new revision
fugue canvas undo canvas-0123456789ab              # back to the revision before; redo goes forward
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

- **Canvas Region** takes a rectangle (X, Y, Width, Height; 0 runs to the edge) or a mask you connect,
  adds Padding around it, and gives you that part of the canvas as a picture, the mask of the part to
  change, and a stitch. Scale and Cap set the size it is redrawn at (a small region is drawn larger,
  up to the megapixel cap); Feather softens the mask's edge outward. Its Revision is `head` unless you
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
makes a new revision from the one Region read and moves the head to it only if nothing else moved the
head meanwhile. If something did, the new revision is kept as a candidate, the node says so with the
warning `canvas_head_moved`, and the head stays as the other change left it. The next run reads the
head as it has become. To keep reading one revision (to try several fills of the same picture), give
Region that revision's hash, or freeze it.

Region's picture keeps the canvas's transparency. A stitch from Region also goes to Stitch. Region
reads the canvas as it looks (the visible layers, with their opacity, blend modes and masks); its
**Layer** input (advanced) reads one layer by name instead, its own pixels.

In the window, Canvas Region's **Canvas** is a drop-down of the canvases of the folder you are working in
(the Default's, or the project's own: a canvas belongs to the one it was made in). Each row has the name,
the size, the time of the last change and the id. **New canvas…** asks for a name and a size
(1024 × 1024 to start; at most 16384 a side and 64 megapixels), or makes one from a picture file with
**From a picture…**; **Import picture…** makes a canvas straight from a PNG, JPEG, WebP or `.ora`. The
canvas is put in the input. The input still holds the id (a workflow file is the same as before). An id this
folder has no canvas by, such as one copied from the Default into a project, shows as `canvas-… (missing)`
and the list is there to pick the right one. The list follows `fugue canvas new` and runs as they happen.

**Pick region…**, the last entry of that list, opens the canvas at the node's revision (`head` is read as it is now) in a large view with a rectangle on it, so the region is dragged out instead of typed. Drag inside the rectangle to move it, drag a handle to resize it (Shift keeps its shape, Alt resizes from the center), or type X, Y, Width and Height under the picture; Free, Original, 1:1, 4:3, 3:2, 16:9 and Turn lock its shape, and **Snap to 16** (the node's Multiple of) puts its edges on multiples of 16. The dashed outer box is what the node will cut (the rectangle and its Padding), and the line under the picture says the size the model will get (Scale, Cap and Multiple of applied, as the node does). Zoom and pan work as in every large view; a large canvas is drawn from a smaller copy, the numbers stay in the canvas's pixels. **Apply** writes X, Y, Width and Height in one step (Undo takes all four back); Cancel or Escape change nothing. With a mask connected the node uses the mask's bounds and the window says so; with X, Y, Width or Height linked to another node there is nothing to apply.

## Review a fill

A fill does not have to change the canvas at once. Canvas Write with **Keep as candidate** on leaves
the head where it is and stores the new layer as a **candidate**: a revision made from the one Region
read, waiting for you. The `generative-fill` template (File > Browse templates, or
`fugue workflow new fill.fugue --template generative-fill`) is Canvas Region, a FLUX.2 Klein redraw
and Canvas Write with that on. Put your canvas's id and the rectangle in Canvas Region and run it.

When a run leaves a candidate, a Canvas view that is looking at the head (File > Canvas…, or a Canvas
area) shows it at once over the head, with a banner: **Accept**, **Drop** or **Keep looking at head**.
Several results at once: the newest is shown and the others are counted. A view on another revision,
with Compare on, or while you drag the region is never moved (a result that came during a drag shows
when you let go). Turn it off in Settings > General, **Show new canvas results**. To skip the review
altogether, turn Canvas Write's **Keep as candidate** off: the head then moves to the result by itself.

- `fugue canvas candidates <id>` lists what waits, newest first; run the template again for another
  candidate to compare (they are all made from the head you started at).
- `fugue canvas diff <id>@<candidate>` says what a candidate adds: the layer, how many tiles, and
  the rectangle they cover.
- `fugue canvas accept <id>@<candidate>` makes it the head. If something else became the head
  meanwhile and the candidate only added a layer, the layer is placed on the new head as a new
  revision; if the candidate changed more than that, `canvas_conflict` says so and you run the fill
  again. `fugue canvas drop <id>@<candidate>` hides it. Neither deletes anything: a dropped one can
  still be accepted, and every revision stays in `fugue canvas log` or `candidates --all`.
- A Write that lost the race for the head (`canvas_head_moved`) is a candidate too.

In the window, File > Canvas… is the picture of one canvas (the newest at first): the strip above it says what
you are looking at (Head, the candidates waiting for you, the latest revisions, Earlier…), and the picture
zooms with the wheel and pans with a drag. A candidate shows over the head; **Show candidate** off shows the
head alone, so you can flip between before and after. **Accept**, **Drop** and **Export…** are in the bar
(Drop only hides: a dropped candidate can still be accepted), and **Compare** opens the slider, side by side,
flip and difference views against the head. Select a Canvas Region node in the graph before you open it and its
rectangle (and the padded box the node cuts) is drawn on the picture; **Region reads from this revision** makes the
node read the revision you are looking at (the head, if you are on the head), as one undo step. That rectangle is
the next work area, and you pick it on the picture: drag in empty space to draw a new one, drag inside it to move it
or a handle to resize it (Shift keeps its shape, or makes a new one square; Alt works from the center). The numbers
under the picture, the padded box and the size the model will get follow the pointer, and **Snap to 16** (the node's
Multiple of) puts the edges on multiples of 16. Letting go writes X, Y, Width and Height in one undo step, with no
Apply; Esc during a drag leaves the node as it was. The wheel zooms and the middle button (or Space held) pans, and
neither moves the rectangle. It works on whichever revision you are looking at, a candidate included. With X, Y,
Width or Height linked to another node, or while Compare is on, it is only drawn; with no Canvas Region selected the
view says so. The view refreshes by itself when a run adds a candidate or moves the head.

File > Canvas history… lists your canvases; open one to see its revisions and
candidates, and click a revision to see it at full size (zoom and pan; the head is shown first). Press
**Compare** to see it against the one before it (the same slider, side by side, flip and difference views
as Compare Images), and **Accept** or **Drop** on a candidate. Ctrl+click a revision to use it as the left
side of a comparison. **Export…** saves the revision you are looking at as a flattened PNG.

`fugue canvas export <id>[@<revision>] <file>` writes the same flattened picture from the command line by
the file extension: `.png`, `.jpg` or `.webp` (a JPEG has no transparency, so it is flattened onto white
and the command says so; WebP is lossless); `.ora` keeps the layers. It never replaces a file.

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
Fugue to read back.
