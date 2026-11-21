# Window placement

Window UI, changed or new, follows this file. [canvas-look.md](canvas-look.md) and [spec.md](spec.md) win when they name the same control.

The dated audit [.ui-ergonomics/2026-11-20-main-window.md](../.ui-ergonomics/2026-11-20-main-window.md) is the measurement of the main window, the Run menu, and node search (from source, text size 12). This file is the rule set that measurement produced, and the list a later change keeps current.

## Rules

Apply these to every control this change adds or moves. The procedure for a control the list below does not settle is the `ui-ergonomics` skill (`references/constraints.md`, `motor.md`, `search.md`, `grouping.md`).

- **Floor.** The press target is at least 24px on its smaller side. A smaller target names its exception in the **Decisions** row: the size is essential (a port on the 6u row), or a 24px circle centered on it meets no other target.
- **Primary.** Run on Ctrl+Enter is the main action (spec 12.3). It stays a key on this window, and the Run menu stays the pointer method. A new surface has one primary control, with a treatment the other controls on that surface do not share.
- **Destructive.** Its own chunk. The gap before it is at least twice the gap inside the neighboring chunk. It is not the last row of a menu. A reversible action keeps undo on this window.
- **Gaps.** One gap inside a chunk. The gap between chunks is at least twice that, in `u`. A label sits closer to its own control than to the control before it.
- **Learned place.** A menu order, or a command the spec marks as matching ComfyUI, stays where it is. Size and gap may change. A reorder waits until that row moves from **Not scheduled** to **Open**.
- **Posture.** The canvas is sovereign: keep its density, and raise a frequent target or anything under the floor. A dialog or a popover is transient: fewer decisions, and its primary target may be larger.
- **Same surface.** The steps of Run stay on this window.

## On a change or a new surface

1. Read **Rules**, **Open**, and **Not scheduled**, and the canvas-look or spec section for each control you add or move.
2. Each of those controls meets **Rules**. A **Not scheduled** row for that control stays as written.
3. An **Open** row for a control this change touches is finished in this change.
4. Record the control under **Decisions**: file, old value, new value, and the rule it meets.
5. When the decision belongs in the look or the spec, write it there and leave a pointer in the row.

Done when every added or moved control has a **Decisions** row that names the rule it meets, any **Open** row for a control this change touches has moved to **Decisions**, and every **Not scheduled** row is unchanged.

## Open

### Port loses to the resize corner

- File: `src/canvas/FugueCanvas.qml` (`cornerAt` runs before `portUnder`)
- Rule: canvas-look §4 and spec Appendix A. A port inside the corner square starts a wire.
- Acceptance: at zoom 1, a left press at a port center that lies inside the corner square starts a link and does not call `beginResize`. A press in that square with no port still resizes.

## Not scheduled

Drafts from the 2026-11-20 pass that the rules above already answered. A window change leaves these as they are until the owner moves the row to **Open**.

| Control | As it is | Why it stays |
| --- | --- | --- |
| Delete on the selection toolbar | 2px from the neighboring buttons | An 8px gap is below what a person notices |
| Run-error row | height is the line plus 8px (22 for one line at text size 12) | The floor would make one line 24 |
| Run menu title | the same treatment as the other titles | Spec 12.3: the main action is Ctrl+Enter |
| Slider click | opens the editor; a drag sets the value | The same click as the other wells |
| Delete in the node menu | last row of `menus.cpp` `node()` | Spec 12.4 keeps the ComfyUI order |
| Resize corner | 4u, and at least 12 screen px | The size written in canvas-look §4 |
| Selection toolbar | one row of glyphs | canvas-look §11, the ComfyUI toolbox |

## Decisions

### 2026-11-20 main window, Run menu, node search

- Record: [.ui-ergonomics/2026-11-20-main-window.md](../.ui-ergonomics/2026-11-20-main-window.md)
- Rule: Primary. Run stays Ctrl+Enter.

### 2026-11-20 widget menu search (#84)

- Control: search field of a widget's menu, `src/canvas/widgets/WidgetMenu.qml` (`widgetMenuSearch`), opened by `WidgetPicker.qml`.
- Old: none; a model list of a ComfyUI folder (a dozen files per type) could only be scrolled, 12 rows at a time.
- New: one row (the menu's row height + 2) above the list, a search glyph and a plain field, focused when the menu opens, for every model menu and any menu of more than 12 entries. Typing filters by every word; Up, Down and Enter work from the field; Escape empties it, then closes.
- Why: a long unordered list is found by recall, not by scanning (Hick); the field takes the keys at once, so no extra click comes before typing.
- Rule: Floor (the field is 26px tall at text size 12: the row height, 24, plus 2). Posture: a transient popover, with no new decision: typing narrows, Enter picks as before.

### 2026-11-20 model menu order by fit (#85)

- Control: the entries of a text encoder or VAE menu in Load Models, `src/canvas/widgets/InputWidget.qml` (`modelEntries`), drawn by `WidgetMenu.qml`.
- Old: every file under its folder, in name order.
- New: the files that fit the picked diffusion model first, under one "Fits <model>" heading (2026-11-20, the owner asked for a clear compatible section); then an "Other" heading with the rest, those that do not fit in the label color (`dim`), still pickable, a file the helper cannot load with a one-line note saying so.
- Rule: Gaps (one heading per chunk, as the folders already are). Posture: no new decision; the likely pick is the first row once the files' headers are read, at once on a later open, a few ms after the first.

### 2026-11-20 what an extra model folder holds (#86)

- Control: under each Extra model folder, `src/settings/PathsSetting.qml` (`model_paths-holds-<n>-<kind>`), a row of choices (`Choices.qml`, the settings' segmented buttons), then `model_paths-found-<n>`, what its files say.
- Old: a path and its remove button.
- New: the path, then its choices (Found by its files, ComfyUI layout, LoRAs, VAEs, Diffusion models, Text encoders, Checkpoints), then "Found: LoRAs" in the label color while the choice is Found by its files.
- Rule: Floor (each choice is a SmallButton, 7u = 28px tall). Gaps (one unit inside the folder's chunk, as the folders' list keeps). Posture: the default needs no decision; the choices are there for a wrong guess.

### 2026-11-20 Templates dialog (#83)

- Control: File > Browse templates (`file.templates`, `src/menu/menus.cpp`), after Open the workflow on the clipboard, before the separator; and the dialog it opens, `src/workflow/TemplatesDialog.qml`: categories on the left (7u rows, as Settings' pages), a search field, cards in a grid (56u or wider, the preview at 16:10, title, two lines of description, a status line in the error color when models or packs are missing).
- Old: none; the examples were files to open by hand.
- New: the menu entry; the dialog, focused on its search field. The whole card is the press target; Enter opens the first match from the field, or the card the arrows reached (Down from the field goes to the first card). The category tabs take Tab focus; Up, Down, Space and Enter pick one. A card's status line names the missing files on hover. Escape or the scrim closes it (Popup's default close policy).
- Why: picking a template is recognition, not recall, so the preview leads the card (Hick: one look per card, not a list of names); the search takes the keys at once. The status says before opening what the machine lacks.
- Rule: Learned place (the File items before it keep their order). Floor (a card is far above 24px; the tabs are 7u = 28px). Primary (one action, open a card, with no other button competing). Posture: a transient dialog, larger targets.

### 2026-11-20 model downloads (#89)

- Control: the banner, `src/workflow/WorkflowDialogs.qml` (`workflowNotice`): after its line, `downloadInto` (a ComboBox, "Into <folder>", only when more than one model folder can take the files) and `downloadModels` (highlighted, "Download 3 models, 19.4 GB"), before the dismiss ×; under it, while models download, `downloadProgress`: "Downloading <file>, 2 of 3", a bar, "4.2 of 10.2 GB", and `downloadCancel` last. The banner now also shows while downloads run with no line.
- Control: on a Templates card, `src/workflow/TemplatesDialog.qml`, the status line's row ends in `templateDownload-<id>`, a flat button, "Download 19.4 GB" (or "Download" when a size is not recorded; its tooltip the count), "Cancel" while that template's models come; the status line then shows the file and progress, and after a failure the failure (the banner is under the dialog's scrim). The status line's tooltip names each file and the folder it goes in. Cards are 3u taller for the 7u row.
- Old: the banner was a line and ×; a card's status was text only.
- New: one action per workflow, where the line names what is missing; the folder choice beside it, remembered, so the usual case is one click. The controls are made only while shown (Loader): the window keeps its item budget (#5).
- Why: the button sits where the eye already is (the line that says what is missing), and the count and size say what the click costs before it is made. One button for all, not one per model: a template's models are wanted together (Hick: one decision, not three). Progress stays at the bottom edge, off the canvas.
- Rule: Floor (the banner's buttons and choice are Material's 48px; the card's button 7u = 28px). Primary (Download is the banner's one highlighted control; on a card, opening it stays the primary and Download is flat). Gaps (8px inside a row, 16 between the line's row and the progress row; px as the banner had). Destructive (Cancel, which drops the part, is its row's last control with 16px before it, twice the row's 8; on a card 2u before it, twice the row's 1u). Posture: the banner is transient; it asks nothing the person did not start.

### 2026-11-20 export for sharing (#91)

- Control: File > Export for sharing… (`file.export-share`, `src/menu/menus.cpp`), the item that was Export share copy…, in the same place after Save as; it opens `exportDialog`, a save dialog for a `.fuguepack` (`src/workflow/WorkflowDialogs.qml`). The open dialog's filter names packages too.
- Control: File > Export the compiled graph (`file.export-compiled`), unchanged in place and label; it now opens `compiledDialog`, a save dialog for a `.json`.
- Control: the banner's `installPack` ("Install <pack>…", plain, before Download), while the line names nodes of a pack the workflow says where to get; it opens Settings > Packs and starts the install there, which asks.
- Old: both items were listed and enabled but did nothing; a missing pack was named with nowhere to go.
- New: the label says what it makes; each dialog asks one thing, where. The banner then says where it went and what it left out, or why the graph does not compile.
- Rule: Learned place (the File items keep their order; only the label changed). Posture: a transient dialog, one decision.

### 2026-11-20 workflow tab colors (#98)

- Control: the tabs, `src/workflow/WorkflowTabs.qml`: each tab has its own color (a bar along the top, 2px, 4px on the current tab, and a light wash behind it), the top corners take `look.corner`; a right click on a tab opens `tabColorPopup-<i>`, six swatches (`tabColor-<i>-<n>`, 7u = 28px).
- Old: every tab had the same look, the current one marked by the accent along its top; square corners.
- New: as above. The color is the six theme hues (red, yellow, green, cyan, blue, magenta), each lifted to 3:1 against the strip and the current tab, so the mark reads on light and dark themes; a theme with no color gets six fixed hues. The name, the unsaved dot and the bold current title stay, so color is never the only signal.
- Why: the tabs of several workflows were told apart by their names only; a color is recognized before a name is read. The first free color is given, so open tabs differ; a saved workflow keeps its own by path.
- Rule: Floor (swatches 28px, the tab row 36px). Posture: a transient popover, one decision (pick a swatch). Learned place: no menu changed.

### 2026-11-20 CUDA helper notice (#97)

- Control: `helperNotice` in `src/Main.qml`, a row between the tabs and the canvas, made only while shown (Loader): a line, `helperNoticeCopy` (highlighted, "Copy sudo pacman -S fugue-helper-cuda"), then the dismiss `helperNoticeDismiss` (×).
- Old: none; an NVIDIA user ran on the slower Vulkan helper without being told.
- New: the notice above, until dismissed (remembered in the state folder).
- Why: the command is on the button, so the click costs nothing unknown; one action, then the ×.
- Rule: Floor (the button and the × are Material's 48px). Primary (Copy is the row's one highlighted control). Gaps (8px inside the row, as the workflow banner). Posture: transient; it asks nothing.

### 2026-11-21 Compare Images viewer (#62)

- Control: on the node's face (`CanvasNode.qml`, `compareViewer`, in the thumbnail's place and size, made only at full detail like the thumbnail; zoomed out, A's thumbnail stands in), the viewer above; the divider (2px, with a 2u-wide grip), the labels A and B (chips on the pictures, as the info chip), the size notice (an amber chip when the sizes differ). Large view: `CompareDialog.qml`, opened by a double click on the picture (`FugueCanvas.qml` `openCompare`), a modal popup at 90% of the window with a bar of seven buttons: Slider, Slider horizontal, Side by side, Flip, Difference, then Fit and 100%, then the close button; Escape closes.
- Commands: `compare.flip` (`\`), `compare.next-mode` (`M`), `compare.fit` (`0`), `compare.actual` (`1`), in the new `compare` context, active only while one Compare Images node is selected; `compare.copy-a`, `compare.copy-b` and `compare.copy-view` (no keys) in the node's context menu, shown when the node has a result.
- Old: none; a Preview showed one image.
- New: as above. The divider is the primary control of the face and the only one with a grip.
- Why: comparing is done by the eye on one place, so the pointer moves the divider with no press (rgthree's Slide); keys do the rest without the pointer leaving the picture (Fitts: the target is the whole picture); the large view's bar is the five views and two zooms, in two chunks with a gap of 2u between (twice the 1u inside a chunk).
- Rule: Floor (the bar's buttons are 7u = 28px tall; the face's whole picture is the target, the divider is read, never aimed at). Primary (the divider on the face; no highlighted button in the large view, the current view is marked by the accent). Gaps (1u inside the bar's chunks, 2u between; the close button apart at the end, last). Destructive: none. Learned place: the context menu's image items keep their order, the three copies follow Save Image. Posture: the face is the canvas, so its density is kept and the picture is the target; the large view is a transient dialog. The bar's buttons take Tab focus; the face's picture too, for the arrow keys.

### 2026-11-21 large image view and visual crop (#73)

- Control: `ImageDialog.qml` (`imageDialog`), opened by a double click on any node's picture (`FugueCanvas.qml` `openImage`), made on first use; `CropDialog.qml` (`cropDialog`), opened by a double click on a Crop node's picture; both in `LargeViewDialog.qml`, the frame and bar `CompareDialog.qml` now uses too (`ViewButton.qml` is its button). The bar: the dialog's own tools, then Fit and 100%, then the close button; keys 0, 1, + and -. The crop window's tools are the ratio lock (Free, Original, 1:1, 4:3, 3:2, 16:9, Turn); under the picture the fields X, Y, Width, Height, then Cancel and OK (`cropOk`, the one highlighted button). On the node's face (`CanvasNode.qml`, the slot's `cropViewer`, made only at full detail): the frame over the picture with eight handles (`CropFrame.qml`).
- Old: a double click on a picture did nothing except Compare Images'; a Crop showed its result and took numbers only.
- New: as above. A press on a handle or inside the frame drags it; a press outside it pans the viewer (nothing at Fit on the face); Shift keeps the ratio, Alt resizes from the center; Enter applies, Escape cancels.
- Why: the picture is the target (Fitts), the frame is moved by direct manipulation and the numbers follow it, so there is no round trip between a field and a guess.
- Rule: Floor (handles are drawn 8 px, with a 24 px press target: 12 px reach, held to a third of a small frame's side so its inside stays to move it; the bar and footer buttons are 7u = 28px, the fields Material's 40px). Primary (OK in the crop window; the large image view has none). Gaps (1u inside the bar's and the footer's chunks, 2u between; the close button apart, last; Cancel before OK with 2u after the fields). Destructive: none, Cancel discards a draft and a drag is one undo step. Learned place: Photoshop's (Shift, Alt, Enter, Escape, the thirds); no menu changed. Posture: the face keeps the canvas's density; the windows are transient dialogs.

## Components

Reusable controls other surfaces may call. A component names its input and output here, knows nothing of its caller, and a caller's glue lives in its own file.

### Color picker (#52)

- Files: `src/canvas/ColorPickerPane.qml` (the picker), `src/canvas/ColorPickerPopup.qml` (the pane on a popup), `src/menu/color_history.h` (`App.colorHistory`: the recent list and the color math), `src/engine/layout_color.h` (the same in plain C++: `normalizeHex`, `hsvFromHex`, `hexFromHsv`, `RecentColors`). Canvas glue: `src/canvas/LayoutColorPicker.qml` and `src/menu/layout_colors.h` (node and Frame coloring, the first caller).
- Input: `setColor(c)` starts it on a color ("#rrggbb", "#rgb", or without the "#"; a bad value is ignored). `remember` (true) and `showRecent` (true).
- Output: `color` (read, the color being chosen now, follows every drag and key), and `picked(color)` on a confirm: Apply, Enter in the hex field, or a click on a recent color. A confirm puts the color first in `App.colorHistory.recent` when `remember` is true. The caller decides what picking does.
- Popup: `ColorPickerPopup.openWith(color)`, `picked(color)`; Escape or a press outside closes it with no `picked`; it has no open or close fade.
- Keys: the wheel, the brightness bar, the hex field, Apply and the recent colors take Tab focus. On the wheel, Left and Right turn the hue and Up and Down set saturation, 5 steps (Shift: 1); on the bar, Up and Down.
- A new caller: a `ColorPickerPane` or `ColorPickerPopup`, `setColor`/`openWith` with its current color, `onPicked`. Needs `App.colorHistory` (null in a window made without it: the picker then picks nothing).

### Large view and crop frame (#73)

- Files: `src/canvas/LargeViewDialog.qml` (a modal frame with a bar: its `tools`, Fit, 100%, close; the `body` and an optional `footer`; keys 0, 1, + and -, Enter as `confirm`), `ViewButton.qml` (its 28px button), `CropFrame.qml` (the frame over any picture), `CropView.qml` (CompareView in its `single` mode with a CropFrame), `CropGeometry` (`src/canvas/crop_qml.h`, the geometry for QML over `src/engine/crop_geometry.h`). Glue: `ImageDialog.qml`, `CropDialog.qml`, `CropNodeView.qml`; C++ `CanvasModel::cropRect`, `setCropRect`, `pictureAt`, `CanvasRuns::imageUrl`.
- LargeViewDialog input: `look`, `name` (the prefix of the objectNames of its bar), `zoomable` (anything with `fit()`, `actualSize()`, `zoomAt()`, `actual`, `zoom`), `tools`, `footer`, the viewer as its child. Output: `confirm`; it closes itself.
- CropFrame input: `imageSize`, `originX`, `originY`, `zoom` (where the picture's corner is in the frame's coordinates, and view units per pixel), `rect` ({x, y, width, height}), `ratio` (0 = free), `editable`. Output: `committed(rect)` at the end of a drag, `changing(rect)`, `pressed`, `activated`. It takes only presses on its handles and inside the frame.
- CompareView `mode: "single"` is one picture with the wheel, drag, Fit and 100%; `showZoom` adds the zoom to the size notice; `imageX`, `imageY` and `imageScale` give a picture's place for what is drawn over it.
- A new caller: a `LargeViewDialog` with a viewer inside, or a `CropView` with the picture, its pixel size, a `rect` and `onCommitted`; the geometry is `CropGeometry` or `crop::` (tests: `tests/engine/test_crop_geometry.cpp`).

### Compare view (#62)

- Files: `src/canvas/CompareView.qml` (the viewer), `src/canvas/CompareNodeView.qml` (glue to a Compare Images node and the model's `compareView` state), `src/canvas/CompareDialog.qml` (the large view). C++: `CanvasModel::compareView`, `setCompareView`, `compareStep`.
- Input: `sourceA`, `sourceB`, `sourceDifference`, `sizeA`, `sizeB`, `labelA`, `labelB`, `look`, and the view: `mode` ("slider" the primary and default, "slider_horizontal", "side_by_side", "flip", "difference"), `position`, `showB`, `zoom` (1 = fit), `actual` (1:1), `centerX`, `centerY`. `interactive`, `hoverWipe`, `chrome` and `wheelModifiers` are the caller's choices.
- Output: `viewChanged(current)` on every change a person makes, `pressed` and `activated` (a double click). Functions: `setView`, `apply` (takes a state without announcing it), `flip`, `nextMode`, `fit`, `actualSize`, `zoomAt`, `grab(callback)`.
- Pointer: in a slider mode the divider follows the pointer and a drag; a middle drag or Shift+drag pans. In the other modes a drag pans and, in flip, a click flips. The wheel zooms at the pointer (the node's face asks for Alt, so a plain wheel keeps moving the canvas). The arrow keys move the divider (Shift: 10%), Home and End put it at the ends.
- A new caller: a `CompareView` with its sources and sizes, `onViewChanged` to keep the state, `apply(state)` to give it back.

### Run toolbar and queue panel (#60)

- Files: `src/run/RunToolbar.qml` (the bar), `src/run/RunQueuePanel.qml` (the panel), `src/canvas/ViewButton.qml` (their button, now with a `glyph` and a `tip`). C++: `src/run/run_queue.h` (`App.runQueue`: the rows and the operations), `src/run/run_bar.h` (`App.runBar`: shown, edge, panel open, and their commands), `Queue::moveToFront` in `src/engine/queue.h`.
- RunToolbar input: `look`, `busy`, `canCancel`, `canRun`, `pending`, `progress`, `batch`, `maxBatch`, `dock` ("top" or "bottom"), `queueOpen`. Output: `runRequested`, `cancelRequested`, `batchRequested(n)`, `queueToggled`, `dockRequested(edge)`, and `menuAboutToOpen` (the caller sets `menuItems`, a CommandMenu's rows, as the split menu shows them). It fills what it floats over; `surface` is the bar, for a caller that places something by it.
- RunQueuePanel input: `look`, `entries`, `history` (the row shapes of `run_queue.h`), `maxHeight`. Output: `cancelRequested(run)`, `frontRequested(run)`, `resultsRequested(run)`, `folderRequested(run)`, `closeRequested`. It knows nothing of where a run came from.
- Keys: Run, Run first, Run to here, Cancel and the panel (Alt+Q) are commands with keys. On the grip, Up and Down move the bar to the top or bottom edge; Left and Right move along the bar's controls once one has the focus, Up and Down along the panel's. Tab is the window's own key, so the arrows do it. Escape closes the panel while it has the focus (the run's cancel keys sleep then, `CanvasRuns::yieldKeys`).
- A new caller: a `RunToolbar` and a `RunQueuePanel` in a Loader, bound as `src/Main.qml` binds them; any `RunQueue`-shaped rows will do.

## Unread

Settings, the shortcuts overlay, the quit question, About, every menu except Run, the context menus, the workflow dialogs and their questions (those questions are a row of buttons), the group dialogs, image save, the widget pickers (but their search field), the magnifier.

### 2026-11-21 align and distribute (#58)

- Control: the Node menu, after Pin: `Align` (Left, Right, Top, Bottom) and `Distribute` (Horizontally, Vertically) submenus, and the same two in the node's context menu after Colors (`src/menu/menus.cpp`; commands `node.align-*`, `node.distribute-*` in `src/menu/menu_commands.cpp`).
- Old: none.
- New: two submenus, no default keys. In the node's menu a command shows only when it applies (Align from two selected nodes, Distribute from three), and a submenu with nothing left is left out; in the bar they stay and dim, as the other Node items do.
- Why: ComfyUI offers both as menu items, so recognition not recall; six leaf commands under two headings keep the Node menu from growing by six rows (Hick).
- Rule: Learned place (the existing Node and node-menu items keep their order; the new rows sit after the toggles, before the image items). Gaps (the two rows join the toggles' chunk; no gap added). Floor (menu rows are the menus' existing height). Destructive: not applicable, and undo reverses each.

### 2026-11-21 color picker for nodes and Frames (#52)

- Control: the last row of the Colors menu (Edit > Colors, the node and Frame context menus and the selection toolbar's color button), `node.color.custom`, "Custom…", after a separator (`src/menu/menus.cpp`); and the popup it opens, `ColorPickerPopup` through `LayoutColorPicker.qml`: the disk 40u (160px), the brightness bar 7u (28px) wide beside it, a row of the color's preview (7u), the hex field (7u tall) and Apply (7u tall, highlighted), and under it the recent colors, 7u swatches 1u apart, six to a row, up to 12.
- Old: nine theme roles and "No color"; no other color could be given.
- New: the row above, enabled with a selection or on a Frame. The popup opens centered with the hex field focused, started on the target's hex color, else the last one used, else the accent. Nothing changes until Apply, Enter, or a click on a recent color; that is one undo step. Escape or a press outside leaves it as it was.
- Why: a person who wants a color the roles lack picks it by sight (the disk, as Blender's picker) or by value (the hex field); the colors used lately are recognized, not recalled (Hick), and one click re-applies one. The roles stay the quick path in the same menu, the order of the menu above is unchanged.
- Rule: Learned place (the nine roles keep their order; the new row is last, after a separator, the chunk gap). Floor (every target is at least 24px: swatches, field, button and bar 28px; the disk 160px; the wheel's handle is a mark, not a target). Primary (Apply is the popup's one highlighted control). Gaps (1u inside a row, 2u between the disk and the bar, 3u between the three chunks, more than twice 1u). Posture: a transient popup, two decisions at most (pick a color, apply it). Destructive: not applicable, undo reverses it.

### 2026-11-21 run toolbar and queue panel (#60)

- Control: the run toolbar (`RunToolbar.qml`, `runToolbar`), floating over the canvas, centered at the top or bottom edge: grip (`runToolbarGrip`), batch count (`runBatchLess`, `runBatchCount`, `runBatchMore`), Run and its arrow (`runButton`, `runMenuButton`), Cancel (`runCancelButton`), the queue button (`runQueueButton`); the panel (`RunQueuePanel.qml`, `runQueuePanel`) with a row per run, `queueFront-<run>` and `queueCancel-<run>` on the runs going and waiting, `historyResults-<run>` and `historyFolder-<run>` on the finished ones. In the View menu: `panel.queue` (Alt+Q) and `view.toggle-run-toolbar`, after the panel rows.
- Old: none; a run was started by the keys or the Run menu, and only the pill said that one was going.
- New: as above. The toolbar and panel are made only while shown (Loader), and the toolbar not in a window without runs: the window keeps its item budget (#5).
- Why: Run is the main action and the pointer reaches it at once (Fitts: a bar at an edge the canvas leaves free, 28px buttons); Cancel is next to it but in a chunk of its own, so aiming at Run never lands on it. The panel is a list read by recognition (what is going, in what order, since when), with the two actions that change the order one press away on the row, and the click on a finished row doing what a person wants of it (see its results).
- Rule: Floor (every button is 7u = 28px; the grip is 4u wide but 7u tall and has the keys; the tooltips name the glyph-only ones). Primary (Run and its arrow are the toolbar's only accent buttons; the panel has none: its rows are equal). Destructive (Cancel is its own chunk, 3u from the chunks on both sides while the gap inside a chunk is 1u; on the panel's rows the x is the last button, after the move-to-front arrow, 1u as the neighbors but the arrow is dimmed on the first waiting run so the two never swap places). Gaps (1u inside a chunk, 3u between). Learned place (the Run menu keeps its order; the split menu is the same rows; ComfyUI's order is batch count, Run, Cancel; the panel is under the toolbar as its sidebar is). Posture: the bar is canvas furniture, small and always the same; the panel is transient and larger (rows 9u).
