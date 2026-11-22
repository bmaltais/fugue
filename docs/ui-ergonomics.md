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

### 2026-11-21 Paint mask row and hollow mask port (#174)

- Control: the `mask` widget row in `src/canvas/widgets/InputWidget.qml` (kind `mask`), `src/canvas/MaskNodeView.qml` (the picture with the mask over it), the hollow port in `src/canvas/PortDot.qml`, `FugueCanvas.openMask`.
- Old: the Open row planned a 28px `ViewButton` under the picture.
- New: the input's own row (the full 24px well across the node, label "Paint mask" at the left, "Painted" or "None" and a brush glyph at the right) is the button; a click opens the editor on the node's picture. A double click on Paint Mask's picture opens it too; Load Image's double click keeps the large view. No picture yet: a banner notice says what to do. The mask port is a hollow square with a 3px frame.
- Why: a row is the face's density and does not change the node's layout or the height rules; the whole well is a target far over the floor in width and meets it in height.
- Rules met: Floor (24px row, the full node width), Learned place (Load Image's double click untouched), Posture (the canvas keeps its density; the editor, a transient dialog, is #172's), Gaps (no new gap), Primary (no new primary). Screenshots: `FUGUE_SHOT_DIR=... test_canvas_mask_node`.

### 2026-11-21 auto-hide menu bar (#160)

- Control: `src/panels/AutoHide.qml` around `MenuBar` in `src/Main.qml`: a hot edge (`autoHideEdge`, 4px tall, the whole window width) and a handle (`autoHideHandle`, 36 x 28px, a menu glyph) hang where the bar was, while it is not shown; the bar slides over the canvas on the pointer at the edge, a press on the handle, F10 and Alt keys; `breadcrumb` in `FugueCanvas.qml` moves right to clear the handle (`leadingClear`).
- Old: hiding the menu bar removed it; F10, the canvas menu or Reset brought it back, and a 4 s notice said so.
- New: hiding leaves the edge and the handle. In after 120 ms of pointer on them (a press at once), out after 400 ms off them, on Escape, after a command or when its menu closes; 170 ms, ease out, on `y` not on the layout, over the canvas: the canvas keeps its size and place while it slides.
- Why: a way back that needs no hidden knowledge (the handle is the cue); the edge is the infinitely deep Fitts target (the pointer pushed to the window's top lands on it), the dwell keeps a passing pointer from opening it, the longer out delay keeps it from flickering while the pointer travels to a menu.
- Rule: Floor (the handle is 28px tall, 36px wide; the edge is a 4px strip but a long edge target: its exception is that the window edge stops the pointer, and the handle, the press target, meets the floor). Posture: the canvas is sovereign, so the bar overlays it and never resizes it; the handle hangs only 24px into the canvas's corner and the breadcrumb steps aside. Gaps: 1.5u from the strip or window side. Learned place: the menu titles and their order did not change.

### 2026-11-21 mask editor (#172)

- Control: `src/canvas/MaskEditor.qml` (a `LargeViewDialog`, `name: "mask"`; viewer `CompareView` single mode; `MaskLayer` over it; the pointer is one `MouseArea` over both).
- Old: none (the Open row "Mask editor and the Paint mask button", spec 14.4).
- New: the bar, left to right, in chunks (1u inside, 2u between, the dialog's own spacer): tools Brush, Eraser, Lasso, Rectangle (28px glyph buttons, the current one in the accent); Size and Hardness sliders (26u = 104px wide, 28px tall, a fill from the left, label left and value right; a press or drag sets, Left and Right step, Shift for ten, the wheel); Undo and Redo (dimmed when none); Invert, Fill, Mask alone (the eye); then Clear alone at the end (danger tint, 4u gap before it where the others are 1u); then the dialog's Fit, 100% and close. Footer: a one-line key reminder (the error in the error color), Cancel then OK (the primary, highlighted). Measured at text size 12: every bar control 28px tall; glyph buttons 28 x 28.
- Pointer: left paints, erases, draws a shape or (Space held) pans; a middle drag pans; the wheel zooms at the pointer; the brush has no target size, its circle (the size, an inner ring for the hardness, a dot) is the cursor, drawn white over a dark ring so it reads on any picture, and the system cursor is hidden over the picture for the brush and a cross for the shapes. Alt+drag with the brush sizes (left and right, the ring's diameter follows the pointer 1:1) and sets hardness (up is harder, 200px for the whole range); Alt on a Lasso or Rectangle release erases. A tablet's pressure scales the size.
- Keys: B, E, X (swap), L, R, I, H, Shift+Delete, `[` `]` (size, x1.25), Shift with them (hardness, 0.1), Ctrl+Z, Ctrl+Shift+Z or Ctrl+Y; the dialog's 0, 1, + and -, Enter (OK), Escape (Cancel). `LargeViewDialog` gained `keyPressed` and `keyReleased` for a key it did not take. Every bar control takes Tab focus (ring in the accent), Enter and Space press a button.
- Look: the mask is the accent at 50% over the picture; H draws it white on black, the picture hidden; both follow light and dark themes (tokens only). Sizes are in picture pixels, so zoom never changes what a stroke covers. Screenshots: `FUGUE_SHOT_DIR=... test_canvas_mask_editor savesTheEditorOnLightAndDarkThemes`.
- Rules met: Floor (28px; the sliders too), Primary (OK, the only highlighted button), Destructive (Clear in its own chunk at the end of the tools, twice the gap, undoable; Cancel drops the whole session), Gaps, Learned place (ComfyUI's Alt+drag; Load Image's double click untouched), Posture (a transient dialog).
- Open choices (owner): the Fill button fills the whole picture (the complement of Clear; no flood fill); the one eye toggle is "mask alone", there is no separate overlay-off.

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

### 2026-11-21 side panels, node library and workflows (#56)

- Control: `SidePanel.qml` (`sidePanel`), made by `Main.qml` only while `panel.nodes` (N) or `panel.workflows` (W) has it open: the title bar's `sidePanelDock` (move to the other side) and `sidePanelClose` (`ViewButton`, 7u = 28px), and the width grip `sidePanelGrip`, 8px along the canvas-facing edge, full height, with Left and Right (16px, Shift 48px) when focused by Tab.
- Control: `NodeLibraryPanel.qml`: `nodeLibrarySearch` (8u = 32px tall field, focused when the panel opens from its key), `nodeLibraryClear` (8u square), `nodeLibraryList` with 7u = 28px rows (a click adds the node in the middle of the view, a drag out of the panel adds it where it is let go, Enter adds, Left and Right close and open a category).
- Control: `WorkflowsPanel.qml`: `workflowsList` with 8u = 32px rows (a click shows a tab or opens a recent file) and `workflowsClose-<i>` (7u wide, the row's 32px tall).
- Old: none; N and W were in the keymap and the View menu with no owner.
- New: as above. One panel is open at a time, remembered with its side and width.
- Why: the library is found by recall and by scanning (Hick), so the search field has the focus first and takes the keys: Down goes to the list, Enter adds the best match, Escape empties the field and then closes the panel. A click is the shortest add (Fitts: the row is the target); the drag is for the place. The grip is on the edge the pointer already reaches the canvas by.
- Rule: Floor (rows, fields and buttons are 28 to 32px; the exception is the width grip, 8px wide: it is an edge that runs the panel's whole height, so it is hit along its length, it grows a 3px mark under the pointer, a 24px circle on it would overlap the canvas it borders, and the keys do the same through Tab focus). Primary: none; the surface's one action is the search and Enter. Gaps (1u between the title bar's buttons, 1u between rows). Destructive: the close button of an open workflow in the workflows panel is at the row's far end and asks nothing more than the tab's own x does (an unsaved workflow raises the same save question); a drop on the panel adds nothing. Learned place: N and W are ComfyUI's; the View menu rows keep their order. Posture: docked, not floating, so the canvas keeps its density and a panel never covers it; the library's list scrolls by the wheel and its scroll bar, because a drag on a row takes the node out.

### 2026-11-21 side tab strip: Templates action tab (#192), Settings at the lower end

- Control: `SideTabStrip.qml` `sideTab-templates` and `sideTab-settings` (registry rows of kind `action` in `side_panels.cpp`, each with an `edge`), 44px square, 24px `layout-template` and `settings` (gear) glyphs. Templates is the third tab of the top group, touching Workflows. Settings is alone at the strip's lower end under a 1px inset rule (`sideTabActionsRule`) and half a unit of gap, 1u from the bottom. Click, Enter or Space runs `file.templates` (File > Browse templates) or `app.settings` (Ctrl+,); the tooltip is "Templates (<key>)" / "Settings (Ctrl+,)" from the command's key when it has one.
- Old: Templates sat at the lower end past the rule; Settings was reachable only from the File menu or its key. (First version of #192 put Templates at the lower end.)
- New: Templates directly under Workflows (the owner's request: the file-like destinations stay together at the top), Settings at the lower end. Neither is ever marked open or checked (the screen reader hears a plain button, not a toggle), because they launch rather than toggle; neither shows state while its dialog is up (the modal scrim already says it).
- Why: recognition over recall (Hick); Templates is a place you start work from, like the workflows list, so it belongs with them (Gestalt: proximity); the settings gear at the end of a rail is the learned place (VS Code, ComfyUI), and the rule keeps the rarely used one away from the toggles so a press is not mistaken for opening a panel.
- Rule: Floor (44px, same as the panel tabs). Gaps (the top tabs touch, as the panel tabs do; the rule and half a unit above the bottom group). Learned place: gear at the bottom of a rail; the File menu rows and their keys are unchanged. Posture: part of the frame, hidden in focus mode with the strip; in a short window the bottom group stays at the bottom and the top tabs keep their order.

### 2026-11-21 side tab strip (#159)

- Control: `SideTabStrip.qml` (`sideTabStrip`), made by `Main.qml` while the registry shows `side-tabs`: the left window edge under the bars, 44px wide, a tab per side panel (`sideTab-nodes`, `sideTab-workflows`), 44px square with a 24px glyph. A click opens the panel, a second click closes it; Tab focuses a tab and Enter or Space presses it; the tooltip says "Node library (N)".
- Old: none; N, W and the View menu were the only ways to open the panels, with nothing on the window to say so.
- New: as above. The canvas and a left-docked panel start after the strip; a narrow window shrinks the panel, never the strip (the canvas keeps 160px).
- Why: opening a panel is recognition, not recall (Hick): the icons are always visible, one per panel, so the key is learned from the tooltip. A strip on the window edge is the largest target a pointer reaches (Fitts: the edge stops it, so a tab is hit by moving there and stopping). Left, because the panels are navigation, and navigation sits at the left in ComfyUI, VS Code and Obsidian.
- Rule: Floor (44px tab, a 24px glyph, both over 24px and the issue's 36px). Primary: none; the strip is a set of equal switches, the open one marked by accent, a bar and a wash, so color is not the only signal. Gaps (the tabs touch: each is a full 44px target, 1u from the top). Destructive: none, closing a panel loses nothing. Learned place: N and W and the View menu rows are unchanged and stay in step. Posture: the strip is part of the window frame, always visible, never covers the canvas.
- Not done: tabs for the run queue (a floating panel of the run toolbar) and the media results (a view inside the workflows panel); they join when they are docked panels, as one row in the table of `side_panels.cpp`.

### 2026-11-21 delete a library group from the node library (#156)

- Control: `src/panels/NodeLibraryPanel.qml`: on the row of a group of the user's library (`local/<name>`, row field `library`) a trash can, `nodeLibraryTrash` (`src/panels/RowTrashButton.qml`, 7u = 28px square, the row's far end, left of the scroll bar's gutter), and the question it opens, `deleteGroupQuestion` (`src/panels/DeleteGroupQuestion.qml`): the title names the group, one paragraph says what happens, then `deleteGroupCancel` and `deleteGroupConfirm` ("Delete").
- Old: a library group could only be removed in Settings > Packs > Library > Remove… > Remove.
- New: the can on those rows only (a built-in node, another pack's node and a category have none; a pack is removed from Settings > Packs). It is made only on those rows (a Loader), so the list keeps its item budget (#5). It opens a separate question; nothing is deleted before its Delete. After it, the row and the node search result go at once and the window's notice line says "Deleted <name> from the library"; there is no undo of the folder. Node search has no can: its row is full (kind tile, name, category, port signature, version) and is a click-to-add target, so a 28px target at its far end would push the version off or sit beside the add click; the panel is the one place that manages the library. Settings > Packs and the panel call the same `PacksModel::removeLibraryGroup`.
- Why: a group someone saved is seen in the library, so the way out is there too (recognition, not a trip to Settings); the can is at the row's far end, away from the name that adds the node (Fitts: a mis-click on the name adds a node, which undo takes back, never a delete), and the delete is a second, separate question (Destructive).
- Rule: Floor (the can is 28px square; the question's buttons are Material's 48px). Primary (Cancel is the question's one highlighted control and its default: Enter and Escape keep the group). Destructive (its own question, 2px accent border, Cancel before Delete with 4u between them, twice the 2u inside the paragraphs' gap; a click on the can never deletes; the can is not reached by a drag: a press that starts on it is its own, the row neither adds nor drags from it; undo for the group's folder does not exist, the question says so). Gaps (the can sits flush with the row's right edge, 0 to the scroll bar; the name's text ends 1u before it). Learned place: ComfyUI deletes a saved blueprint from its node library after a confirmation, the same place. Posture: a transient modal question, one decision.
- Keys: Tab reaches each can (it shows a 1px accent ring); Enter or Space on it asks. Delete on a library group's row in the list asks too, and does not delete canvas nodes then (the row's `onShortcutOverride`). Focus returns to the list when the question closes.
- Not checked by a person: the can's look on a light theme and a hover on a real pointer; a panel at its narrowest width with a long group name (the name elides before the can).

## Components

Reusable controls other surfaces may call. A component names its input and output here, knows nothing of its caller, and a caller's glue lives in its own file.

### Color picker (#52, #122)

- Files: `src/canvas/ColorPickerPane.qml` (the picker), `ColorNumberField.qml` (a label and a digits-only field), `src/canvas/ColorPickerPopup.qml` (the pane on a popup), `src/menu/color_history.h` (`App.colorHistory`: the recent list and the color math for QML), `src/engine/layout_color.h` (the same in plain C++: `normalizeHex`, `hsvFromHex`, `hexFromHsv`, `rgbFromHex`, `hexFromRgb`, `hsbRounded`, `RecentColors`; tests `tests/engine/test_layout_color.cpp`). Canvas glue: `src/canvas/LayoutColorPicker.qml` and `src/menu/layout_colors.h` (node and Frame coloring, the first caller).
- Input: `setColor(c)` starts it on a color ("#rrggbb", "#rgb", or without the "#"; a bad value is ignored); that color is also the "current" swatch. `remember` (true) and `showRecent` (true).
- Output: `color` (read, the color being chosen now, follows every drag and key), `picked(color)` on a confirm: OK, Enter in a field or on the field or slider, or a click on a recent color; and `canceled()`: Cancel or Escape. A confirm puts the color first in `App.colorHistory.recent` when `remember` is true. The caller decides what picking does; the popup closes on either signal.
- Popup: `ColorPickerPopup.openWith(color)`, `picked(color)`; Cancel, Escape or a press outside closes it with no `picked`; it has no open or close fade.
- Look: the saturation and brightness field (48u square, hue color at the top right, white to the left, black below), the hue slider (6u wide) beside it, the New swatch over the Current one with captions (a click on Current puts the start color back), H S B and R G B fields (digits only, capped at 360, 100 and 255; H and S and B show ° and %), the hex field, the recent colors under a caption (6u swatches), and Cancel and OK (`ViewButton`, 28px, OK highlighted) at the bottom right. Handles are double rings (dark, white, dark) with a soft shadow; the fields and sliders have a sunk 1px edge; hover enlarges a handle and rings a swatch; focus is the accent ring. Every color is a `look` token, so it follows light and dark themes.
- Keys: the field, the hue slider, every number field, the hex field, the recent colors, Cancel and OK take Tab focus. On the field, Left and Right set saturation and Up and Down brightness, 5 steps (Shift: 1); on the slider, Up and Down turn the hue; Enter anywhere confirms; Escape cancels. A hex value that is not a color is drawn in the error color and OK is disabled.
- Sync: the hue, saturation and brightness are kept as typed or dragged, the fields show them rounded, so a gray keeps its hue; the field being typed in is never rewritten until it loses the focus.
- A new caller: a `ColorPickerPane` or `ColorPickerPopup`, `setColor`/`openWith` with its current color, `onPicked`, and `onCanceled` for a pane. Needs `App.colorHistory` (null in a window made without it: the picker then picks nothing).

### Large view and crop frame (#73)

- Files: `src/canvas/LargeViewDialog.qml` (a modal frame with a bar: its `tools`, Fit, 100%, close; the `body` and an optional `footer`; keys 0, 1, + and -, Enter as `confirm`), `ViewButton.qml` (its 28px button), `CropFrame.qml` (the frame over any picture), `CropView.qml` (CompareView in its `single` mode with a CropFrame), `CropGeometry` (`src/canvas/crop_qml.h`, the geometry for QML over `src/engine/crop_geometry.h`). Glue: `ImageDialog.qml`, `CropDialog.qml`, `CropNodeView.qml`; C++ `CanvasModel::cropRect`, `setCropRect`, `pictureAt`, `CanvasRuns::imageUrl`.
- LargeViewDialog input: `look`, `name` (the prefix of the objectNames of its bar), `zoomable` (anything with `fit()`, `actualSize()`, `zoomAt()`, `actual`, `zoom`), `tools`, `footer`, the viewer as its child. Output: `confirm`; `keyPressed(event)` and `keyReleased(event)` for a key it did not take (the mask editor's); it closes itself.
- CropFrame input: `imageSize`, `originX`, `originY`, `zoom` (where the picture's corner is in the frame's coordinates, and view units per pixel), `rect` ({x, y, width, height}), `ratio` (0 = free), `editable`. Output: `committed(rect)` at the end of a drag, `changing(rect)`, `pressed`, `activated`. It takes only presses on its handles and inside the frame.
- CompareView `mode: "single"` is one picture with the wheel, drag, Fit and 100%; `showZoom` adds the zoom to the size notice; `imageX`, `imageY` and `imageScale` give a picture's place for what is drawn over it.
- A new caller: a `LargeViewDialog` with a viewer inside, or a `CropView` with the picture, its pixel size, a `rect` and `onCommitted`; the geometry is `CropGeometry` or `crop::` (tests: `tests/engine/test_crop_geometry.cpp`).

### Mask editor (#172)

- Files: `src/canvas/MaskEditor.qml` (the dialog), `src/canvas/mask_layer.{h,cpp}` (`MaskLayer`, a QQuickItem: 256px texture cells, only the cells a stroke touched are uploaded), `src/engine/mask_paint.{h,cpp}` (the pure core: dabs, shapes, invert, clear, fill, undo by 64px tiles, PNG and scaling; tests `tests/engine/test_mask_paint.cpp`, `tests/canvas/test_canvas_mask_editor.cpp`).
- Input: `imageSource` (url), `imageSize` (the picture's pixels), `maskPath` (a stored 8-bit gray PNG, or empty; another size is scaled to the picture), `look`; `stem` (the saved file's name start; the picture's plus `-mask`), `folder` (default: the inputs folder). Then `open()`; each open starts from `maskPath`.
- Output: `saved(path)` on OK or Enter: the mask stored by `storeImage` as `<stem>-<10 hex>.png` (the same mask is one file), or an empty path when nothing is painted (no mask); `canceled()` on Cancel, the close button or Escape. It knows nothing of nodes.
- A new caller: a `MaskEditor` with `look: App.theme.look`, set the picture and the mask, `open()`, `onSaved: path => ...` to write the path where it keeps the mask (one canvas undo step for the session), `onCanceled` for nothing.

### Compare view (#62)

- Files: `src/canvas/CompareView.qml` (the viewer), `src/canvas/CompareNodeView.qml` (glue to a Compare Images node and the model's `compareView` state), `src/canvas/CompareDialog.qml` (the large view). C++: `CanvasModel::compareView`, `setCompareView`, `compareStep`.
- Input: `sourceA`, `sourceB`, `sourceDifference`, `sizeA`, `sizeB`, `labelA`, `labelB`, `look`, and the view: `mode` ("slider" the primary and default, "slider_horizontal", "side_by_side", "flip", "difference"), `position`, `showB`, `zoom` (1 = fit), `actual` (1:1), `centerX`, `centerY`. `interactive`, `hoverWipe`, `chrome` and `wheelModifiers` are the caller's choices.
- Output: `viewChanged(current)` on every change a person makes, `pressed` and `activated` (a double click). Functions: `setView`, `apply` (takes a state without announcing it), `flip`, `nextMode`, `fit`, `actualSize`, `zoomAt`, `grab(callback)`.
- Pointer: in a slider mode the divider follows the pointer and a drag; a middle drag or Shift+drag pans. In the other modes a drag pans and, in flip, a click flips. The wheel zooms at the pointer (the node's face asks for Alt, so a plain wheel keeps moving the canvas). The arrow keys move the divider (Shift: 10%), Home and End put it at the ends.
- A new caller: a `CompareView` with its sources and sizes, `onViewChanged` to keep the state, `apply(state)` to give it back.

### Move grip (#120)

- Files: `src/canvas/MoveGrip.qml` (the handle: a drag and the arrow keys), `src/engine/float_place.h` (`fugue::place`: the math, plain C++, tests `tests/engine/test_float_place.cpp`), `src/canvas/float_place_qml.h` (`FloatPlace`, the singleton QML calls).
- Input: `areaWidth`, `areaHeight`, `boxWidth`, `boxHeight`, `margin` (kept from the area's edges), `place` (a point of fractions, 0 to 1 of the room the box has: 0 at the start edge, 0.5 in the middle, 1 at the far edge), `step` (an arrow key; Shift is five times), `snapDistance` (a drag within it of an edge or the middle is pulled there). Its children are the handle's look.
- Output: `position` (read: where the box is now, the pointer's during a drag; the caller binds the box's `x` and `y` to it), `dragging`, `placeRequested(place)` at the end of a drag and on each key. The caller keeps the place and gives it back, so a resized area keeps the box where it was relative to the edges.
- A new caller: a `MoveGrip` inside the box, the area's size and the box's size bound to it, `x: grip.position.x`, `y: grip.position.y`, a name for assistive tools, and the place kept wherever the caller keeps state (`RunBar::place` is one). For a panel hung from the box, `FloatPlace.panelPlace` and `roomOnSide`.

### Run toolbar and queue panel (#60)

- Files: `src/run/RunToolbar.qml` (the bar), `src/run/RunQueuePanel.qml` (the panel), `src/canvas/ViewButton.qml` (their button, now with a `glyph` and a `tip`). C++: `src/run/run_queue.h` (`App.runQueue`: the rows and the operations), `src/run/run_bar.h` (`App.runBar`: shown, place, panel open, and their commands), `Queue::moveToFront` in `src/engine/queue.h`.
- RunToolbar input: `look`, `busy`, `canCancel`, `canRun`, `pending`, `progress`, `batch`, `maxBatch`, `place` (a point of fractions, 0 to 1, of the room it has in its parent: {0.5, 0} is centered at the top), `queueOpen`. Output: `runRequested`, `cancelRequested`, `batchRequested(n)`, `queueToggled`, `placeRequested(place)`, and `menuAboutToOpen` (the caller sets `menuItems`, a CommandMenu's rows, as the split menu shows them). It fills what it floats over; `surface` is the bar, for a caller that places something by it.
- RunQueuePanel input: `look`, `entries`, `history` (the row shapes of `run_queue.h`), `maxHeight`; `wantedHeight` (read) is the height it wants, for a caller that picks the side it hangs on (`Main.qml` does, with `FloatPlace.panelPlace` and `roomOnSide`). Output: `cancelRequested(run)`, `frontRequested(run)`, `resultsRequested(run)`, `folderRequested(run)`, `closeRequested`. It knows nothing of where a run came from.
- Keys: Run, Run first, Run to here, Cancel and the panel (Alt+Q) are commands with keys. On the grip, the arrows move the bar (2u, Shift 10u) and it stays inside the window; Tab (or, once a button has the focus, Left and Right) move along the bar's controls once one has the focus, Up and Down along the panel's. Tab is the window's own key, so the arrows do it. Escape closes the panel while it has the focus (the run's cancel keys sleep then, `CanvasRuns::yieldKeys`).
- A new caller: a `RunToolbar` and a `RunQueuePanel` in a Loader, bound as `src/Main.qml` binds them; any `RunQueue`-shaped rows will do.

## Unread

Settings, the shortcuts overlay, the quit question, About, every menu except Run, the context menus, the workflow dialogs and their questions (those questions are a row of buttons), the group dialogs, image save, the widget pickers (but their search field), the magnifier.

### 2026-11-21 align and distribute (#58)

- Control: the Node menu, after Pin: `Align` (Left, Right, Top, Bottom) and `Distribute` (Horizontally, Vertically) submenus, and the same two in the node's context menu after Colors (`src/menu/menus.cpp`; commands `node.align-*`, `node.distribute-*` in `src/menu/menu_commands.cpp`).
- Old: none.
- New: two submenus, no default keys. In the node's menu a command shows only when it applies (Align from two selected nodes, Distribute from three), and a submenu with nothing left is left out; in the bar they stay and dim, as the other Node items do.
- Why: ComfyUI offers both as menu items, so recognition not recall; six leaf commands under two headings keep the Node menu from growing by six rows (Hick).
- Rule: Learned place (the existing Node and node-menu items keep their order; the new rows sit after the toggles, before the image items). Gaps (the two rows join the toggles' chunk; no gap added). Floor (menu rows are the menus' existing height). Destructive: not applicable, and undo reverses each.

### 2026-11-21 color picker OK and Cancel, richer look (#122)

- Control: the color picker (`ColorPickerPane.qml`, in `ColorPickerPopup`): `colorOk`, `colorCancel`, `colorField`, `colorHue`, `colorPreview` and `colorCurrent` (the swatches), `colorChannel-H|S|B|R|G|Blue`, `colorHex`, `colorRecent-<n>`.
- Old: a hue and saturation disk with a brightness bar, an unlabeled Apply pill beside the hex field, one swatch, the recent colors with no caption.
- New: the field and slider layout of Adobe's picker, a New and Current swatch pair, six number fields beside the hex one, a captioned recent row, and Cancel and OK as labelled buttons at the bottom right (Cancel first, as CropDialog and the large views have them). Enter and Escape still confirm and cancel. The popup is 94u wide (about 376px).
- Why: Apply was the one answer and had no name; a person who has seen another picker looks for OK and Cancel (recognition, Hick). Seeing the new color against the current one makes the change visible before it is made, so the one decision (OK) is informed. A field is faster than a disk for aiming at a bright saturated color (Fitts: 192px square, any point is a color), and numbers give a way in for a person who has a value.
- Rule: Floor (every target is 24px or more: the swatches 24px, the number and hex fields and buttons 28px, the field 192px, the hue slider 24px wide; the handles are marks, not targets). Primary (OK is the one highlighted control; Cancel is plain). Destructive: not applicable, Cancel loses nothing that was applied, and undo reverses OK. Gaps (1u inside a chunk: label and field, swatches; 2u between the field and slider and between the field columns; 3u between chunks: the picking area, the recent colors, the buttons row). Learned place (OK last at the right, Cancel before it, as the other dialogs). Posture: a transient popup, two decisions at most (pick a color, OK). A click on a recent color still applies and closes at once, as before (#52): it is the quick path, and the picker is open to see before it.

### 2026-11-21 color picker for nodes and Frames (#52)

- Control: the last row of the Colors menu (Edit > Colors, the node and Frame context menus and the selection toolbar's color button), `node.color.custom`, "Custom…", after a separator (`src/menu/menus.cpp`); and the popup it opens, `ColorPickerPopup` through `LayoutColorPicker.qml`: the disk 40u (160px), the brightness bar 7u (28px) wide beside it, a row of the color's preview (7u), the hex field (7u tall) and Apply (7u tall, highlighted), and under it the recent colors, 7u swatches 1u apart, six to a row, up to 12.
- Old: nine theme roles and "No color"; no other color could be given.
- New: the row above, enabled with a selection or on a Frame. The popup opens centered with the hex field focused, started on the target's hex color, else the last one used, else the accent. Nothing changes until Apply, Enter, or a click on a recent color; that is one undo step. Escape or a press outside leaves it as it was.
- Why: a person who wants a color the roles lack picks it by sight (the disk, as Blender's picker) or by value (the hex field); the colors used lately are recognized, not recalled (Hick), and one click re-applies one. The roles stay the quick path in the same menu, the order of the menu above is unchanged.
- Rule: Learned place (the nine roles keep their order; the new row is last, after a separator, the chunk gap). Floor (every target is at least 24px: swatches, field, button and bar 28px; the disk 160px; the wheel's handle is a mark, not a target). Primary (Apply is the popup's one highlighted control). Gaps (1u inside a row, 2u between the disk and the bar, 3u between the three chunks, more than twice 1u). Posture: a transient popup, two decisions at most (pick a color, apply it). Destructive: not applicable, undo reverses it.

### 2026-11-21 run toolbar moves freely (#120)

- Control: `runToolbarGrip` (a `MoveGrip`, 6u = 24px wide and 7u = 28px tall, was 4u wide), and the bar it moves, `runToolbar`; the panel `runQueuePanel` follows the bar.
- Old: the grip moved the bar between the top and bottom edges, centered; Up and Down on the grip docked it, Left and Right on it moved focus along the bar's buttons.
- New: a drag moves the bar anywhere in the window, 4u from the edges at the nearest, pulled lightly (3u) to the edges and the middle; the arrows on the grip move it 2u (Shift 10u); the place is kept as fractions of the room it has, so a resized window keeps it inside and where it was relative to the edges. The panel hangs under the bar when there is room, else over it, centered on the bar and held inside the window. Left and Right on the grip no longer step along the buttons; Tab does, and Left and Right do once a button has the focus.
- Why: the owner used #112 in the real window and wanted it out of the way of the nodes it covers (Fitts: the bar goes where the work is not; a person puts it near the work they repeat). The pull to the edges and middle gives the old docked places back without aiming (the edge is a target of infinite depth). The grip was under the floor on its narrow side; it is now the only way to place the bar, so it meets it.
- Control: `ViewButton.danger` (new, false): the toolbar's Cancel and the queue rows' `queueCancel-<run>` are red-tinted (`danger-fill`, `danger-hover`, `error` ring; canvas-look 11) while enabled; disabled they stay the plain dimmed well.
- Why (danger): a run in progress makes Cancel the control a person may need fast, and red is the destructive signal already used for errors (Destructive rule: chunk, gap, undo are unchanged; color is added on top, with the label kept, so it is not the only cue). Text reads at 4.5:1 on both fills.
- Rule: Floor (the grip is now 24 x 28px; the 4u margin keeps the bar from the window edge, where a press would also hit the window's resize area). Primary (unchanged: Run and its arrow). Destructive (unchanged: Cancel keeps its own chunk). Gaps (unchanged; the grip is 2u wider, chunks stay 3u apart). Learned place (the bar starts centered at the top as before and ComfyUI's action bar is draggable; the batch count, Run, Cancel order is unchanged). Posture: the bar is canvas furniture; the panel is transient and follows it.

### 2026-11-21 run toolbar and queue panel (#60)

- Control: the run toolbar (`RunToolbar.qml`, `runToolbar`), floating over the canvas, centered at the top or bottom edge: grip (`runToolbarGrip`), batch count (`runBatchLess`, `runBatchCount`, `runBatchMore`), Run and its arrow (`runButton`, `runMenuButton`), Cancel (`runCancelButton`), the queue button (`runQueueButton`); the panel (`RunQueuePanel.qml`, `runQueuePanel`) with a row per run, `queueFront-<run>` and `queueCancel-<run>` on the runs going and waiting, `historyResults-<run>` and `historyFolder-<run>` on the finished ones. In View > Bars and panels (#129): `panel.queue` (Alt+Q) and `view.toggle-run-toolbar`.
- Old: none; a run was started by the keys or the Run menu, and only the pill said that one was going.
- New: as above. The toolbar and panel are made only while shown (Loader), and the toolbar not in a window without runs: the window keeps its item budget (#5).
- Why: Run is the main action and the pointer reaches it at once (Fitts: a bar at an edge the canvas leaves free, 28px buttons); Cancel is next to it but in a chunk of its own, so aiming at Run never lands on it. The panel is a list read by recognition (what is going, in what order, since when), with the two actions that change the order one press away on the row, and the click on a finished row doing what a person wants of it (see its results).
- Rule: Floor (every button is 7u = 28px; the grip is 4u wide but 7u tall and has the keys; the tooltips name the glyph-only ones). Primary (Run and its arrow are the toolbar's only accent buttons; the panel has none: its rows are equal). Destructive (Cancel is its own chunk, 3u from the chunks on both sides while the gap inside a chunk is 1u; on the panel's rows the x is the last button, after the move-to-front arrow, 1u as the neighbors but the arrow is dimmed on the first waiting run so the two never swap places). Gaps (1u inside a chunk, 3u between). Learned place (the Run menu keeps its order; the split menu is the same rows; ComfyUI's order is batch count, Run, Cancel; the panel is under the toolbar as its sidebar is). Posture: the bar is canvas furniture, small and always the same; the panel is transient and larger (rows 9u).

### Side panel (#56)

- Files: `src/panels/SidePanel.qml` (the dock container), `src/panels/side_panels.h` (`App.sidePanels`: which panel is open, its side and width, kept in the state folder; the commands `panel.nodes` and `panel.workflows`). Contents, each its own file and knowing nothing of the container: `NodeLibraryPanel.qml` (rows from `src/search/node_library.h`, `App.nodeLibrary`) and `WorkflowsPanel.qml` (tabs from `App.workflows`, files from `src/workflow/recent_files.h`, `App.recentFiles`). Glue, the only place that knows the canvas: `Main.qml` (`syncSidePanel` and the three components).
- Input: `look`, `title`, `dock` ("left" or "right"), `minWidth`, `maxWidth`, `width` (the caller's), and the content as its children (or `contentArea` for content made later). `focusTarget` is the content's item that takes the focus first.
- Output: `closeRequested`, `dockRequested(dock)`, `widthRequested(width)`. The panel changes none of them itself.
- A new panel: a content component with its own signals, a tab name in `SidePanels` (`knownTab`), a command, and a branch in `Main.qml`'s `syncSidePanel`. The container needs no change.

### 2026-11-21 player on a video or song node (#116)

- Control: `src/canvas/MediaView.qml`, in a node's picture slot (`CanvasNode.qml`, `mediaPlayer`) and in the large view (`MediaDialog.qml`, `mediaViewer`): the bar's `mediaPlay`, `mediaSeek`, `mediaMute` and `mediaExpand` (a video only), and the picture's double click.
- Old: none; a video or song result showed a label ("Video · 0:04").
- New: one bar, 7u = 28px tall, over the picture: play and pause first, the time, the seek rail (the rest of the width), then mute and expand. The large view has the same bar and the close button of `LargeViewDialog`, with no Fit and 100% (a player has no zoom).
- Why: play is the one thing wanted of a player, so it is first and in the same place on the face and the large view, as the dialog's primary; the rail is the widest target, since a seek is by pointing (Fitts); a press on the picture is not play, so it can select the node and a double click can open the large view, as for a picture. Space, Left, Right and M work once it has the focus.
- Rule: Floor (every button is 28px square; the rail's press area is the bar's whole 28px height, its rail 4px). Primary (play is the first control; the others are quiet). Gaps (one `u` between the time, rail and buttons). Learned place: no menu changed. Posture: on the canvas, a control that is frequent is raised to 28px, over the node's 24px floor; the dialog is transient and starts playing at once, so it asks no decision.

### 2026-11-21 hover magnifier setting (#123)

- Control: Settings > General, `src/settings/SettingsPanel.qml` "Magnify small nodes on hover" (`hoverMagnifier`), a `SettingsParts.Toggle` (`src/settings/SettingsParts.qml`), after Corner radius, before Language. Writes `hover_magnifier` in settings.json.
- Old: the magnifier (#74) always came up on a short hover far out.
- New: off by default, so a node never grows under the pointer unasked; on, it behaves as #74. The toggle's label reads On or Off beside the switch. Applies at once; turning it off closes an open magnifier.
- Why: the owner found a node growing on hover a surprise, not a help; a surprise costs more than the wire it saves, and the choice is one the user makes once.
- Rule: Floor (the whole row, switch and label, is the press target, 7u = 28px tall; the 22x12 switch is the same drawing as Providers' "Use" switch). Primary: none, a settings page has none. Posture: a setting, no frequent action. The toggle takes Tab focus; Space or Enter flips it.

### 2026-11-21 Bars and panels menu, focus mode and hideable bars (#129)

- Control: a submenu **Bars and panels** at the end of the View menu, then **Focus mode** (H) (`src/menu/menus.cpp`, `barsMenu()`), and the same submenu in the canvas's context menu. (A top Window menu was built first and dropped: its title added 4 items to the window's budget.) Rows, each a checked command while its bar is shown: Menu bar, Workflow tabs, Run toolbar, Selection toolbar, Group breadcrumb, Run status, Zoom readout (`view.toggle-*`); then Node library, Workflows, Run queue (`panel.*`, moved from View); then Focus mode (H) and Reset bars and panels. The hint pill `chromeHint` (`Main.qml`), a line at the bottom for 4 s after the menu bar is hidden or focus mode starts.
- Old: View held the three panel rows and the run toolbar toggle; no other bar could be hidden.
- New: as above. The View menu keeps fit, zoom and links.
- Why: a list of every bar with a check is recognition, not recall (Hick), as Photoshop's Window menu; the chunks are the bars, the panels, then the two whole-window actions, the gap between chunks being a separator. The menu bar can hide itself, so the way back is shown when it goes (the pill names F10) and the canvas's menu carries the same list; F10 shows a hidden bar before it opens it.
- Rule: Learned place (the panel rows keep their order and keys, N, W, Alt+Q; they moved from View to the menu that is now for bars; the existing View rows keep theirs). Floor (menu rows keep the menus' row height; the pill is not a target). Gaps (separators between the three chunks, the menus' own). Destructive: not applicable, and hiding is reversible by the same row, F10, H or Reset. Posture: transient menu rows, no decision beyond a check. Primary: none added.

### 2026-11-21 models to download in the model picker (#134)

- Control: the model menu of any resource input (`src/canvas/widgets/WidgetMenu.qml` via `InputWidget.qml`, `modelEntries()`): after the files, a heading **Download** and one row per catalog model not on disk (title, size at the right, a second line with scale, architecture, license and what it is for). Picking one opens a question (`WidgetPicker.qml`, `downloadAsk`, a `Question`): title "Download <name>?", its description, License (the error color and "not for commercial use" when the license forbids it), Made by, Size, From (host), Saved in (folder), a line saying the file is kept only if its SHA-256 matches, then `downloadAskLicenseLink` (flat, "Read the license", at the left), `downloadAskCancel` and `downloadAskOk` ("Download", the one highlighted button, last at the right).
- Old: a model menu listed files and provider models; a missing model was fetched only from a workflow's banner or a template card, and a Load Upscale Model had nothing to list.
- New: as above. The search field also matches the architecture, license, author and tags of the downloadable rows (`keywords`). The download goes on the window's one download line, so the banner shows its progress and Cancel; when the file has arrived and its SHA-256 matched, the input holds it.
- Why: choosing a model and getting it is one act, so it happens where the model is chosen (Hick: no trip to a model library); a download is large and has a license, so it is asked once, with what decides it in view (spec 6.7: nothing silent), and never by Enter on a filtered list alone (Enter picks the row and opens the question; the question's Enter is not bound, the highlighted button is the answer).
- Rule: Learned place (the menu keeps its order: Any ready model, provider models, files, then Download, then Type a name…). Floor (menu rows are the menus' row height; the question's buttons are Material's 48px as the group questions'). Primary (Download is the question's one highlighted button; Cancel before it, as the other questions have them). Destructive: none, nothing is deleted and Cancel fetches nothing. Gaps (3u between the question's chunks, 1u inside the facts grid, 2u between the buttons). Posture: the menu is transient and keeps its density; the question is a transient dialog with one decision.

### 2026-11-21 move a node to a newer major version (#118)

- Control: the node's chip `updateChip` (`CanvasNode.qml`, the one a group shows as "Update to 1.1.0"), now also "Move to 2.1.0" on a node that runs on an older installed major of its type (`NewerMajor` role); the Node menu's and the node menu's `node.move-version` ("Move to newer version…", shown only for one selected node that has a newer major); and the question `groupUpdateAsk` (`GroupDialogs.qml`, `major` set), whose highlighted button reads "Move".
- Old: the offer was the compile warning `node_newer_major` and the CLI; moving meant editing the node's `version` in the file.
- New: as above. The chip is the group chip's place (right end, above the node's header), size and look; a click or the command asks, naming the inputs and outputs that come and go (or that they stay the same), then Move rewrites the version, one undo step. A node never has both chips (a group has its library update, a node its newer major).
- Why: it is the same act as a group's update, so it is asked the same way (recognition, Hick); the question comes first because moving to a new major can change results (spec 3.6, 6: nothing changes silently), and Cancel before Move leaves the file as it was.
- Rule: Floor (the chip is the group update chip's 16px by text width, an existing exception on the canvas, and the command is the keyboard way; the question's buttons are the group questions'). Primary (Move is the question's one highlighted button, last at the right, Cancel before it). Destructive: none, undo reverses the move. Gaps (the question's own: 3u between chunks). Learned place (the Node menu keeps its order; the new row is last, after the group rows, and shows dimmed in the bar; in the node menu it follows Update group). Posture: the chip is canvas furniture, shown only while there is something to do; the question is a transient dialog with one decision.

### 2026-11-21 templates carry their pack (#118)

- Control: the banner's existing install button (`installPack`, `WorkflowDialogs.qml`), now "Install <pack> <version>…" for a pack the opened template carries; the template card's status line, "Carries the pack to install: <id>" beside the models line.
- Old: the button offered only the `requires.packs` source of a missing pack; the card said "Packs needed".
- New: a template folder with `packs/<id>/` is offered from that folder, first, with a line saying it carries the pack and that nothing is trusted until allowed; the click goes to Settings > Packs, which asks to install and, later, to trust (the same questions as any pack).
- Why: it is the control that already exists for a missing pack, so no new place to learn; the question stays on the Packs page, never in the banner (spec 6.6, 6.7: nothing silent).
- Rule: Floor, Primary, Destructive, Gaps, Learned place and Posture: unchanged, the button and the card keep their size, place and order; only the text differs.

### 2026-11-21 Cache page: Clear all…

- Control: `cacheClearAll`, a `SmallButton` "Clear all…" (28px tall) in the Cache page's header row, right of the "N in all" total (`CachePage.qml`). It opens the same `cacheQuestion` (`cacheYes` highlighted, `cacheNo`) with the total size and all three losses in words.
- Old: only the three per-kind "Clear…" buttons.
- New: as above; off while measuring or clearing and when nothing can be cleared.
- Why: the page's big action sits with the number it acts on, at the top-left, away from the per-kind Clear… at the right edge of the rows (Destructive: not next to a frequent control, and not in line with the others, so it is not hit for a row's); it never acts on the first click (asks first).
- Rule: Floor (28px). Primary (the question's Clear is the one highlighted button; Clear all… is plain). Destructive (a second click, the question). Gaps (4u from the total). Learned place (the per-kind Clear… and the question are unchanged).

### 2026-11-21 Cache page and Clear image preview cache (#141)

- Control: Settings > Cache, the eighth page (`src/settings/CachePage.qml`, made only when first shown): a list of the three kinds (`SizedList.qml`: title, a line, the size, a share bar, `cache-action-<n>` "Clear…"), the question (`cacheQuestion`: what is lost, `cacheYes` highlighted, `cacheNo`), `cacheMessage`, then the limits as choices (`cacheResults-*`, `cacheThumbnails-*`, `runsKept-*`). Command `cache.clear-previews` ("Clear image preview cache…", no key) in the Edit menu, between `edit.clear` and Settings, each in its own chunk: it opens the page on the question for the previews.
- Old: none; nothing could be cleared or limited from the window.
- New: as above. Clear never acts on the first click: it asks, in words ("nodes run again", "pictures show after the next run", "the history is gone"), with the size.
- Why: the sizes and the verb sit in one row (recognition; the person sees which kind is big), and a destructive step is a second, separate click on a different place (Destructive). The question is the Packs page's, so it is learned once.
- Rule: Floor (Clear… and the choices are SmallButtons, 7u = 28px; rows are taller). Primary (Clear in the question is its one highlighted control; the rows' buttons are plain). Destructive (its own question, 2px accent border, Cancel after it; the menu command is not the last row and has separators on both sides). Gaps (1u inside the choices, 4u between settings, 2u between rows). Learned place (the Edit menu keeps its order; the new row sits before Settings…). Posture: a transient dialog; the page is long and scrolls, the limits below the fold.

### 2026-11-21 media browser and the workflows panel's Results view (#133)

- Controls: `src/panels/WorkflowsPanel.qml` (`workflowsView-files`, `workflowsView-results`, `workflowsResultsRefresh`); `src/media/MediaBrowser.qml` (`mediaMode-grid|filmstrip|single`, `mediaInfo`, `mediaAction-open|folder|copy`), the grid cells `MediaThumb.qml` (`mediaCell-<n>`), the filmstrip cells and the large item `MediaSingle.qml`.
- Old: the workflows panel was the file lists only.
- New: a row of switch buttons at the top (Files, Results, then Refresh while Results shows), under it the browser's bar (Grid, Filmstrip, Single, Info, the count), the grid or the large item, and the actions in the last row. Every button is a `ViewButton`, 7u = 28px tall. A grid cell is 24u = 96px square, a filmstrip cell 16u = 64px, both well above the floor.
- Rules: Floor (all targets 28px or more). Gaps (1u inside each row; the rows are the chunks, separated by 1u as the panel's lists are, the switch row above the list by 2u). Primary: none on this surface; browsing has no main action, and Open is the first of the three actions on the current item, in the order of how often a file is wanted. Destructive: none (delete is a follow-up and will be its own chunk). Posture: a docked panel, so the canvas's density.
- Keys: the browser takes Left, Right, Up, Down, PageUp, PageDown, Home, End, Enter, Escape and I through `onShortcutOverride`, so the window's own shortcuts (Home fits the view) do not take them while it has the focus; Escape goes back to the grid, then closes the panel.
- Not checked by a person: the layout at the panel's narrowest width (200px: the buttons wrap) and in a light theme.

### 2026-11-21 fit on open, clear of the run toolbar (#162)

- Control: none added. `FugueCanvas.qml` `fit()` (View > Fit, `view.fit`) and the same fit when a workflow with no saved view is put in; `floatingBar` is the run toolbar's box, set by `Main.qml`.
- Old: a template opened at 100%, nodes cut off at the edge and under the run toolbar; Fit used the whole canvas, so the bar still covered the top row.
- New: the nodes fill three quarters of the canvas without the band the bar covers (its box and 2u, on the top or bottom edge, whichever it is on); the canvas already starts after the 44px tab strip and a docked panel. A saved view is kept.
- Why: the first look at a workflow is recognition (Hick): the whole graph is in view and none of it is behind a control (Fitts: a node under the bar cannot be reached by the pointer).
- Rule: no new control, so Floor, Primary, Gaps and Destructive do not apply. Learned place: Fit's key and menu entry are unchanged. Posture: the canvas keeps its density.

### 2026-11-21 scroll bars on every scrolling surface

- Control: `src/canvas/FugueScrollBar.qml` (`fugueScrollBar`), on the lists and panels named in canvas-look 16.1; replaces the plain Material `ScrollBar {}`, the AlwaysOn-when-long copies and the CommandMenu's ScrollIndicator. The widget menu's list also takes Home and End when it has the keys (a menu with no search field).
- Old: the model picker, the other widget menus, the command menus, the multiline editor, the run queue, the group editor, the media details and the filmstrip scrolled with no visible bar; some panels had one in the style's default look, hidden until moved.
- New: one themed bar, visible whenever the content overflows, a draggable thumb of at least 32px, a press on the track pages.
- Why: a long list (hundreds of model files) was found by wheel or arrow keys only; a bar shows where the person is and drags to the far end at once (Fitts: the thumb is a target as long as the list is long).
- Rule: Floor, with an exception: the bar is a 10px gutter along the whole edge of its view, hit along its length, a 24px circle on it would cover the rows it borders; the wheel and keys do the same scrolling. Reversed 2026-11-21: the first version let the bar lie over the content and took no width (only the widget menu padded for it), which cut off the last key chip of every row of the Shortcuts overlay and put the library's thumb under the panel's width grip, so it could not be dragged. The bar now reserves its gutter: `inset` (its breadth while it shows), and each host pads its content by it; the side panel's body stops short of the grip. Tests: `tests/scroll_checks.h` (no text or image under the bar, wheel and thumb drag scroll). Posture: no new decision, nothing to choose.

### 2026-11-21 the wheel on every scrolling surface, and one themed tooltip

- Control: the wheel handler inside `src/canvas/FugueScrollBar.qml` (every view that has the bar scrolls by it) and `src/canvas/ToolTipStyle.qml` (made once by `Main.qml`; the one look of every `ToolTip.text` / `ToolTip.visible` in the window), with `src/canvas/TipGate.qml` on the node library's and the workflows panel's lists.
- Old: the wheel did not scroll a list in the real window. On the owner's Wayland session an ordinary mouse wheel is a device of type TouchPad with no scroll phase (angle 120, pixel 15 per notch): the library's `WheelHandler` took the default devices and never heard it, and a Flickable's own wheel handling did not move for it either (tests/scroll_checks.h `waylandWheelAt` reproduces it). A tooltip was as wide as its text: a node's description was one 1500px line above the row, delayed 600ms, and a tooltip fading for 500ms under a pointer that kept scrolling took the wheel events.
- New: the bar's handler scrolls its view for a Mouse or a TouchPad device: a notch is 60px, a touchpad (told by its scroll phase alone, as the canvas does) moves by its pixels; the touchpad path and the thumb drag are unchanged. Every tooltip is wrapped at 80u (320px at the default text size, about 45 characters), never wider than the window less its margins, shown below its control with the left edges aligned, after 700ms (`look.tooltipDelay`; each site sets `ToolTip.delay` to it), and it goes at once (no fade out). The library's and workflows lists hide a row's tooltip when the list scrolls, until the pointer moves.
- Why: Fitts and steering: a tooltip over what the person is aiming at, or lying where the scrolled row was, is an obstacle in the path; a long line is not read (the eye returns to the line start: lines of 40 to 50 characters read best), and a short delay shows it by a deliberate rest, not on the way across (Hick: the pointer passing over twelve rows should not open twelve). Below, not beside: the pointer comes from above or the side, the hovered row stays readable, and the rows that tooltip covers are the ones not being read.
- Rule: Floor (no new control; the tooltips are not targets). Primary, Destructive, Gaps (1u between the control and its tooltip), Learned place (desktop tooltips: Qt, GNOME and KDE show them below the control after about 500 to 1000ms), Posture: transient, never takes the pointer's events. Tests: `theWheelScrollsWhileARowTooltipShows` and `aRowTooltipIsWrappedAndInsideTheWindow` (tests/panels/test_side_panels_window.cpp), `scrollsByWheelAndThumb` for every page of the settings and every menu. The masking editor's own tooltip (`MaskEditor.qml`) keeps its 600ms until the masking work lands.

### 2026-11-21 where a model lives: badges on template cards and model rows, Ready to run (#164)

- Control: `src/canvas/PlaceBadge.qml` (`templatePlaceLocal-<id>`, `templatePlaceHost-<id>`, `widgetMenuPlace`), a 16px `monitor` (this machine) or `server` (a paired host) glyph with a tooltip, on a pill at the lower left of each Templates card's preview (`templatePlaces-<id>`) and left of the detail text of each model menu row; the **Ready to run** chip (`templatesReadyOnly`, 7u tall, a toggle: a check and the accent edge when on) right of the Templates search field.
- Old: a card said only what it lacked ("Models missing"); a model row said where it lived only by the heading it sat under, and an offline host's models read "not running".
- New: as above. A template with models on both places shows both badges; none when none are found (the "Models missing" line stays). An offline host's badge and row are dimmed, the row says "host offline". With the chip on, only templates whose models are all somewhere show, and within a category the ready ones come first.
- Why: where a model lives decides whether a template can run now, and a glyph is read faster than a line (recognition, not recall: Hick); the tooltip carries the host's name so the glyph is not the only signal; the pill keeps the card the same height, so nothing reflows when the hosts answer after the dialog opens.
- Rule: Floor (the badges are informational, they take no press, so no 28px floor; the chip is 7u = 28px tall and the whole chip is the press target). Primary (none: the chip is a toggle, the dialog's action is opening a card). Destructive (none). Gaps (1u between badges; 2u between the search field and the chip). Learned place (the search field keeps its place at the top; the chip sits on the search's row at the right, as ComfyUI's filters sit by its search). Posture (a dialog read at arm's length: the glyphs are 16px, the same as the other row glyphs; the tooltip names the host).
