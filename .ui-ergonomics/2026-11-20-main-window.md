# Ergonomics spec: main window

Date: 2026-11-20. Project: `~/github/fugue` (checkout on `component-nodes` at `534d782`, same commit as `main`).

Sizes are device-independent pixels at `textSize` 12, where `look.unit` (`u`) is `textSize / 3` = 4 (`src/look.cpp`). Measurement is **from source** (layout properties). Nothing in this file was read off a rendered window. One font figure was measured with `QFontMetrics` on `QGuiApplication::font()` (Adwaita Sans on this machine) and is marked that way.

Interface notes: `docs/canvas-look.md` (units, node anatomy, furniture, search), `docs/spec.md` §12 and Appendix A (keymap, “the main action is Run on Ctrl+Enter”, “a port inside a corner’s square wins”). No `AGENTS.md`.

## Scope

| Surface | Path | Purpose | Posture | Input | Reach |
| --- | --- | --- | --- | --- | --- |
| Main window | `src/Main.qml` | Assemble a workflow and run it (README; spec 12.3) | sovereign | pointer | resting |
| Run menu | `src/menu/menus.cpp` `bar()` Run submenu, drawn by `src/menu/CommandMenu.qml` | Pointer method for Run | transient | pointer | resting |
| Node search | `src/search/NodeSearchPopup.qml` | Add a node (spec 12.8). Opened by double-click on empty canvas, Tab, Shift+A, or a link dropped on empty canvas | transient | mixed (keyboard to accept, pointer to pick a row) | resting |

The primary task is Run. Node search is the dialog the assemble step opens. The Run menu is the pointer method of Run.

## Tasks

### Run the open workflow

- Frequency: **counted**. Spec 12.3 names it the main action. `run.start` is the first row of `src/commands/default_commands.cpp`. Binding `Ctrl+Return`.
- Methods:
  1. Keys: `Ctrl+Return` fires `run.start` while a text field is focused too (12.4 gives the field only bare letters, Shift letters, and Ctrl+A/C/V/X/Z/Y).
  2. Pointer: menu title “Run”, then the first row “Run”.
- Origin: after a prompt edit, the pointer is on that well and the hand is on the keyboard. From elsewhere, the pointer is wherever the last press was.

Two methods, so KLM (excludes search, errors, novices). K = 0.28 s, P = 1.10 s, B = 0.10 s, BB = 0.20 s, H = 0.40 s, M = 1.35 s. A known chord is one cognitive unit.

```
Method: Ctrl+Enter, hand on the keyboard
Hands start: keyboard
Operators: M K K
Total: 1.91 s
Excluded: search, errors, novice

Method: Ctrl+Enter, hand on the pointer
Hands start: pointer
Operators: H M K K
Total: 2.31 s
Excluded: search, errors, novice

Method: Run menu
Hands start: pointer
Operators: M P BB M P BB
Total: 5.30 s
Excluded: search, errors, novice
```

The shortcut wins. The menu stays as the visible pointer method (sovereign surface, shortcut map already in the `?` overlay).

### Add a node

- Frequency: **assumed**. Spec 12.8 is search-first creation; double-click, Tab and Shift+A all open it. Basis: building a graph is the other half of the product, and no call-site count exists.
- Method: double-click empty canvas, or Tab, or Shift+A; type; Enter accepts the current row. A row click also accepts.
- Origin: the pointer, for a double-click or a dropped link. The popup’s top-left is placed at that point (`NodeSearchPopup.place`).

### Edit a prompt, then run

- Frequency: **assumed**. The example workflow and `fugue/generate-image` lead with a multiline prompt. Basis: a generation session changes the prompt and runs again. Not counted above Run.
- Method: click the prompt well, type, `Ctrl+Return`. Enter in a multiline well is a newline.
- Origin: the well.

### Set a slider

- Frequency: **assumed**, lower than Run. Sliders in the built-in nodes: Strength on Generate Image, Brightness and Contrast, Blend opacity.
- Method today: drag along the well. A click that does not drag opens the text editor (`InputWidget.clicked` default).
- Origin: the well, under the pointer.

### Leave a group, inspect an error, resize, cut a wire

- Frequency: **assumed**, rare next to Run. Basis: each needs a group, a failed run, a size pinch, or a hovered wire.

### Controls on no task

Zoom pill (`objectName: "zoomPill"`) shows a percent and takes no press. Fit and zoom already have keys (`view.fit`, `view.zoom-in`, `view.zoom-out`). Not a missed task.

## Inventory

One row per control kind. Repeaters say so. Visual and target are the press box. `u` = 4.

| Control | Role and name | Source | Visual | Target | Position | Chunk | Order | Nearest |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Menu title | menu item “File”, “Edit”, “View”, “Node”, “Run”, “Help” (`&` stripped) | `src/menu/MenuBar.qml` `titleButton` | height 27 (`7u − 1`); width is the label plus `2.5u` padding each side | same | top edge of the window, from the leading edge, inset `1.5u` | menu titles | File, Edit, View, Node, Run, Help | the next title |
| Menu button | button “Menu” | `MenuBar.qml` `hamburger` | 32 × 27 (`8u` by `7u − 1`) | same | same slot, only when `titles.implicitWidth + 8u > width` | menu titles | the only title | none on the bar |
| Run | menu item “Run” | `menus.cpp` `run.start`; `CommandMenuItem.qml` | height 28 (`7u`); width follows the label and the key | same | first row of the Run menu, under the Run title | Run menu | 1 | “Run, ahead of the queue” |
| Run ahead | menu item “Run, ahead of the queue” | `run.start-first` | height 28 | same | Run menu | Run menu | 2 | Run |
| Run to here (menu) | menu item “Run to the selected node” | `run.to-here` | height 28 | same | Run menu | Run menu | 3 | the separator, then Cancel |
| Cancel the run | menu item “Cancel the run” | `run.cancel` | height 28 | same | Run menu, after a separator | Run menu | 4 | the separator |
| Workflow tab | button, name is the workflow title (“…, unsaved” when dirty) | `src/workflow/WorkflowTabs.qml` `tabButton` | width 176 (`44u`), height 36 (`9u`) | same | under the menu bar; hidden while one workflow is open | tabs | tab order | its close button |
| Close tab | button “Close <title>” | `closeTab-*` | 24 × 24 (`6u`) | same | trailing end of the tab | that tab | after the title | the tab and the next tab |
| New workflow | button “New workflow” | `newTab` | width 36 (`9u`), height of the strip | same | after the last tab | tabs | last | the last close button |
| Empty canvas | canvas (no accessible name) | `FugueCanvas.qml` `canvasInput` | the canvas | the canvas | below the menu bar (and the tabs when shown) | canvas | — | whatever node is under the pointer |
| Node body | node (drag, select); double-click on the header renames, on a group body enters | `FugueCanvas.qml` press path; `CanvasNode.qml` | the node box, default width 240 (`60u`) | the box, after ports and the corner square take their presses | on the canvas | that node | — | its ports |
| Port | port, no accessible name | hit in `FugueCanvas.portUnder`; drawn by `PortDot.qml` | 10, or 14 when wired (2 px body ring) | diameter 20 at zoom 1: radius `max(8, 10 / scale)` world px | centered on the node edge, one per row | that node | row order, inputs then outputs | the next port, 24 px center to center (`rowHeight` 6u) |
| Widget well | the input’s value; accessible name unmeasured | `CanvasNode.qml` `widgetLoader`; `InputWidget.qml` `widgetMouse` | height 20 (row 24 minus `round(0.5u)` top and bottom); width is the node minus `6u` | the well | the row, inset `3u` from the node sides | the label and the well | the label is painted on the well | the next well, 4 px of row padding between wells |
| Paste | button “Paste” | `InputWidget.qml` `pasteImageButton` | width `textSize * 3.6` = 43; height `well − 4` = 16 | same | trailing end of a Load Image file well | that well | after the value | the well, which surrounds it |
| Advanced header | button “Advanced” or “Advanced (n)” | hit `CanvasModel::advancedHeaderAt`; drawn `sectionHeader` | the row: height 24, width of the node | the row (full `rowHeight`) | across the node at `headerTop` | the section | the header, then its rows | the well above and below |
| Enter group | button “Enter group ›” | `CanvasNode.qml` `enterGroupButton` | height `rowHeight − u` = 20; width is the label plus `2u` | same | group card footer, trailing | the footer | after the node count | the node count text, which is not a target |
| Warnings | button, no accessible name set | `CanvasNode.qml` `warningsBadge` | height 14 (default glyph size); width unmeasured | the badge | trailing side of the header | header meta | after the time caption | the caption (not a target) |
| Resize corner | resize handle, no name | `FugueCanvas.cornerAt`; mark `ResizeMark.qml` | the mark is about `2.25u` of stroke; the hit square is `max(4u, 12/scale)` = 16 at zoom 1 | the 16 px square inside each corner | four corners of an open node | that node | — | a port, when the port center lies inside the square |
| Cut handle | button (glyph “x”), no name | `FugueCanvas.qml` `cutHandle` | 18 × 18 | same | midpoint of the hovered wire | the wire | the only control | the two ports at the wire’s ends |
| Selection toolbar button | button, command label, or “colors” / “more” when the entry label is empty | `FugueCanvas.qml` `toolbar-*` | 28 × 28 (`7u`) | same | centered above the selection, or below it when `y` would be under 8 | toolbar | Run to here, Bypass, Colors, Frame, Enter, Open Image, Delete, “…” | the next button, gap `u/2` = 2 |
| Breadcrumb level | button, the level title (not the current level) | `FugueCanvas.qml` `crumb` + `crumbArea` | the text at `labelSize` 11 (font height 12); the pill is 28 tall (`7u`) | the text only | top-left of the canvas, inset `4u`, only inside a group | breadcrumb | Workflow, then each group | the next title; the separator is a chevron, a glyph, and `3u` of spacing |
| Run pill | button only when `errorCount > 0`; the name is the line (“n errors”, “Running …”) | `RunPill.qml` `runPillBox` | height 28 (`7u`); width is the text plus `6u` | the box, and only while errors remain | top-right of the canvas, inset `4u` | run status | the only control | the error list, when open |
| Error row | button, “<node id> <message>” as styled text | `RunPill.qml` delegate under `runErrors` | width 352 (`90u − 2u`); height `implicitHeight + 2u` = 22 for one line (font height 14 at `labelSize` 11, plus 8) | same | under the pill, gap `u` | error list | error order | the next row, Column spacing 0 |
| Drop row | button, the port label | `FugueCanvas.qml` `dropTarget-*` | height 28 (`7u`); width of the list | same | at the dropped wire | drop list | port order | the next row |
| Search field | text field “Search nodes” | `NodeSearchPopup.qml` `searchField` | the line at `searchFieldSize` 15 (font height unmeasured); the row is 44 tall (`11u`) | the text line | top of the popup | the query | the only field | the filter chip, or the first result |
| Clear filter | button, no name (glyph “x”) | `NodeSearchPopup.qml` `clearFilter` | glyph at 12 px | the glyph | trailing end of the filter chip (chip height 24, row height 34) | the filter chip | after the type name | the chip’s text, which is not a target |
| Search result | button, the node name | `NodeSearchPopup.qml` `searchResult` | height 44 (`11u`); width of the list minus `3u` | the row | under the field | the result list | rank order; sections when the query is empty | the next row |

## Constraints

Posture. **Holds.** The window is sovereign. Search and the Run menu are transient. No daemonic surface in scope.

Platform slot. **Holds** for the menu bar: File, Edit, View, Node, Run, Help stay in that order, and the narrow window replaces them with one menu button (`MenuBar.qml`). **n/a** for accept, reject, and destructive roles: none of the three surfaces has a dialog button row.

Learned place. **Holds** for the menu order, the keymap (spec 12.4, ComfyUI where the column says “same”), and the toolbar order (canvas-look §11). **Broken** for the corner versus the port: canvas-look §4 and spec Appendix A say a port inside the corner square wins; `FugueCanvas.qml` tests `cornerAt` before `portUnder`.

Minimum target. Floor 24. Pointer surface, so the touch floor does not apply.

- **Broken:** error rows. One line is 22 tall and the next row starts at once, so a 24 circle centered on one row intersects the next.
- **Broken:** resize corner. The hit square is 16 at zoom 1, and it intersects a port whose center sits inside it. The repair that grows the square breaks the written 4u square, so it is P2. C3 only fixes which control wins the overlap.
- **Holds, essential size:** ports. Diameter 20 at zoom 1. Centers are 24 apart (`rowHeight`), so a 24 circle intersects the next port’s circle. The mark is a connection point on a 6u pitch (canvas-look §5, “hit area at least 20 screen px”). Growing the hit to a 24 diameter would make neighboring hits meet.
- **Holds, circle exception:** widget well (20 tall, wide; the 24 circle sits in the middle of the well and the next well is 4 px past the first), Paste (16 tall, 6 px of well and row padding before the next well), Enter group (20 tall, nothing targetable below the footer), warnings badge (14 tall, header is 28, the first well starts at 30), cut handle (18, alone at the wire midpoint), clear filter (12 glyph inside a 34 px row), search field (the line is centered in a 44 px row), breadcrumb text (the next title is a chevron plus a glyph plus `3u` away).
- **Holds, at or above 24:** menu titles (27), menu rows (28), menu button (32 × 27), tabs (36), close tab (24), new tab (36), toolbar buttons (28), run pill (28), drop rows (28), search rows (44), Advanced header (24).

Destructive. **Broken** for toolbar Delete: it sits in the same row as Run to here and the other commands, gap 2 px (`u / 2`), which is the within gap, not a between gap. Delete is reversible (`edit.undo` on this window). It is not the last toolbar control (“…” is). **Holds** for close tab (it belongs to that tab, and a dirty tab asks). **Holds** for the cut handle (undo on this window, not on the Run path). The node context menu ends with `edit.delete` (`menus.cpp` `node()`); that menu is not a reviewed surface, and moving the item is P1.

Semantic group. **Holds.** File stays in File, Run stays in Run, the toolbar’s node commands stay one chunk. C1 splits only Delete out of that chunk.

Illegal and unused. **Holds.** A toolbar button that cannot run stays visible at opacity 0.4 with its `MouseArea` disabled. A command that is not built is dimmed and described “Not available yet” (`CommandMenuItem.qml`). The zoom pill is unused as a control and is not a missed task (see Tasks).

Same surface. **Holds.** Run is a key on this window. Search opens on this window at the pointer. Cancel is `Escape` and `Ctrl+Alt+Return` while a run is going.

Density and tokens. **Holds.** The scale is `u`. C1 uses `2u` and `u/2`. C4 uses the 2 px accent bar the current tab already uses (`WorkflowTabs.qml`). No new step.

Project notes, recorded above: `docs/canvas-look.md`, `docs/spec.md` §12 and Appendix A.

## Changes

### C1. Separate Delete from the selection toolbar’s other commands

- Surface: main window (sovereign, pointer, resting)
- File: `src/canvas/FugueCanvas.qml:1370`
- Control: button “Delete” (`toolbar-edit.delete`)
- Rule: constraints.md#Destructive
- Current: one `Row`, `spacing: canvas.look.unit / 2`. Order: `run.to-here`, `node.bypass`, `colors`, `frame.make`, `group.enter`, `node.open-image`, `edit.delete`, `more`. Every neighbor gap is 2.
- New: a parent `Row` with `spacing: 0` and three children. First, a `Row` with `spacing: canvas.look.unit / 2` holding every button except `edit.delete` and `more`. Then an `Item` with `width: 2 * canvas.look.unit` and `height: 1`. Then the Delete button, unchanged (28 × 28, opacity 0.4 and `MouseArea.enabled: false` when `entry.enabled` is not true). Then an `Item` with `width: 2 * canvas.look.unit`. Then the “…” button. Order is unchanged.
- Approach: n/a (grouping). The gap before Delete and the gap after it go from 2 to 8. Button size stays 28.
- Search: n/a
- Chunk: commands (Run to here, Bypass, Colors, Frame, Enter, Open Image) keep within gap `u/2`. Delete is its own chunk. “…” is its own chunk. Between gap `2u` = 8, which is four times the within gap. Focus order unchanged: these buttons are not in the tab order.
- Constraint check: learned place holds (same order). Floor holds (28). Platform slot holds. Semantic group holds (the node commands stay together). Undo stays on this window. Density holds (`u` and `u/2` already in the file).
- Acceptance: at `u` = 4, the gap between Delete and the nearest visible command button is 8, the gap between Delete and “…” is 8, and the gap between Bypass and Colors is 2. Delete is still not the last control.

### C2. Give each run-error row a 24px target

- Surface: main window (sovereign, pointer, resting)
- File: `src/canvas/RunPill.qml:112`
- Control: button of the error text (`runErrors` delegate)
- Rule: constraints.md#Minimum target
- Current: `height: entryText.implicitHeight + 2 * pill.u`. At `labelSize` 11 that is 14 + 8 = 22 for one line. The `Column` has no `spacing`, so the next row starts at 22.
- New: `height: Math.max(entryText.implicitHeight + 2 * pill.u, 24)`
- Approach: origin is the center of `runPillBox` (the press that opened the list). Vertical D to the first row’s center goes from 28 (pill center at 14, row center at 28 + 4 + 11) to 30 (row center at 28 + 4 + 12). W along that approach goes from 22 to 24. ID = log2(D/W + 1) goes from log2(28/22 + 1) = 1.18 to log2(30/24 + 1) = 1.17.
- Search: n/a
- Chunk: n/a
- Constraint check: floor holds after the change. The pill’s own box stays 28. Destructive does not apply (the row selects the node). Density holds (a minimum, not a new spacing step).
- Acceptance: with two one-line errors, each delegate’s rendered height is at least 24.

### C3. Let the port win inside a resize corner

- Surface: main window (sovereign, pointer, resting)
- File: `src/canvas/FugueCanvas.qml:447`
- Control: port (hit by `portUnder`) and resize corner (hit by `cornerAt`)
- Rule: the project note in `docs/canvas-look.md` §4 and `docs/spec.md` Appendix A (“a port inside a corner’s square wins over it”), which is a constraint under constraints.md (a rule the project wrote down)
- Current: on left press, `cornerAt` runs first and a hit calls `beginResize` and returns. `portUnder` runs only when the corner square missed.
- New: on left press, call `portUnder` first. When `port.node` is set, keep the existing port branch (`disconnectPort` on Ctrl+Alt, otherwise `beginLinkDrag` / `beginLinksMove`) and return. Call `cornerAt` only when the port missed. The corner square stays `Math.max(4 * model.unit, 12 / world.scale)`.
- Approach: both boxes already cover the corner. Before, a press whose point is inside the 16 px square and inside the port’s 20 px diameter starts a resize (D 0, W 16, the port is not the target). After, that press starts a link (D 0, W 20). ID is 0 either way because the pointer is already there; the change is which target receives it.
- Search: n/a
- Chunk: n/a
- Constraint check: the written port rule holds. Learned place holds (corners stay the resize gesture). The 16 px square is still under the floor; growing it is P2, because that breaks the written 4u square. Semantic group holds.
- Acceptance: at zoom 1, a left press at a port center that also lies inside the corner square calls the link-drag path and does not call `beginResize`. A left press in that square with no port inside it still calls `beginResize`.

### C4. Mark the Run menu title as the one primary control

- Surface: main window (sovereign, pointer, resting)
- File: `src/menu/MenuBar.qml:178`
- Control: menu item “Run” (`menuTitle-*` whose `modelData.items[0].command === "run.start"`)
- Rule: search.md#What a spec may change (one primary action, a distinct treatment)
- Current: every title shares one background: `look.body` while open, a 5% tint while hovered, otherwise transparent. Run is the fifth title.
- New: that title keeps the shared background and adds a `Rectangle`, `anchors.bottom: parent.bottom`, `width: parent.width`, `height: 2`, `color: bar.look.accent`, visible in every state. The other five titles do not get it. Same mark as the current workflow tab (`WorkflowTabs.qml`, height 2, `look.accent`).
- Approach: origin unchanged (the title’s place). D and W unchanged (height stays 27).
- Search: before, Run is one of six titles with one treatment, so the pointer method starts with a scan of six. After, Run is the only title with the accent bar; the other five stay one treatment. The menu still opens with “Run” first, and the row still shows `Ctrl+Enter`.
- Chunk: the six titles stay one chunk. Within gap stays the padding already on each title (`2.5u` each side, titles adjacent). No region added. Focus order unchanged (titles are `Qt.NoFocus`; Alt+letter and F10 stay).
- Constraint check: platform slot holds (Run stays the fifth title). Learned place holds (order and keys unchanged). Floor holds. Density holds (the tab’s existing 2 px bar). The shortcut comparison in Tasks still stands; this does not add a second pointer target.
- Acceptance: the title whose first item is `run.start` has a bottom rectangle of height 2 in `look.accent`, and File, Edit, View, Node, and Help do not.

### C5. Make a slider click set the value

- Surface: main window (sovereign, pointer, resting)
- File: `src/canvas/widgets/InputWidget.qml:248`
- Control: the slider well (`widgetMouse` when `kind === "slider"`)
- Rule: motor.md#Tunnels (a track press sets the value; the drag stops being the common case)
- Current: a release that did not drag calls `clicked`, and `clicked` has no `slider` case, so it calls `openEditor()`. A drag calls `edits.valueAt(nodeId, name, event.x / root.width)` and previews it.
- New: in `clicked`, `case "slider": return edits.setValue(nodeId, name, edits.valueAt(nodeId, name, x / width))`. Add `onDoubleClicked` on `widgetMouse`: when `kind === "slider"`, call `openEditor()`. No new thumb. The well stays the target (20 tall; the circle exception in Constraints still covers it). A 24 px thumb on a 20 px well would meet the next row.
- Approach: origin is the well (the pointer is on it). Before, the common case is a steered drag, length A along the well and width W = 20. After, the common case is a press at that point: D = 0, W = the well’s width along the track, ID = 0. The double-click still opens the editor, after the first release has set the value under the pointer.
- Search: n/a
- Chunk: the label stays painted on the well (one chunk). Focus order unchanged.
- Constraint check: floor exception still holds (no new box). Learned place holds (drag still scrubs). Density holds. Same surface holds (the editor stays on the well).
- Acceptance: a click that moves less than `startDragDistance` on a slider sets the value to `valueAt` of that fraction and does not open the editor. A double-click opens the editor. A drag still previews `valueAt` and finishes on release.

## For a person

### P1. Move Delete off the end of the node context menu

- Surface: node context menu (not reviewed; opened from the toolbar’s “…” and from right-click)
- File: `src/menu/menus.cpp:73`
- Control: menu item “Delete” (`edit.delete`)
- Rule: constraints.md#Destructive (not the last item of a frequent menu)
- Current: `node()` ends with `command("edit.delete")` after a separator
- New: place Delete earlier, with a between gap (the separator) still above it, and put a non-destructive row last so a fast downward move does not land on Delete
- Approach: the last row is where an overshoot along the menu tunnel stops. W of the row stays 28.
- Search: n/a
- Chunk: Delete would leave the end of the menu
- Constraint check: this breaks learned place. Spec 12.4 says the basics match ComfyUI, and this menu was built to that order. Floor, platform slot, and undo would still hold.
- Acceptance: the last enabled row of the node menu is not `edit.delete`, and Delete is still in the menu.

### P2. Grow the resize corner to 24 screen px

- Surface: main window
- File: `src/canvas/FugueCanvas.qml:91`
- Control: resize corner
- Rule: constraints.md#Minimum target
- Current: `Math.max(4 * model.unit, 12 / world.scale)` → 16 at zoom 1
- New: `Math.max(4 * model.unit, 24 / world.scale)` so the on-screen square is at least 24
- Approach: W along either axis from 16 to 24 at zoom 1. At zoom 1 the square also grows past the written 4u, so it covers more of the header.
- Search: n/a
- Chunk: n/a
- Constraint check: this breaks the written square, “4u wide (at least 12 screen px)” in canvas-look §4 and spec Appendix A. C3 would still be required, because a larger square overlaps more ports. Rare next to Run, so sovereign density keeps it at the written size until this is lifted.
- Acceptance: at zoom 1, `resizeCornerAt`’s reach is 24, and a port inside that square still starts a link (C3).

### P3. Replace the selection toolbar with a pie

- Surface: main window
- File: `src/canvas/FugueCanvas.qml:1370`
- Control: the eight toolbar buttons
- Rule: motor.md#Tunnels (a pie has equal short travel)
- Current: one horizontal row, 28 px buttons
- New: a pie of the same eight commands, opened at the pointer
- Approach: each item’s D becomes the pie radius instead of a position along the row
- Search: the user would have to learn the pie
- Chunk: the command set stays one pie
- Constraint check: this breaks learned place. Canvas-look §11 specifies ComfyUI’s selection toolbox, a row of glyphs. The project has no pie. The Fitts gain is real, and so is the unfamiliarity.
- Acceptance: not specified, until the project chooses a pie.

If a later count put Add a node or the prompt edit above Run, drop C4. Search already opens at the pointer with the field focused, and the prompt well is already the target under the pointer, so that flip does not create a new primary control. It only removes the accent bar.

The zoom pill is unused as a control. Fit and the zoom keys cover that task. Leaving it as a status readout does not hide a command.

## Unchanged

- Run by `Ctrl+Return`: the KLM total is 1.91 s from the keyboard against 5.30 s for the menu. Spec 12.3 already makes this the main action. No canvas Run button; the menu is the pointer method, and a second target would not beat the shortcut.
- Run menu rows: Run is first, height 28, and the row shows the key. Cancel follows a separator.
- Menu titles other than Run, and the narrow-window menu button: platform order, height 27 or more.
- Tabs, close (24, on its own tab), and new workflow: floor holds. The strip is hidden with one workflow; New is still `Ctrl+N` on this window.
- Empty canvas and node body: the add-node and move targets. Search opens at the pointer.
- Ports: essential-size exception (Constraints).
- Widget wells, Paste, Enter group, warnings, breadcrumb text, cut handle, search field, clear filter: circle exception (Constraints). A rare control stays at that size.
- Advanced header: the hit is the full 24 px row.
- Drop rows: height 28.
- Toolbar buttons other than the Delete gap: 28 px, glyph inside the target, order fixed by canvas-look §11. “…” is the overflow convention. Run to here is not `run.start`.
- Run pill while a run is going: it is status, not a button (arrow cursor). Cancel is `Escape` (M K = 1.63 s from the keyboard), which beats a point at the top-right pill.
- Search results: height 44, name inside the row, sections when the query is empty, field focused on open. The list is long and the user knows the name, so the field is the method.
- Slider drag: still the precise scrub. C5 only changes the click.
- Node context menu order, aside from P1. Colors submenu: `CommandMenu.qml` sets `overlap: menu.u`, the toolkit’s corridor. Run does not open a submenu.

## Not reviewed

Settings panel (`src/settings/SettingsPanel.qml`). Shortcuts overlay and the quit question (`src/commands/CommandUi.qml`). About (`MenuBar.qml`). File, Edit, View, Node, and Help menus, and the canvas, frame, and node context menus (`menus.cpp`), except the Run menu and the Delete note in P1. Workflow open and save dialogs, the close, trust, and recovery questions (`src/workflow/WorkflowDialogs.qml`). Those questions use a `RowLayout` of `ThemedButton`, not a `DialogButtonBox`. Group questions and the interface editor (`src/canvas/GroupDialogs.qml`). Image save (`src/menu/ImageSaveDialog.qml`). Widget menus and file or color dialogs (`src/canvas/widgets/WidgetPicker.qml`). Magnifier (`lens`). Per-node widgets other than the well, Paste, and the slider. Packs and providers pages.

## Coverage

3 surfaces (main window, Run menu, node search); pointer, and mixed on search; resting; from source; assumed frequencies: add a node, edit a prompt, set a slider, leave a group, inspect an error, resize, cut a wire. Run is counted.
