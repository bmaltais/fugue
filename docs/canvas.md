# Canvases

A canvas is a layered picture that Fugue keeps as a chain of revisions: a change never edits a
canvas in place, it makes a new revision and the old ones stay. This first slice of the canvas
work ([epic #206](canvas-research.md)) is the store and the command line; the nodes that read a
region from a canvas and write a result back, and the window, come next. The design and its reasons
are in [spec 14.5](spec.md#145-the-canvas-revision-store-206-208).

```
fugue canvas new --name Sketch --size 1024x768     # prints the canvas id, canvas-0123456789ab
fugue canvas import photo.ora                      # a canvas from an OpenRaster file of Fugue's
fugue canvas log canvas-0123456789ab               # the revisions, newest first
fugue canvas show canvas-0123456789ab@sha256:ab12cd34   # one revision: tree hash, layers, tiles
fugue canvas export canvas-0123456789ab out.ora    # the head as an .ora (never replaces a file)
```

`<id>@<revision>` names a revision by `head`, its full hash, or the first 8 hex digits or more.

A canvas lives in your Fugue folder (`~/.local/share/fugue/canvases`), or in the project's own
folder when you work in a project ([projects](spec.md#142-projects)). Pictures are kept as 256 px
tiles named by their pixels, so a revision that changes one corner of a large canvas stores only the
tiles it touched, and the same pixels are stored once however many revisions or canvases hold them.
Two canvases with the same pixels and layer settings have the same **tree hash**, shown by
`fugue canvas show`. Nothing is deleted yet.
