# Cohesion audit: the canvas and areas surfaces

Date: 2026-11-22. Project: `~/github/fugue-cohesion` (branch `cohesion-audit` at `eb3e7e73`, the same code as `main`). Issue #228.

Sizes are device-independent pixels at `textSize` 12, where `look.unit` (`u`) is 4 (`src/look.cpp`). Measured **from source** (file:line on that commit) and from the offscreen pictures the tests already save (list under **Pictures**). A gap written "4u" is the real distance: a positioner's `spacing` counts on both sides of a spacer `Item`, so `Row { spacing: u }` with `Item { width: 2u }` between two buttons puts them 4u apart.

The question here is not "does each control meet the rules" (the **Decisions** rows of [docs/ui-ergonomics.md](../docs/ui-ergonomics.md) answer that per control) but "does the same job look and behave the same everywhere". Each new surface is compared with a long-standing one that does the same job.

## Scope

| Surface | Path | Compared with |
| --- | --- | --- |
| Canvas history dialog | `src/canvas/CanvasHistoryDialog.qml` | Compare Images large view (`CompareDialog.qml`), the Canvas view |
| File > Canvas… dialog | `src/canvas/CanvasViewDialog.qml` | Mask editor and paint window (`MaskEditor.qml`), Crop window (`CropDialog.qml`) |
| Canvas view: bar, strip, tools, banner, picture labels, region frame, notes | `src/canvas/CanvasView.qml` | Mask editor bar, Crop window, workflow banner (`WorkflowDialogs.qml` `workflowNotice`) |
| Region picker | `src/canvas/RegionDialog.qml` | Crop window |
| Canvas picker: well, menu, New canvas question | `src/canvas/widgets/InputWidget.qml` `canvasEntries`, `WidgetPicker.qml` `newCanvasAsk` | Model picker menu, download question (`downloadAsk`) |
| Area header, view-type menu, border handle, placeholder | `src/workspace/AreaHeader.qml`, `AreaHandle.qml`, `AreaPlaceholder.qml`, `CanvasArea.qml` | Side panel title bar (`SidePanel.qml`), side tab strip, paint layers panel |
| Close button, project chip | `src/menu/MenuBar.qml` | Side panel close, tab close, large view close |
| Settings rows Reopen tabs at start, Show new canvas results | `src/settings/SettingsPanel.qml:241-268` | Magnify small nodes on hover (same page) |
| Projects dialog, chip, origin tags | `src/settings/ProjectsDialog.qml`, `SettingsParts.qml:14-55` | Settings panel, Cache question, delete-group question |

Not in scope: the layers panel (not merged).

## How the long-standing surfaces do each job

The reference column every finding below is measured against.

| Job | Reference | How |
| --- | --- | --- |
| Button in a picture window | Mask editor, Crop, Compare Images | `ViewButton` 28px, `well` fill, no edge, `labelSize`, 14px Lucide glyph before the label |
| Tool button (glyph only) | Mask editor tools, paint layers panel, side panel title | `ViewButton { glyph; tip }` 28px square, tooltip names the key: "Brush (B)" |
| Settings and dialog button | Settings, Cache page, Projects | `SettingsParts.SmallButton` 28px, `well` fill **with** a `hairlineStrong` edge |
| Question button | Download, group, delete-group questions | `ThemedButton` (Material, 48px), Cancel then the highlighted answer at the right |
| Primary | Crop OK, Mask OK, Run | The one accent-filled control, last at the right of its row |
| Destructive | Mask Clear, Run Cancel, paint Remove | `danger` tint, its own chunk, at the end of its row |
| Commit a rectangle | Crop window | OK (primary), Cancel before it, Enter applies, Escape cancels |
| Close a window | Side panel `sidePanelClose` | `ViewButton { glyph: "x" }` 28px |
| Chevron for a menu | Choice and model wells, area type menu | Lucide `chevron-down` (`Glyph`) after the text |
| Status word on a row | Settings "Inherited from Default", Projects "Current" | caption size, DemiBold, accent or error, capitalized |
| Notice with actions | `workflowNotice`, `helperNotice` | `popup` fill, `hairline` edge, 8px padding |
| Empty state | Templates, node library | says what to do in the app |
| Keys of a picture | Every `LargeViewDialog` | 0 Fit, 1 100%, + and - zoom, Escape closes, Enter confirms |
| Keys of the graph | `default_commands.cpp:30-75` | `.` and Home fit, Ctrl+= and Ctrl+- zoom |
| Focus | `ViewButton`, side tab strip | 2px accent ring inside the control |
| Right-to-left | Settings panel, Projects dialog, group dialogs, `?` overlay | `LayoutMirroring.enabled` on the popup's content |

## Findings

Severity: **high** breaks a written rule or will mislead a person; **medium** the same job looks or acts differently in two places; **low** polish. Each finding names the file and line on `eb3e7e73`.

### Fix now (small, safe, not in `CanvasView.qml`)

#### N1. The large views close with a text "×"; every other close is the Lucide `x` (low)

- File: `src/canvas/LargeViewDialog.qml:171-176` (`text: "×"`); the same text glyph in the helper notice, `src/Main.qml:170`.
- Reference: side panel close `src/panels/SidePanel.qml:95-99` (`ViewButton { glyph: "x" }`), menu bar close `MenuBar.qml:304-311`, area close `AreaHeader.qml:204-210`. In `canvas-view-candidate.png` the two × glyphs (menu bar, dialog) are different sizes and shapes on one screen.
- Fix: `ViewButton { glyph: "x"; tip: qsTr("Close (Esc)") }`. Every large view (Crop, Mask, Compare, Image, Region, History, Canvas) gets it at once.

#### N2. Fit and 100% have no tooltip naming their keys (low)

- File: `src/canvas/LargeViewDialog.qml:152-169`.
- Reference: the Mask editor tips name the key ("Undo (Ctrl+Z)", `MaskEditor.qml:241`); the area header buttons do too (`AreaHeader.qml:34-37`).
- Fix: `tip: qsTr("Fit (0)")`, `tip: qsTr("Actual size (1)")`.

#### N3. A `ViewButton` used as a toggle does not tell assistive tools it is on (medium)

- File: `src/canvas/ViewButton.qml:40-41` (role Button, no checked state). Toggles that use it: Show candidate, Compare, Snap (`CanvasView.qml:221-266`), History Compare (`CanvasHistoryDialog.qml:76-85`), Region Snap (`RegionDialog.qml:117-129`), the paint layer eye.
- Reference: `SettingsParts.Toggle` sets `Accessible.role: CheckBox` and `Accessible.checked` (`SettingsParts.qml:68-70`).
- Fix: a `checkable` property on `ViewButton`; when set, `Accessible.checkable: true` and `Accessible.checked: current`. Callers that toggle set it.

#### N4. History's back button draws its chevron as text (low)

- File: `src/canvas/CanvasHistoryDialog.qml:71` (`"‹ Canvases"`).
- Reference: the side panel's dock button uses `glyph: "chevron-left"` (`SidePanel.qml:91`).
- Fix: `glyph: "chevron-left"; text: qsTr("Canvases")`.

#### N5. Two hover colors in the History dialog (low)

- File: `src/canvas/CanvasHistoryDialog.qml:160` (canvas rows hover in `selectedRow`) against `:204` (revision rows hover in `menuRow`, selected in `selectedRow`).
- Reference: node library rows (canvas-look 11: `menu-row` hover, `selected-row` current).
- Fix: canvas rows hover `menuRow`.

#### N6. The History canvas list cannot be used from the keyboard (medium)

- File: `src/canvas/CanvasHistoryDialog.qml:154-172` (rows have a `TapHandler` only; `onKeyPressed` at `:56-63` moves the revision list only).
- Reference: the revision list in the same dialog (Up and Down), the Templates categories (Tab, Up, Down, Enter).
- Fix: Up and Down move a current canvas row, Enter opens it, as the revision rows do.

#### N7. The region picker commits with "Apply"; the Crop window and the Mask editor say "OK" (low)

- File: `src/canvas/RegionDialog.qml:263-270` (`regionApply`, "Apply", closes the dialog).
- Reference: `CropDialog.qml:136-142` (`cropOk`, "OK"), `MaskEditor.qml:549-554`. Both close on the press. "Apply" in other apps means "apply and stay open", which this button does not do.
- Fix: text "OK" (keep the objectName); update the #213 Decisions row.

#### N8. The New canvas size fields are sized in pixels (low)

- File: `src/canvas/widgets/WidgetPicker.qml:223` and `:232` (`Layout.preferredWidth: 90`).
- Reference: the Crop and Region fields, `14 * look.unit` (`RegionDialog.qml:238`).
- Fix: `22 * ask.look.unit` (88px at 12, and it follows the text size).

#### N9. One action, three names: make a canvas from a picture (low)

- File: `InputWidget.qml:365` ("Import picture…", makes the canvas at once), `WidgetPicker.qml:251` ("From a picture…" inside New canvas), `WidgetPicker.qml:163` (file dialog title "Make a canvas from a picture").
- Fix: keep the menu's "Import picture…" (the CLI's word, `fugue canvas import`) and the question's "From a picture…" (it reads under New canvas); the file dialog becomes "Import a picture as a canvas".

#### N10. The canvas picker's actions are not a chunk of their own (low)

- File: `src/canvas/widgets/InputWidget.qml:364-370`: New canvas…, Import picture… and Pick region… follow the canvas rows with no heading.
- Reference: the model menu puts its actions under a heading ("Download", #134) and its rows under "Fits <model>" and "Other" (#85).
- Fix: a heading row before the actions (`{ heading: true, text: qsTr("Make or pick") }`, or a plain separator if `WidgetMenu` gets one).

#### N11. The area close glyph turns red on hover but not on keyboard focus (low)

- File: `src/workspace/AreaHeader.qml:91` (`button.danger && hover.hovered`).
- Reference: the menu bar close, red on hover or focus (`MenuBar.qml:308`).
- Fix: `button.danger && (hover.hovered || button.activeFocus)`.

#### N12. Three focus rings for one job (medium)

- Files: `MenuBar.qml:296-302` (1px inset ring), `SettingsParts.qml:115` (the 1px edge turns accent), `SettingsParts.qml:81` (Toggle: 1px track edge), against `ViewButton.qml:36` and `AreaHeader.qml:79` (2px ring).
- Reference: canvas-look 11, "a focused tab a 2 px accent ring inside it".
- Fix now: the menu bar close to 2px. The SmallButton and Toggle rings wait for **D4** (one button family).

#### N13. "Remove from list…" is the one reversible removal without the danger tint (low)

- File: `src/settings/ProjectsDialog.qml:241-246` and `:253-261` (`SmallButton` has no `danger`).
- Reference: Drop (`CanvasHistoryDialog.qml:131`, reversible), Mask Clear (`MaskEditor.qml:283`, undoable), paint Remove: all `danger`.
- Fix: a `danger` property on `SmallButton` (the `ViewButton` colors: `dangerFill`, `dangerHover`, `error` ring), set on both buttons.

#### N14. The placeholder area uses Material's `Label` (low)

- File: `src/workspace/AreaPlaceholder.qml:16-23`.
- Reference: every other area and panel text is `Text` with `look.labelSize` and `look.label`.
- Fix: `Text { font.pixelSize: App.theme.look.labelSize }`.

#### N15. The split buttons are drawn in fixed pixels (low)

- File: `src/workspace/AreaHeader.qml:94-109` (14 × 12, radius 2, 1.5px lines).
- Reference: every other glyph is a Lucide character at 14px (canvas-look 16), which follows `color` and batches.
- Fix: add `columns-2` and `rows-2` to the icon subset (`share/fonts/lucide-fugue.txt`) and use `glyph:`; or, until then, the drawing in `u` (`3.5u × 3u`).

### Fix after the layers panel lands (`CanvasView.qml`, or a shared component first)

#### L1. Two highlighted Accept buttons and two Drops on screen at once (high)

- File: `src/canvas/CanvasView.qml:281-299` (the bar's Accept and Drop) and `:333-349` (the banner's), both visible while `newResultShown`.
- Rule: Primary ("a new surface has one primary control"). The #225 Decisions row kept the bar's pair on purpose.
- Fix: while the banner shows, hide the bar's Accept and Drop (`visible: root.canDecide && !root.newResultShown`); the banner is where the eye is. Update the #225 row.

#### L2. The File > Canvas… dialog stacks three rows of buttons, with Fit, 100% and close alone on the first (high)

- File: `src/canvas/CanvasViewDialog.qml:335-343` (`zoomButtons: false`, nothing in `tools`) inside `LargeViewDialog.qml:142-177`; the view's own two rows at `CanvasView.qml:156-300`. Seen in `canvas-view-candidate.png`: row 1 Fit, 100%, ×; row 2 the canvas and the strip; row 3 the tools.
- Reference: the Mask editor, the paint window and the Crop window have one bar: the tools, then Fit, 100% and close at its end (`paint-window-dark-layers.png`).
- Fix: the view keeps its own Fit and 100% (`zoomButtons: true`) and gains a `trailing` slot at the end of its tools row; the dialog puts its close there and turns its own bar off (a `LargeViewDialog.showBar` property). The view then reads the same in the dialog and in an area.

#### L3. "Keep looking at head" beside Drop reads as "keep the result" (medium-high)

- File: `src/canvas/CanvasView.qml:351-358`.
- Problem: the row reads Accept, Drop, Keep…: "Keep" is the opposite of "Drop" to a reader who stops at the first word, and the button does neither (it shows the head; the result stays a candidate). "Keep" also means cancel in the Projects question (`ProjectsDialog.qml:250`) and "Keep as candidate" is a node input.
- Fix: "Show head" (or "Back to head"), still flat and last.

#### L4. The view's chevrons are text "▾" (low)

- File: `src/canvas/CanvasView.qml:165` and `:189`.
- Reference: the canvas well and the area type menu draw Lucide `chevron-down` after the text (`InputWidget.qml:90`, `AreaHeader.qml:158-163`).
- Fix: a `trailingGlyph` property on `ViewButton` (the glyph after the label), used here.

#### L5. Two canvas menus with different rows (medium)

- File: `src/canvas/CanvasView.qml:115-119` (name, size; nothing else) against `InputWidget.qml:351-371` (name, "size · last change", the id as a second line, a missing id, "No canvases yet", New canvas…, Import picture…).
- Fix: one function for the rows (a `WidgetEdit` or `CanvasViewModel` call, or a shared JS file), used by both; the view's menu gains New canvas… once **C7** exists.

#### L6. The empty states send the person to the command line (medium)

- File: `src/canvas/CanvasView.qml:457` and `src/canvas/CanvasHistoryDialog.qml:152` ("No canvas yet: fugue canvas new, or fugue canvas import, makes one.").
- Reference: the canvas picker offers New canvas… and Import picture… in the app (`InputWidget.qml:364-365`); the picker's own empty text is "No canvases yet" (`:363`), a second wording for the same state.
- Fix: the empty state says "No canvas yet" and shows New canvas… and Import picture… buttons (**C7**), the same words in all three places.

#### L7. Node ids where a person reads a name (medium)

- File: `src/canvas/CanvasView.qml:490` ("Region %1 reads %2" with the node id: the picture shows "Region region reads head"), `:326` (`newResultFrom`, the producer `canvas-write:<node>`, spec 14.5 choice (8)).
- Reference: node faces and menus name a node by its title.
- Fix: the node's title, with the id only when two nodes share it ("Canvas Region (#12)").

#### L8. "Region reads from this revision" is a statement, not a command (low-medium)

- File: `src/canvas/CanvasView.qml:500`.
- Reference: every other button is a verb (Accept, Drop, Export…, Compare). The #221 Decisions row calls it "Use for region".
- Fix: "Use for region", the tooltip unchanged.

#### L9. A third look for a notice with actions (medium)

- File: `src/canvas/CanvasView.qml:303-319`: `Qt.alpha(accent, 0.14)` fill, accent edge, `u/2` (2px) padding.
- Reference: `workflowNotice` (`WorkflowDialogs.qml:105-111`: `popup`, `hairline`, 8px) and `helperNotice`. The accent tint is also the color of a current button and of Accept, so the banner competes with the primary. 2px padding is the tightest box in the app.
- Fix: one `Banner` component (**C8**): `popup` fill, `hairline` edge, 2u padding, an optional 3px accent bar at the start for "needs a decision". The alpha values (0.1, 0.14, 0.2, 0.22, 0.5 across five files) become one `look.accentWash` token.

#### L10. The padded box is drawn two ways (low)

- File: `src/canvas/CanvasView.qml:431-445` (accent dashes only) against `RegionDialog.qml:157-178` (accent dashes on a 3px dark halo, so it reads on a light or accent-colored picture).
- Fix: one `ContextBox` item (**C9**) with the halo, used by both.

#### L11. The view's empty-state error is in the label color (low)

- File: `src/canvas/CanvasView.qml:459` (`canvasData.error` in `look.label`).
- Reference: `RegionDialog.qml:188` and the History error line (`CanvasHistoryDialog.qml:299`), both `look.error`.
- Fix: the error in `look.error`, the other empty texts in `look.label`.

#### L12. The tools row's chunks are uneven (low)

- File: `src/canvas/CanvasView.qml:170` and `:220` (spacer `u`: 3u between chunks) against `:267` and `:280` (spacer 2u: 4u); Snap, a region tool, sits 1u after the compare modes (`:257`).
- Rule: Gaps. Reference: the Mask editor's bar, one chunk gap throughout.
- Fix: one chunk gap (4u) everywhere in the bar; Snap in its own chunk after the compare chunk.

#### L13. Three forked lists of compare modes, and the canvas ones lack one (medium)

- File: `CompareDialog.qml:37-39` (five: Slider, Slider horizontal, Side by side, Flip, Difference), `CanvasHistoryDialog.qml:87-88` and `CanvasView.qml:242-243` (four: no Slider horizontal). The spec and the #221 row call them "the four compare modes of Compare Images", which has five.
- Fix: one `CompareModeButtons` component (**C4**), the same five everywhere, the mode remembered across them.

#### L14. The same Snap tip in two wordings (low)

- File: `CanvasView.qml:263` ("with a ratio held") and `RegionDialog.qml:121` ("with a ratio lock"). Fix: one string.

#### L15. Revision names differ between the strip, the picture label and History (low)

- File: `CanvasView.qml:178` and `:391-393` ("Candidate 82d368f5", "Head", a bare hash, "A", "B"); `CanvasHistoryDialog.qml:212` ("82d368f5  B") and `:230` (tags "head", "candidate", "dropped", lowercase).
- Fix: one formatter (**C2**) for the name of a revision and its tag; the case follows **D3**.

#### L16. The banner puts Drop in the middle (low)

- File: `CanvasView.qml:333-358`: Accept, Drop, Show head; 4u gaps. The bar puts Drop last with 6u before it (`:290`).
- Fix: Accept, Show head, then Drop last, with the bar's gap: the order of every other row with a destructive control.

#### L17. The banner pushes the picture down when it appears (low)

- File: `CanvasView.qml:306-310` and `:376` (the picture is anchored under the banner).
- Problem: a result arriving moves the picture by the banner's height under a pointer that may be aiming at it (the drag lock covers a drag in progress, not a hover or a press about to start).
- Fix: the banner over the top of the picture (as the size notice is), or the picture keeps its place and the banner takes the notes' place under it.

#### L18. Large views and questions do not mirror right-to-left; Settings and Projects do (medium)

- File: `src/canvas/LargeViewDialog.qml:92-190` and `src/canvas/Question.qml:6-16` (no `LayoutMirroring`); popups live in `Overlay.overlay`, outside the window's mirrored root (`Main.qml:95-99`).
- Reference: `SettingsPanel.qml:71-72`, `ProjectsDialog.qml:100-101`, `GroupDialogs.qml:73-74`, `CommandUi.qml:82-83`.
- Effect on cohesion: the same `CanvasView` mirrors in an area (inside the window root) and not in the File > Canvas… dialog.
- Fix: mirror the bar and the footer of `LargeViewDialog` and the content of `Question`; keep the picture unmirrored, as `FugueCanvas.qml:363-364` does. After the layers panel, because it changes the Canvas view's bar.

### Decide (owner)

#### D1. Two windows for one job: Canvas history and the Canvas view (high)

The two show revisions, compare, export, accept and drop, with different layouts (a list column against a strip of chips) and **different meanings for the same button**: Compare on a candidate is against its parent in History (`CanvasHistoryDialog.qml:82`) and against the head in the view (`CanvasView.qml:236`). History alone lists dropped candidates and the canvases of the store; the view alone has the overlay, the region and the live refresh. Options: (a) retire History into the view (dropped candidates in Earlier, a "Show dropped" row), File > Canvas history… removed; (b) keep both, with History renamed to what it is now (an audit list) and its Compare matching the view. Recommendation: (a), high confidence: one place to decide on a candidate, one meaning of Compare.

**Decided** (coordinator for the owner, 2026-11-22, high): retire Canvas history into the Canvas view later; History stays until the view does everything it does (the dropped candidates, the list of canvases). One meaning of Compare from now on: a candidate is compared with the head, as in the view.

#### D2. File > Canvas… once areas host the view

The dialog is the stop-gap "until the areas can host it" (`CanvasViewDialog.qml` header). Areas host it now (View > Areas > Canvas). Keep both entries, or make File > Canvas… split the window and show a Canvas area? Recommendation: keep the dialog for the one-area window (hobbyists) and leave it as is; revisit after workspaces 2.

**Decided** (coordinator for the owner, 2026-11-22, high): keep File > Canvas… for the one-area window.

#### D3. The user's words: candidate or result; Head or head; status-word case

- The banner and the setting say **result** ("New result…", "Show new canvas results", `CanvasView.qml:326`, `SettingsPanel.qml:257`); the strip, the bar, History, the setting's own note, the spec and `fugue canvas candidates` say **candidate**; the template and the default layer name say **Fill**. Recommendation: **candidate** in every canvas surface ("New candidate 82d368f5 from Canvas Write", "Show new candidates"), "fill" only for the template.
- **Head** is capitalized on a chip and lowercase in sentences and tags. Recommendation: a chip or label is a name, capitalized; inside a sentence lowercase.
- Status words: "Current" (capital) beside "missing" and "invalid" (lowercase) in one Projects row (`ProjectsDialog.qml:174`, `:182`), History tags lowercase (`:230`), the picker's "not in this root" lowercase. Recommendation: status chips capitalized, as "Current" and "Inherited from Default".

**Decided** (coordinator for the owner, 2026-11-22, high): **candidate** for a pending run result in every surface; **Head** capitalized as a name in UI text; status words capitalized (Current, Missing, Invalid, Candidate, Dropped).

#### D4. Three button families of the same size

`ViewButton` (pictures and toolbars: no edge, 0.4 when disabled, 2px focus ring, no hover color), `SettingsParts.SmallButton` (settings and Projects: a hairline edge, 0.5 disabled, 1px accent edge on focus, `selectedRow` hover) and `AreaHeader`'s private `BarButton` (24px, transparent, `menuRow` hover). Both large ones are 28px and do the same job; the screenshots show bordered buttons in Projects and borderless ones in the Canvas view. Options: one component with `flat`, `compact`, `danger`, `primary` and a hover state, or a written split (pictures: borderless; forms: bordered). Recommendation: one component (**C3**), medium confidence (it touches every dialog's look).

**Decided** (coordinator for the owner, 2026-11-22, medium-high): one button component with variants (primary, toggle, quiet, danger); `SmallButton` and `BarButton` stay as thin aliases so names and objectNames do not move. Not done in the fix-now batch: a refactor of every dialog's buttons of its own.

#### D5. Two keymaps for fit and zoom in one window

The Graph fits on `.` and Home and zooms on Ctrl+= (`default_commands.cpp`, `view.fit`, `view.zoom-in`); every picture view fits on `0`, shows 100% on `1` and zooms on `+` (`LargeViewDialog.qml:131-134`, `CanvasView.qml:147-150`). With areas both views sit side by side. Blender fits on Home; Photoshop on Ctrl+0 and 100% on Ctrl+1. The Compare Images keys (`\` flip, `M` next mode, `menu_commands.cpp:90-93`) do not reach the compare in History or the view; Accept and Drop have no keys anywhere. Recommendation: Home fits in every view (keep `.` and `0` as second keys), `1` stays 100%; the compare keys work wherever a compare is shown; Enter accepts a shown candidate (the `LargeViewDialog.confirm` the Canvas view does not connect). Check first that a focused Canvas area does not hand `Q` (quit), `.` and Escape to the window's shortcuts (not tested; see **Not checked**).

**Decided** (coordinator for the owner, 2026-11-22, medium): one fit and zoom keymap in the pictures and the Graph: Home fits, 1 is 100%, + and - zoom; Enter accepts where Accept is the surface's primary and the focus is not in a text field. Not done in the fix-now batch (the Canvas view and the Graph keymap, spec 12.4, change with it).

#### D6. A rectangle edited in two places with two commit models

The region picker (`RegionDialog.qml`: ratio buttons, four fields, Apply) and the view's frame (`CanvasView.qml:407-430`: live, one undo step, no ratios, no fields) edit the same four numbers; Pick region… in the canvas menu (`InputWidget.qml:367-370`) opens the first while the Canvas view offers the second. Spec 14.5 choices (4) and (5) keep both on purpose. Options: retire the picker and add the ratio buttons and the fields to the view; or keep the picker for exact work. Recommendation: retire it once the view has ratios (medium).

**Decided** (coordinator for the owner, 2026-11-22, medium): the region picker becomes redundant once the Canvas view edits live; keep it for now and retire it later. The commit model is live with one undo step.

#### D7. A toggle that is on looks like the primary

`ViewButton` fills `current && accented` and `primary` with the same accent (`ViewButton.qml:25`, `:33`). In `canvas-view-candidate.png` Show candidate, the shown chip "Candidate 82d368f5" and Accept are the same yellow; History's Compare and mode buttons likewise. The Mask editor's current tool and its OK share the accent too (an existing case). Recommendation: on a surface with a primary, toggles and choices use the quiet marking (`accented: false`, `selectedRow`), as Fit and 100% already do; the accent belongs to the primary alone.

**Decided** (coordinator for the owner, 2026-11-22, high): a toggle that is on uses a quiet marking, never the primary's accent fill, on any surface that has a primary. As built in the fix-now batch: `ViewButton`'s existing quiet marking (`accented: false`: the `selectedRow` fill, text color), the marking Fit and 100% already use. The coordinator's note said "inset and label color"; the label color on an on toggle reads dimmer than an off one, so the text color stays (to confirm).

#### D8. Which answer of a destructive question is highlighted

Projects highlights the destructive "Remove from list" (`ProjectsDialog.qml:253-256`); the delete-group question highlights Cancel (#156); the Cache question highlights Clear (#141). Recommendation: the safe answer is highlighted when the loss cannot be undone (delete group), the action when it can (Remove from list, Clear previews); write it as a rule.

**Decided** (coordinator for the owner, 2026-11-22, medium): a destructive question highlights the safe answer (Cancel, Keep); the destructive answer is never the default.

#### D9. The order of a form's buttons

Projects' New form: Create and open (highlighted), Create, Back, primary **first** (`ProjectsDialog.qml:369-391`); Project settings: Save, then Back; the unsaved question: Open in new window, then Cancel. Every other dialog: Cancel, then the highlighted answer **last at the right** (Crop, Mask, color picker #122, questions). Recommendation: Back, Create, Create and open, right-aligned.

**Decided** (coordinator for the owner, 2026-11-22, high): the primary last at the right everywhere; Projects' Create and open moves there.

#### D10. "Canvas" still means the graph in user-facing text

Now that a Canvas is a picture and the node view is the Graph (`view_registry.cpp:13`, `:17`): Settings > General "Canvas navigation" and "Left-drag on empty canvas…" (`SettingsPanel.qml:160-166`), "The canvas corners" (`:431`) mean the graph. In code `canvas` stays the graph (`FugueCanvas`, `App.canvasModel`); that is a code name, not this question. Recommendation: the user-facing words become "Graph navigation", "empty graph", "node corners".

**Decided** (coordinator for the owner, 2026-11-22, high): user-facing "canvas" meaning the node graph becomes "graph" in Settings and the docs' wording; code names stay.

#### D11. The Graph's glyph is the Workflows panel's

The Graph view type's glyph is `workflow` (`view_registry.cpp:13`), the glyph of the Workflows side tab (#159); the Canvas view's `image` is the selection toolbar's Open image. Recommendation: a glyph of its own for the Graph (`git-fork` or `network` added to the subset).

**Decided** (coordinator for the owner, 2026-11-22, high): the Graph view gets a glyph of its own, apart from the Workflows panel tab's.

## Status

2026-11-22, first batch (commits `d75a7b18` to `7e587afa` on `cohesion-audit`; the Decisions row "cohesion fixes, first batch" in docs/ui-ergonomics.md lists the controls):

- **Fixed:** N1 to N15. N9 kept the menu's "Import picture…" (the CLI's word) and renamed only the file dialog. N12 fixed the menu bar's ring; the SmallButton and Toggle rings wait for D4. N10 is a `WidgetMenu` `separator` entry, not a heading.
- **Decisions built:** D1 (History compares a candidate with the head; History itself stays until the view does everything it does), D3 (History's tags, Projects' status words, the setting "Show new candidates"), D7 (History's Compare and modes, the region picker's Snap and ratios, the Crop window's ratios), D8 (Projects' remove question, the Cache question; the delete-group question already did), D9 (Projects' forms), D10 (Settings and the project's Preferences word), D11 (`waypoints`).
- **Left for the Canvas view's batch** (`CanvasView.qml` and `canvas_view_model.*` are being rewritten): L1 to L18; D3 in the banner and in docs/canvas.md ("New result…", "Keep looking at head"); D7 in the view's toggles and strip; D5 (one keymap, with the Graph's in spec 12.4); D4 (one button component); D6 (retire the region picker once the view has ratios and fields); D7 in the Mask editor's tools and the paint panel's eye (left alone while the layers work is open); D10 in the spec's own wording (section 12 still says "canvas" for the graph throughout).

2026-11-22, second batch (branch `cohesion-canvas-view`, after the layers panel #226 merged; the Decisions row "cohesion fixes, second batch" in docs/ui-ergonomics.md lists the controls):

- **Fixed:** L1 (the bar's Accept and Drop hide while the banner shows), L2 (one bar: `LargeViewDialog.showBar`, the view's `trailing` slot), L3 and L16 (**Back to Head** first, Accept, Drop last and apart), L4 (`ViewButton.trailingGlyph`, Lucide `chevron-down`; also the layers panel's Blend), L5 (`canvasMenu.js`, the picker's rows in the view's menu, with New canvas… and Import picture…), L6 (`NewCanvasQuestion.qml`, `CanvasHistory::newCanvas`/`importPicture`: "No canvases yet" and the two buttons in the view and History), L7 (`CanvasModel::nodeName`: the region line and the banner name the node by its title), L8 ("Use for region"), L9 (`Banner.qml`, the notice look with a 3px accent bar; the layers' solo notice too), L10 (`ContextBox.qml`, the halo in both), L11 (the empty-state error in `look.error`), L12 (one chunk gap, 4u; Snap in its own chunk), L13 (`CompareModeButtons.qml`: the five modes everywhere, the last canvas pick remembered, `compareMemory.js`), L14 (one Snap tip), L15 (`CanvasHistory` rows' `name` and `tag`: one formatter), L17 (the banner over the picture's foot: the picture never moves), L18 (large views' bar and footer, questions and the Canvas view mirror; pictures never).
- **Decisions built:** D3 in the view and docs/canvas.md (candidate, Head capitalized as a name, "Not in this root"); D4 (one `ViewButton` with `bordered`, `flat`, `compact`; `SmallButton` and `BarButton` thin aliases; the SmallButton and Toggle focus rings: SmallButton's is now the 2px ring, the Toggle's track edge is not changed); D5 (Home fits every picture and the Graph, `+` and `-` zoom the Graph too, Enter accepts where Accept is the primary; checked first, offscreen: a focused Canvas area handed `.`, Home, Delete, Ctrl+A and the bare Q to the window, and now keeps them, `CommandRegistry::ownerOfPress`/`actsOnGraph`); D7 in the view's toggles and strip, the Mask editor's tools and Mask alone, the paint and layers panels' eyes. D6: the region picker stays.
- **Owner reports in the same batch:** a drag on the picture draws without a selected node (the view follows the only Region node of the canvas, else the last chosen; with several it asks which, with none it offers Add a Canvas Region node); a Region node with no canvas takes the one shown at its first edit, and a node on another canvas or an unknown one is explained with Show its canvas and Use this canvas for it. The layers panel's read-only state says why by case with Go to Head (and Accept and Drop for a candidate). The owner's decision that a run lands on the Head: the view follows it with a "Layer … added" notice and Undo; the banner is for real candidates only, without its hint line. The owner then removed the strip: no revision chips and no hashes in the default view; a History panel (steps in words, newest first, All revisions for the candidates) and a `N pending` chip replace it; L15's formatter now serves the History dialog, and the view names steps in words.
- **Seen in the new pictures and fixed:** the Canvas area drew nothing behind the view (black in the light theme) and had no inset; the area header's close sat in the danger tint at rest; the canvas menu was too narrow for a name beside its size.
- **Left:** `look.accentWash` (the remaining accent tints are value fills and mask tints, other jobs); the menu bar's close and `RowTrashButton` as `ViewButton` variants (C3); C5 (one Export button and file dialog for History and the view); C10 (one rectangle footer for Crop and Region); the compare keys (`\`, M) in the canvas compares; History retired into the view (D1); D10 in the spec's section 12 wording; a narrow Canvas area (the layers panel takes half of a small area); the run toolbar floats over the foot of a short Canvas area (it covered Add a Canvas Region node in a test window); whether a disabled `ViewButton` shows its tooltip at all (Qt does not deliver hover to a disabled item, so the tips that say why a button is disabled are likely never seen; not checked offscreen).

## The same action in two places

| Action | Where | Same? |
| --- | --- | --- |
| Accept, Drop | History bar `:117-135`, view bar `:281-299`, banner `:333-349` | Words and calls the same; tips differ ("candidate" against "result"); the banner's order and gaps differ (L16); bar and banner both shown (L1) |
| Compare | History `:76-85`, view `:230-240` | Same label, different B side for a candidate (D1) |
| Compare modes | Compare Images, History, view | Five against four (L13) |
| Export… | History `:103-115`, view `:268-279` | Same; the code and the `FileDialog` are copied (C5); History hides it without a canvas, the view shows it disabled |
| Choose a canvas | picker menu, view menu, History list | Three lists, three sets of rows (L5); History's has no keyboard (N6) |
| Make a canvas | picker menu and question; empty states | The empty states name the CLI (L6); three names for the picture path (N9) |
| Edit the region | picker dialog, view frame | Two commit models (D6); halo on one box only (L10); one Snap tip in two wordings (L14) |
| Close | menu bar, area header, side panel, large view, tab, helper notice | Five looks: 40 × 27 Lucide 16px, 24px Lucide in a transparent square, 28px `ViewButton` glyph, 28px `ViewButton` text "×", Material "×" (N1, D4) |
| Menu chevron | well, area type menu, view bar | Lucide twice, text "▾" once (L4) |
| Focus | most buttons, menu bar close, settings | 2px ring, 1px inset ring, 1px edge (N12) |
| Disabled | `ViewButton`, `SmallButton`, `ThemedButton` | 0.4, 0.5, Material's (D4) |

## Components worth extracting

Each makes one of the findings above impossible to repeat.

- **C1. Decision bar**: Accept (`primary`) and Drop (`danger`, its chunk last) for a candidate, with the tips in one place. Used by the History bar, the view bar and the banner (L1, L16).
- **C2. Revision label**: one formatter for the name of a revision (Head, Candidate <hash>, <hash>) and its tag word and color; used by the strip chips, the picture label, History rows and the Earlier menu (L15, D3).
- **C3. One button**: `ViewButton` with `flat` (transparent, `menuRow` hover), `compact` (24px), `trailingGlyph`, `checkable`, and a hover state; replaces `BarButton`, the menu bar close, `RowTrashButton` and, after D4, `SmallButton` (N3, N12, N13, L4, D4).
- **C4. Compare mode buttons**: the five modes and the remembered mode (L13).
- **C5. Export revision button**: the button, the `FileDialog` and the Pictures default (History and view share ~20 copied lines).
- **C6. Canvas menu rows**: one function for the rows of a canvas menu (L5).
- **C7. New canvas dialog** apart from `WidgetPicker` (today it needs a node input to write to): a canvas is made and returned, the caller decides what to do with it (the picker sets its input, an empty state opens it) (L6).
- **C8. Banner**: the notice-with-actions look of `workflowNotice`, `helperNotice` and the canvas banner (L9).
- **C9. Context box**: the dashed padded box with its halo (L10).
- **C10. Rectangle footer and ratio bar**: the X, Y, Width, Height fields and the ratio buttons that `CropDialog.qml:60-143` and `RegionDialog.qml:94-130`, `:193-272` copy line for line.
- Tokens: `look.accentWash` (the accent tints), `look.iconSize` (14, typed as a literal in `ViewButton.qml:51`, `Glyph.qml:12`, `SettingsParts.qml:130` as 12, `MenuBar.qml:307` as 16), and `look.onPicture` / `look.onPictureHalo` (the white and the dark used by `CropFrame.qml:79-133` and `RegionDialog.qml:158` over any picture).

## Pictures

Run offscreen (`QT_QPA_PLATFORM=offscreen`, `FUGUE_SHOT_DIR=<dir>`) from a build of this commit (the second batch adds, dark and light: `test_canvas_view_window savesThePicturesOfTheWindow` as `canvas-view-<theme>-candidate|banner|menu|new-canvas|empty.png`, `test_canvas_history_window savesThePictures`, `test_region_picker savesThePictures`, `test_area_split_window savesThePictures`): `test_canvas_view_window savesThePicturesOfTheWindow`, `test_projects_window savesThePicturesOfTheWindow`, `test_chrome_window picturesOfTheBarOutAndIn`, `test_canvas_mask_editor savesTheEditorOnLightAndDarkThemes`, `test_canvas_paint_node savesTheWindowOnLightAndDarkThemes`. All passed. Looked at:

- `canvas-view-candidate.png` (dark, 960 × 708): three button rows (L2); Show candidate, the shown chip and Accept in the same accent (D7); "Region region reads head" (L7); the dialog's text × beside the bar's Lucide × (N1); four lines of notes under the picture at one size and color, readable.
- `projects-list.png`, `projects-settings-general.png`: bordered `SmallButton`s against the Canvas view's borderless ones (D4); "missing" beside "Current" (D3); the two new General toggles match Magnify's row, spacing and On/Off label.
- `menubar-in.png`, `menubar-out.png`: the bar and its handle; the chip and close are not in these pictures (the fixture has no project).
- `mask-editor-*-overlay.png`, `paint-window-*-layers.png` (light and dark): the reference one-bar layout, glyph tool buttons, danger Clear at the end of the tools.

## Not checked

- **Light theme** of every new surface: the only picture of a new surface is the Canvas view, dark. No test saves a picture of History, the banner, the area headers and borders, the view-type menu, the Region dialog or the New canvas question. Worth adding: one `savesThePictures…` per surface, light and dark (the Mask editor test's `_data` rows are the pattern).
- Hover and pressed looks on a real pointer (offscreen pictures show rest states only; the rule is never to move the real pointer).
- Right-to-left rendering (read from source only, L18); whether `CompareView` and the region frame stay correct when an area mirrors them.
- Narrow areas: a Canvas area at its 240 × 180 minimum (the strip and tools wrap to several rows and may leave no picture).
- Whether window shortcuts (`Q` quits, `.` fits the Graph, Escape cancels a run) fire while a Canvas area has the focus (D5).
- Screen-reader output; Tab order beyond reading `activeFocusOnTab` in the source.
- The layers panel (not merged).

## Coverage

9 surfaces, from source on `eb3e7e73`, against 8 reference surfaces; 5 picture tests run and looked at (dark Canvas view only for the new surfaces). Findings: 15 fix now, 18 after the layers panel, 11 for the owner.

2026-11-22, #235 (the owner's report that the quit question looks different): `ThemedButton` was the last Material button family. It is now a `ViewButton` alias; the quit question is a `Question` with the safe answer highlighted and focused; `QuestionAnswers` is the one answer row; Discard in the close-tab and recover questions is tinted. The Decisions row "Question dialogs" lists the files; pictures from `test_question_dialogs`.
