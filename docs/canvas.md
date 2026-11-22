# Canvases

A canvas is a layered picture that Fugue keeps as a chain of revisions: a change never edits a
canvas in place, it makes a new revision and the old ones stay. The canvas work
([epic #206](canvas-research.md)) so far is the store, the command line, the two nodes that read a
region from a canvas and write a result back, and the review of what they wrote. The design and its reasons
are in [spec 14.5](spec.md#145-the-canvas-revision-store-206-208).

```
fugue canvas new --name Sketch --size 1024x768     # prints the canvas id, canvas-0123456789ab
fugue canvas new --size 1920x1080 --background white   # filled: transparent (default), white, black or #rrggbb
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
makes a new revision from the one Region read and moves the head to it: the fill is a layer on the
canvas at once, to hide, edit or delete (Undo removes it too). If something else moved the head
meanwhile, the layer is added onto the head as it is now, as a new revision. Only if that is
impossible (the canvas changed size) is the new revision
kept as a candidate, with the warning `canvas_head_moved`, and the head stays as the other change
left it. The next run reads the head as it has become. To keep reading one revision (to try several fills of the same picture), give
Region that revision's hash, or freeze it.

Region's picture keeps the canvas's transparency. A stitch from Region also goes to Stitch. Region
reads the canvas as it looks (the visible layers, with their opacity, blend modes and masks); its
**Layer** input (advanced) reads one layer by name instead, its own pixels.

In the window, Canvas Region's **Canvas** is a drop-down of the canvases of the folder you are working in
(the Default's, or the project's own: a canvas belongs to the one it was made in). Each row has the name,
the size, the time of the last change and the id. **New canvas…** opens the New canvas dialog (below); **Import picture…** makes a canvas straight from a PNG, JPEG, WebP or `.ora`. The
canvas is put in the input. The input still holds the id (a workflow file is the same as before). An id this
folder has no canvas by, such as one copied from the Default into a project, shows as `canvas-… (missing)`
and the list is there to pick the right one. The list follows `fugue canvas new` and runs as they happen.

### New canvas

**File > New canvas…** (Ctrl+Alt+N) opens Photoshop's New Document dialog, from anywhere in the window; Canvas Region's **New canvas…** opens the same one. Give a name (Untitled 1, Untitled 2, … never one this folder already has) and say where the canvas starts:

- **Blank**: pick a preset (Square 1024 × 1024, Landscape 4:3, Wide 16:9, Portrait 3:4, HD, 4K) or type a width and a height, then a background: Transparent, White, Black or a Color. A canvas is at most 16384 px a side and 67 megapixels; the line under the fields shows the size in megapixels and says when it is too much. A transparent canvas is one empty layer, any other background one filled layer called Background.
- **Clipboard**: when you have copied a picture, the dialog opens on it with its size filled in, and Create makes the canvas from it (a screenshot, a picture copied from a browser). **File > Paste as new canvas** skips the dialog: it makes the canvas at once, named Pasted 1, Pasted 2, ….
- **A file…**: a PNG, JPEG, WebP or `.ora` file, as Import picture… does, named after the file.

The size, background and color of the last canvas you made are there the next time. A canvas made from the File menu opens in the Canvas view (in the active Canvas area when there is one).

**Pick region…**, the last entry of that list, opens the canvas at the node's revision (`head` is read as it is now) in a large view with a rectangle on it, so the region is dragged out instead of typed. Drag inside the rectangle to move it, drag a handle to resize it (Shift keeps its shape, Alt resizes from the center), or type X, Y, Width and Height under the picture; Free, Original, 1:1, 4:3, 3:2, 16:9 and Turn lock its shape, and **Snap to 16** (the node's Multiple of) puts its edges on multiples of 16. The dashed outer box is what the node will cut (the rectangle and its Padding), and the line under the picture says the size the model will get (Scale, Cap and Multiple of applied, as the node does). Zoom and pan work as in every large view; a large canvas is drawn from a smaller copy, the numbers stay in the canvas's pixels. **Apply** writes X, Y, Width and Height in one step (Undo takes all four back); Cancel or Escape change nothing. With a mask connected the node uses the mask's bounds and the window says so; with X, Y, Width or Height linked to another node there is nothing to apply.

## Review a fill

A fill lands on the canvas at once; you do not have to review it. When you do want a review step,
turn Canvas Write's advanced **Keep as candidate** on: it leaves the head where it is and stores the new layer as a **candidate**: a revision made from the one Region
read, waiting for you. The `generative-fill` template (File > Browse templates, or
`fugue workflow new fill.fugue --template generative-fill`) is Canvas Region, a FLUX.2 Klein redraw
and Canvas Write, with that off. Turn it on for the review below. Put your canvas's id and the rectangle in Canvas Region and run it.

When a run's layer lands on the head, a Canvas view that is looking at the head (File > Canvas…, or a Canvas area)
goes along to it and says so for a moment, "Layer <name> added", with **Undo** (the head back one step; Ctrl+Z over the
Layers panel does the same). Delete a layer you do not want in the Layers panel.

When a run leaves a candidate instead (Keep as candidate on, or a head that moved in a way the layer could not follow),
the view shows it at once over the head, with a banner over the foot of the picture, "New candidate <hash> from <the
Canvas Write's title>": **Back to latest** (the candidate stays pending), **Accept** (or Enter) or **Drop**. Several
candidates at once: the newest is shown and the others are counted. A view on another revision, with Compare on, or
while you drag the region is never moved (a candidate that came during a drag shows when you let go). Turn it off in
Settings > General, **Show new candidates**. While a candidate or an earlier revision is shown, the Layers panel says
so and offers **Back to latest** (and, for a candidate, Accept and Drop): layers are changed on the latest version.

- `fugue canvas candidates <id>` lists what waits, newest first; run the template again for another
  candidate to compare (they are all made from the head you started at).
- `fugue canvas diff <id>@<candidate>` says what a candidate adds: the layer, how many tiles, and
  the rectangle they cover.
- `fugue canvas accept <id>@<candidate>` makes it the head. If something else became the head
  meanwhile and the candidate only added a layer, the layer is placed on the new head as a new
  revision; if the candidate changed more than that, `canvas_conflict` says so and you run the fill
  again. `fugue canvas drop <id>@<candidate>` hides it. Neither deletes anything: a dropped one can
  still be accepted, and every revision stays in `fugue canvas log` or `candidates --all`.
- A Write that lost the race for the head is placed on the new head; only one that cannot be placed (`canvas_head_moved`) is a candidate.

In the window, File > Canvas… is the picture of one canvas (the newest at first), its Layers panel with Undo and
Redo, and a **History** panel you can open from the bar: each change in words, newest first ("Layer added: A wooden
table", "Opacity 50%: Fill", "Layer deleted: Sketch"), whoever made it. Click a step to look at the canvas as it was
then (read only) and **Back to latest** to return; **All revisions** adds the candidates, pending and dropped. When
candidates wait, one chip says how many ("2 pending"); click it to review the newest. The picture
zooms with the wheel and pans with the middle button or Space held (Home or 0 fits, 1 is 100%, + and - zoom); a left
drag draws a region. The canvas button opens the
list of your canvases, with **New canvas…** and **Import picture…** under it; with no canvas yet the picture's place
offers the same two. A candidate shows over the latest version; **Show candidate** off shows the latest alone, so you can
flip between before and after. **Accept** (or Enter), **Drop** and **Export…** are in the bar (Drop only hides: a dropped
candidate can still be accepted), and **Compare** opens the views of Compare Images (slider, horizontal slider, side
by side, flip and difference) against the latest version; the view you pick last comes back next time. The view follows the Canvas
Region node you select in the graph, or by itself the only one that reads this canvas (a new one with no canvas yet
takes this canvas at your first change): its rectangle (and the padded box the node cuts) is drawn on the picture;
**Use for region** makes the node read the step you are looking at (the latest version, if you are on it), as one
undo step. With no node followed a drag still draws a rectangle: with several Region nodes the view asks which one it
is for, with none it offers **Add a Canvas Region node** (one undo step); until then the rectangle is not kept. A node
that reads another canvas says so, with **Show** its canvas and **Use** this canvas **for this node**. That rectangle is
the next work area, and you pick it on the picture: drag in empty space to draw a new one, drag inside it to move it
or a handle to resize it (Shift keeps its shape, or makes a new one square; Alt works from the center). The numbers
under the picture, the padded box and the size the model will get follow the pointer, and **Snap to 16** (the node's
Multiple of) puts the edges on multiples of 16. Letting go writes X, Y, Width and Height in one undo step, with no
Apply; Esc during a drag leaves the node as it was. The wheel zooms and the middle button (or Space held) pans, and
neither moves the rectangle. It works on whichever revision you are looking at, a candidate included. With X, Y,
Width or Height linked to another node, or while Compare is on, it is only drawn; with no Canvas Region selected the
view says so. The view refreshes by itself when a run adds a candidate or moves the head.

File > Canvas history… lists your canvases (or, with none yet, offers New canvas… and Import picture…); open one to
see its revisions and candidates, and click a revision to see it at full size (zoom and pan; the head is shown
first). Press **Compare** to see a candidate against the head and any other revision against the one before it (the
same views as Compare Images), and **Accept** (or Enter) or **Drop** on a candidate. Ctrl+click a revision to use it as the left
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
  the panel go back and forward. Layers are changed on the head only: on an older revision or a
  candidate the list is there to look at.
- Hover a layer to see where it came from (the Canvas Write that made it, and when).
