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
- New: the files that fit the picked diffusion model first, under their folder; then an "Other" heading with the rest, those that do not fit in the label color (`dim`), still pickable.
- Rule: Gaps (one heading per chunk, as the folders already are). Posture: no new decision; the likely pick is the first row once the files' headers are read, at once on a later open, a few ms after the first.

### 2026-11-20 what an extra model folder holds (#86)

- Control: under each Extra model folder, `src/settings/PathsSetting.qml` (`model_paths-holds-<n>-<kind>`), a row of choices (`Choices.qml`, the settings' segmented buttons), then `model_paths-found-<n>`, what its files say.
- Old: a path and its remove button.
- New: the path, then its choices (Found by its files, ComfyUI layout, LoRAs, VAEs, Diffusion models, Text encoders, Checkpoints), then "Found: LoRAs" in the label color while the choice is Found by its files.
- Rule: Floor (each choice is a SmallButton, 7u = 28px tall). Gaps (one unit inside the folder's chunk, as the folders' list keeps). Posture: the default needs no decision; the choices are there for a wrong guess.

## Unread

Settings, the shortcuts overlay, the quit question, About, every menu except Run, the context menus, the workflow dialogs and their questions (those questions are a row of buttons), the group dialogs, image save, the widget pickers (but their search field), the magnifier.
