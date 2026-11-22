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
fugue canvas list                                  # the canvases, newest first
fugue canvas candidates canvas-0123456789ab        # results waiting for your review
fugue canvas diff canvas-0123456789ab@sha256:ab12cd34   # what a revision changed, against its parent
fugue canvas accept canvas-0123456789ab            # the one candidate becomes the head
fugue canvas drop canvas-0123456789ab@sha256:ab12cd34   # hide one (it is kept)
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
- **Canvas Write** puts a picture on the canvas as a new layer, blended in through the mask (Blend
  pixels soften the seam). Wire Region's canvas, stitch and your generated picture into it.

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

Region's picture keeps the canvas's transparency. A stitch from Region also goes to Stitch.

In the window, Canvas Region's **Canvas** is a drop-down of the canvases of the folder you are working in
(the Default's, or the project's own: a canvas belongs to the one it was made in). Each row has the name,
the size, the time of the last change and the id. **New canvas…** asks for a name and a size
(1024 × 1024 to start; at most 16384 a side and 64 megapixels), or makes one from a picture file with
**From a picture…**; **Import picture…** makes a canvas straight from a PNG, JPEG, WebP or `.ora`. The
canvas is put in the input. The input still holds the id (a workflow file is the same as before). An id this
folder has no canvas by, such as one copied from the Default into a project, shows as `canvas-… (missing)`
and the list is there to pick the right one. The list follows `fugue canvas new` and runs as they happen.

## Review a fill

A fill does not have to change the canvas at once. Canvas Write with **Keep as candidate** on leaves
the head where it is and stores the new layer as a **candidate**: a revision made from the one Region
read, waiting for you. The `generative-fill` template (File > Browse templates, or
`fugue workflow new fill.fugue --template generative-fill`) is Canvas Region, a FLUX.2 Klein redraw
and Canvas Write with that on. Put your canvas's id and the rectangle in Canvas Region and run it.

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

In the window, File > Canvas history… lists your canvases; open one to see its revisions and
candidates, click a revision to see it against the one before it (the same slider, side by side, flip
and difference views as Compare Images), and press **Accept** or **Drop** on a candidate. Ctrl+click
a revision to use it as the left side of the comparison.
