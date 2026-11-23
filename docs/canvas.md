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
fugue canvas export canvas-0123456789ab team.fuguepack            # a package to share: the layers as they are now (head only)
fugue canvas export canvas-0123456789ab team.fuguepack --history  # ... with every revision, so the other person can undo
fugue canvas import team.fuguepack                 # a package's canvas, as a new canvas (a new id when yours is taken)
fugue canvas export canvas-0123456789ab out.webp --quality 80 --scale 50%   # lossy WebP at half size
fugue canvas export canvas-0123456789ab --format jpg --quality 60 --estimate   # the size it would be, nothing written
fugue canvas export canvas-0123456789ab shots --layers   # one picture per layer into the folder shots
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

## Fill a region

A fill lands on the canvas at once. The `generative-fill` template (File > Browse templates, or
`fugue workflow new fill.fugue --template generative-fill`) is Canvas Region, a FLUX.2 Klein redraw
and Canvas Write. Put your canvas's id and the rectangle in Canvas Region and run it.

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

File > Canvas history… lists your canvases (or, with none yet, offers New canvas… and Import picture…); open one to
see its revisions and candidates, and click a revision to see it at full size (zoom and pan; the head is shown
first). Press **Compare** to see a candidate against the head and any other revision against the one before it (the
same views as Compare Images), and **Accept** (or Enter) or **Drop** on a candidate. Ctrl+click a revision to use it as the left
side of a comparison. **File > Export > Export as…** saves the revision you are looking at: a flattened PNG, JPEG or WebP, or an .ora with its layers, by the file's extension.

`fugue canvas export <id>[@<revision>] <file>` writes the same flattened picture from the command line by
the file extension: `.png`, `.jpg` or `.webp` (a JPEG has no transparency, so it is flattened onto white
and the command says so; WebP is lossless); `.ora` keeps the layers. It never replaces a file.

**File > Export > Export with quality and scale…** (Save for Web) is the same command with a window: PNG, JPEG, WebP or
the layers kept (.ora); a quality (JPEG, and WebP unless Lossless); a size as 25, 50, 100 or 200 percent or in pixels
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
  the panel go back and forward. Layers are changed on the head only: on an older revision the list is there to look at.
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

## The menu bar

The Canvas view has its own menu bar at the top, in the File > Canvas… dialog and in an area alike (#232). Every option of
the view is a row of a menu, with its key beside it; what does not apply to the canvas now is greyed.

- **Canvas** (it was File; the window's File is the workflows', #262): New canvas…, Open canvas… (the picker), Import picture… (also a `.fuguepack`, below), **Export** (Export as…: PNG, JPEG, WebP flattened, .ora
  with layers, by the extension; Export with quality and scale… and Export layers as files…, #247 and #248, above; never replaces a file), **Share…** (asks, then where: a `.fuguepack` of the canvas as it is now, or with its history; see Sharing a canvas), Rename canvas…, Duplicate canvas (the whole history, under a new
  id; the copy is shown), **Clear canvas…** (asks; one undoable step that leaves one empty layer of the same size),
  **Delete canvas…** (asks; permanent; the next canvas is shown, or the empty state) and Close canvas (the dialog closes; an
  area shows no canvas).
- **Edit**: Undo and Redo of the canvas's own history, as the keys do. It stays here, and not only in the window's Edit, because the canvas has its own history and the File > Canvas… dialog covers the window's bar.
- **Select**: Select all (Ctrl+A: the whole canvas as the rectangle, or the whole Quick Mask), **Deselect** (Ctrl+D, and
  Escape when nothing is being dragged or run: it clears the free rectangle, sets the Canvas Region node's area back to 0, 0, 0, 0 = the
  whole canvas as one undo step, and clears a painted selection), Inverse (Ctrl+Shift+I, the painted selection), Quick Mask
  and Use selection.
- **Layer**: Add layer mask, Invert mask, Mask overlay, Reveal all, Hide all. The layers panel keeps its own rows.
- **View**: Fit, 100%, Zoom in, Zoom out, History, Layers, Compare, Advanced details, Snap region, and
  **Tools** (Hand, Zoom, Rectangle region, Mask brush, Mask eraser, swap, brush size).

A greyed row with the tooltip "Not built yet (#N)" is a feature that is planned and tracked in issue N; it is not a command and has
no key ([the research](canvas-menus-research.md) says what the other editors have and what was decided). The top bar keeps the
canvas picker, Fit and 100%; each tool's own settings stay in the options bar.

## The task bar: Generate on the canvas

You can change the canvas without opening a workflow (#231, [design](canvas-actions-design.md)). Draw a rectangle on
the picture, or paint a selection with Quick Mask, and a bar appears under it (above it when there is no room; across
the foot of the picture in a small area):

- the **action** menu: **Auto** (picks the action from the selection and the prompt) and the actions, Generative fill
  for now;
- the **prompt**: type what should be in the selection and press Enter (Ctrl+Enter is still Run of the Graph). Up and
  Down in an empty field bring back earlier prompts; the clock button lists the last 20;
- **Generate**, which becomes **Cancel** while it runs (Escape too); a cancelled run adds nothing;
- `⋯`: **Clear prompt history…** (it asks first).

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
(`share/actions/generative-fill.fugue`, compiled into Fugue) is the recommended workflow of the research: FLUX.2 Klein
4B Q8, 4 steps, the selection redrawn near 1 MP with the crop as its reference, a Blend of 16. If you generate again
with exactly the same prompt, seed and selection on the same version after an Undo, the cached result is placed again.
The prompts you type are kept per project, 200 per action, and never shared with another project.

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
