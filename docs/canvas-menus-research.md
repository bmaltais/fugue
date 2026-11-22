# Canvas view menus: what other editors offer, and what Fugue builds (#232 slice B)

Source: the public user documentation and menu references of Adobe Photoshop (helpx.adobe.com: File, Edit, Layer, Select
and View menus), Photopea (photopea.com/learn), Krita (docs.krita.org, Main menu), Affinity Photo (affinity.serif.com
help) and Krea (krea.ai editor). **Limit of this table:** it was written on 2026-11-22 from those documents' menu lists
as known then; the pages were not re-fetched one by one (the author had no sub-agent and a short budget), so "who has
it" names a menu, it is not a verified feature comparison. Krea's editor has no menu bar (its actions sit in a
toolbar), so it counts for tasks, not for menu names.

Rule for the Canvas view's menus: a canvas is **autosaved** (every edit is a revision), so Save, Save as, Revert and
Open recent have no meaning here. A task is **built** when it is cheap on what exists. A feature Fugue should have later
is a **placeholder**: a greyed row, not a command (no handler, no key, nothing in the shortcut map), whose tooltip says
"Not built yet (#N)", with an issue of its own. The rest is **skipped**, with the reason.

## File

| Item | Who has it | Decision | Why |
|---|---|---|---|
| New canvas… | Ph, Pp, Kr, Af | built | `NewCanvasDialog` (slice A) |
| Open canvas (this root's canvases) | Ph Open, Pp, Kr, Af | built | the canvas picker's rows (`canvasMenu.js`) as the canvas button's menu |
| Import picture… | Ph Open, Kr Open, Af | built | `importPicture` |
| Export as… (PNG, JPEG, WebP flattened; .ora with layers) | Ph Export As, Pp, Kr, Af | built | by the file's extension, as `fugue canvas export` does |
| Export > Export as with quality and scale (Save for web) | Ph, Pp, Af | placeholder, new issue | needs a dialog with a preview and a size estimate |
| Export > Export layers as files | Ph, Kr, Af | placeholder, new issue | one picture per layer, named |
| Export > Export PSD | Ph, Pp, Kr, Af | placeholder, #227 | issue exists |
| Rename canvas… | Pp, Kr | built | `CanvasStore::rename` (new) |
| Duplicate canvas | Pp, Af, Kr | built | `CanvasStore::duplicate` (new): the whole history under a new id |
| Clear canvas | Ph Edit > Fill, Kr Edit > Clear | built | `CanvasStore::clear` (new): one undoable revision, asks first |
| Delete canvas… | none (files go in the file manager) | built | `CanvasStore::remove` (new): permanent, asks first, danger style |
| Close canvas | Ph, Pp, Kr, Af | built | the dialog closes; an area shows the empty state |
| Place embedded / linked | Ph, Pp, Af | placeholder, #236 | place a picture as a layer |
| Canvas size… / Document properties | Ph, Kr, Af | placeholder, #240 | the resize revision |
| Edit in external editor | Krita plugins | placeholder, #215 | issue exists |
| Share / Publish | Ph Share, Af | placeholder, new issue | a package that carries a canvas (epic #206) |
| Automate / Batch | Ph Automate, Kr scripts | placeholder, new issue | a canvas action over a folder of pictures |
| Open recent | Ph, Pp, Kr, Af | skip | the canvas list is newest first and one click away |
| Save, Save as, Revert, Save a copy | all | skip | autosaved revisions; Undo and History revert; Export is the copy |
| Print | Ph, Kr, Af | skip | no print path in Fugue; Export is the way out |

## Edit

| Item | Decision | Why |
|---|---|---|
| Undo, Redo | built | `canvas-view.undo` and `redo` |
| Cut, Copy, Copy merged, Paste (as layer or canvas) | placeholder, #236 | paste and place |
| Fill…, Stroke…, Free Transform | placeholder, #236 | move and transform |
| Preferences | skip | Settings is the window's |

## Select

| Item | Decision | Why |
|---|---|---|
| All (Ctrl+A) | built | the whole canvas |
| Deselect (Ctrl+D) | built | clears the rectangle, the Region node's rectangle and the painted selection |
| Inverse (Ctrl+Shift+I) | built for the painted selection only | `invert()`; the rectangle has no inverse yet (disabled with the reason) |
| Quick Mask (Q), Use selection | built | existing |
| Subject, Sky, by words | placeholder, #238 | AI selection |
| Modify (feather, grow, shrink), Transform selection | placeholder, new issue | selection model first |

## Layer

| Item | Decision | Why |
|---|---|---|
| Add layer mask (Ctrl+Shift+M), Invert mask (Ctrl+I), Overlay (\\) | built | existing commands |
| Reveal all, Hide all (mask) | built | `fillMask`, `clearMask` |
| Duplicate, Delete, Rename, Raise, Lower, Show or hide | stay in the Layers panel | the panel has them with keys; doubling them is the follow-up |
| New layer, Merge down, Flatten, Group, Layer properties | placeholder, new issue | the engine has no new-layer or merge op |

## View

| Item | Decision | Why |
|---|---|---|
| Fit (Home), 100% (1), Zoom in (+), Zoom out (-) | built | the view's own keys, shown as text |
| History, Layers panels | built, checked | |
| Compare, Show candidate, Advanced details | built, checked | |
| Snap to the node's multiple | built, checked | the options bar's Snap |
| Tools: Hand, Zoom, Rectangle region, Mask brush, Mask eraser, Quick Mask | built, checked | the strip's commands |
| Rulers, Grid, Guides | placeholder, new issue | none in the view |
| Screen modes, Rotate view | skip | |
