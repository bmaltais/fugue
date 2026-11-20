# Window placement

Placement decisions for the window that are not in [canvas-look.md](canvas-look.md) or [spec.md](spec.md) yet. Those two win when they disagree with this file.

A dated audit under `.ui-ergonomics/` is the record of one pass. This file is the list a window edit keeps current.

## On a window edit

1. Read **Open** and **Not scheduled**, and the canvas-look or spec section for the control you are changing.
2. An **Open** row is unfinished work on that control. When this change is already in that code, finish it and move the row to **Decisions** with the date.
3. A **Not scheduled** row stays until the owner lifts it. Lifting it means moving the row to **Open** before any edit.
4. A placement this file does not mention follows the `ui-ergonomics` skill (`references/constraints.md`, `motor.md`, `search.md`, `grouping.md`). Record it here in this change: control, file, old value, new value, why.
5. When a decision belongs in the look or the spec, write it there and replace the row here with a pointer to that section.

Done when every control this change moves is a **Decisions** row or an **Open** row, and any **Open** row this change finished has moved to **Decisions**.

## Open

### Port loses to the resize corner

- File: `src/canvas/FugueCanvas.qml` (`cornerAt` runs before `portUnder`)
- Written rule: canvas-look §4 and spec Appendix A. A port inside the corner square starts a wire.
- Acceptance: at zoom 1, a left press at a port center that lies inside the corner square starts a link and does not call `beginResize`. A press in that square with no port still resizes.

## Not scheduled

Left by the 2026-11-20 pass. Each stays here until the owner lifts it.

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
- Measured from source at text size 12. Run stays Ctrl+Enter.

## Unread

Settings, the shortcuts overlay, the quit question, About, every menu except Run, the context menus, the workflow dialogs and their questions (those questions are a row of buttons), the group dialogs, image save, the widget pickers, the magnifier.
